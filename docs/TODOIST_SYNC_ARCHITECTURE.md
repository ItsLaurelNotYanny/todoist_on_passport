# Todoist on BlackBerry 10 Passport - Architecture & Feasibility Specification


---

## 1. Executive Summary & Objective

This specification defines the engineering architecture for a native, offline-first Todoist client on the BlackBerry 10 Passport.

### Core Goals
1. **Target User Experience**: A native Cascades GUI application tailored for the 1:1 aspect ratio (1440×1440) screen, supporting smooth keyboard navigation and touch gestures.
2. **Reliable Two-Way Synchronization**: True offline-first operation backed by a local transactional SQLite store, synchronizing with the unified Todoist API via the delta `/api/v1/sync` endpoint.
3. **Data Integrity Guarantee**: Formal transaction boundaries, command deduplication via persistent UUIDs, temporary-to-permanent ID reconciliation, and deterministic conflict resolution.
4. **Feasible Delivery on Legacy Platform**: Explicit handling of BlackBerry 10 (QNX Neutrino 6.6) toolchain constraints, expired system root CAs, and TLS 1.2/1.3 protocol negotiation realities.

---

## 2. API Selection: Unified Todoist API v1 & Sync Endpoint

### 2.1 Sunset of Legacy APIs
* **Context**: Todoist officially sunset REST API v2 and Sync API v9 on February 10, 2026.
* **Decision**: All communications must target the unified **Todoist API v1** (`https://api.todoist.com/api/v1/`).

### 2.2 Endpoint Decision: `/api/v1/sync` vs REST Endpoints
While Todoist API v1 provides individual REST resources, this offline-first client selects **`/api/v1/sync`** as the primary synchronization backbone for the following architectural reasons:

| Capability | Individual REST Endpoints | `/api/v1/sync` (Selected) |
| :--- | :--- | :--- |
| **Delta / Incremental Fetch** | Not natively supported across all entities; requires polling modified timestamps. | Native `sync_token` cursor returns only changed entities. |
| **Batch Write Operations** | Requires multiple HTTP round trips per action. | Single HTTP POST can dispatch an atomic batch of commands. |
| **Offline Command Deduplication**| Manual idempotent tokens vary by endpoint. | Native `uuid` parameter per command guarantees server-side deduplication on retry. |
| **Temporary ID Resolution** | Client must wait for server response before creating dependent tasks/items. | Server automatically resolves client-generated `temp_id` to server IDs via `temp_id_mapping`. |
| **Bandwidth & RTT Efficiency** | High latency on 3G/4G with multiple round trips. | Optimal: 1 HTTP round-trip performs full pull + push cycle. |

---

## 3. Hardware Capability & TLS Feasibility Evaluation

### 3.1 Hardware Profile & Cryptographic Workload
* **Target Device**: BlackBerry Passport (Model SQW100-1)
* **SoC / Architecture**: Qualcomm Snapdragon 801 (MSM8974AA v3), Quad-core Krait 400 @ 2.26 GHz, ARMv7-A with NEON SIMD engines.
* **System Memory**: 3 GB LPDDR3.
* **Workload Characteristics**: A typical `/api/v1/sync` payload spans 10 KB to 300 KB of JSON data.
* **Theoretical Crypto Throughput**:
  * On a 2.26 GHz Krait 400 core, pure software/NEON AES-128-GCM or ChaCha20-Poly1305 achieves approximately 50–120 MB/s.
  * Symmetric encryption/decryption of a 100 KB payload consumes approximately 1–3 ms of CPU time.
  * **Latency Reality**: Network round-trip time (RTT) over mobile data (80–250 ms) completely dominates processing time. Crypto computation on Snapdragon 801 is negligible.
  * Session resumption (TLS session tickets / IDs) reduces subsequent handshakes to 1 RTT, while HTTP Keep-Alive eliminates repeated handshakes over an open connection.

### 3.2 The Real TLS Challenge on BlackBerry 10
The obstacle on BB10 OS 10.3.3 is **not CPU throughput**, but **obsolete platform SSL infrastructure**:

1. **Qt 4.8.x Limitations**:
   * Upstream Qt 4.8's `QSsl::SslProtocol` lacks a native `TlsV1_2` enum. Upstream `QSsl::TlsV1` invokes OpenSSL's `TLSv1_client_method()`, which negotiates only TLS 1.0.
   * Modern endpoints (`api.todoist.com`) enforce TLS 1.2 or TLS 1.3 with SNI (Server Name Indication).
2. **Expired System Trust Stores**:
   * The built-in QNX CA certificates (`/etc/ssl/certs` or system keystore) are obsolete (e.g., Let's Encrypt DST Root CA X3 expiration).
3. **BlackBerry OS Patches vs Standalone Library**:
   * Whether BB10's proprietary patched Qt 4.8 enables TLS 1.2 via `QSsl::AnyProtocol` or custom socket flags must be proven empirically on hardware.

### 3.3 Network Transport Architecture: Primary vs Fallback

```
+---------------------------------------------------------------------------------+
|                                 Network Layer                                   |
+---------------------------------------------------------------------------------+
                                         |
                       [Attempt Primary Direct Channel]
                                         |
               +-------------------------+-------------------------+
               |                                                   |
      [Direct Channel A: Native libcurl]                  [Direct Channel B: Qt4 QSsl]
      - Bundled libcurl + mbedtls / BearSSL / OpenSSL 1.1 - Patched QSslConfiguration
      - Embedded Mozilla cacert.pem                       - Embedded cacert.pem
      - Full SNI & TLS 1.2/1.3 support                    - Verify on device
               |                                                   |
               +-------------------------+-------------------------+
                                         |
                                  [Direct Success?]
                                   /           \
                             [YES]/             \[NO]
                                 /               \
                       Direct Cloud Sync     [Fallback Gateway Channel]
                    (api.todoist.com)       - Local reverse proxy / NAS
                                            - Local plain HTTP / custom auth
                                            - Translates to Todoist TLS 1.2/1.3
```

* **Primary Architecture (Direct Connection)**:
  * Compile `libcurl` statically linked with `mbedtls` or a modern `OpenSSL 1.1.1` target for QNX 6.6 (`armv7le`).
  * Bundle a clean Mozilla root CA bundle (`cacert.pem`) in the application sandbox assets (`app/native/assets/certs/cacert.pem`).
  * Provides complete autonomy, portability over cellular data, and zero external infrastructure dependencies.
* **Pragmatic Fallback Architecture (Local Gateway / NAS Proxy)**:
  * If on-device compilation of modern TLS fails due to toolchain incompatibilities, use a lightweight sync proxy on a local NAS or VPS.
  * The proxy terminates modern TLS and relays simplified requests to the device.
  * *Trigger Condition*: Only activated if hardware spike verification proves Direct Channel cannot establish TLS 1.2 on Passport.

---

## 4. Two-Way Synchronization & Data Consistency Specification

### 4.1 Storage & Schema Design (SQLite)
The client maintains an offline-first SQLite database (`todoist_local.db`) with atomic transactional boundaries:

1. **`tasks` Table**:
   * `id`: TEXT PRIMARY KEY (Can be a temporary UUID `temp_uuid_xxx` or server integer/string ID).
   * `server_id`: TEXT NULL (Populated when acknowledged by server).
   * `content`: TEXT NOT NULL.
   * `description`: TEXT.
   * `is_completed`: INTEGER NOT NULL DEFAULT 0.
   * `priority`: INTEGER NOT NULL DEFAULT 1.
   * `due_date`: TEXT NULL (ISO-8601).
   * `project_id`: TEXT NOT NULL DEFAULT 'inbox'.
   * `local_updated_at`: INTEGER NOT NULL (Epoch ms).
   * `sync_state`: TEXT NOT NULL (Options: `synced`, `locally_created`, `locally_updated`, `locally_deleted`).

2. **`sync_commands` Table (Outbound Queue)**:
   * `uuid`: TEXT PRIMARY KEY (Unique command UUID, generated at mutation time).
   * `type`: TEXT NOT NULL (`item_add`, `item_update`, `item_complete`, `item_uncomplete`, `item_delete`).
   * `temp_id`: TEXT NULL (Specified for `item_add`).
   * `args_json`: TEXT NOT NULL (Serialized JSON command arguments).
   * `status`: TEXT NOT NULL DEFAULT 'pending' (`pending`, `in_flight`, `failed`).
   * `created_at`: INTEGER NOT NULL.
   * `retry_count`: INTEGER NOT NULL DEFAULT 0.

3. **`sync_metadata` Table**:
   * `key`: TEXT PRIMARY KEY (`sync_token`, `last_synced_at`, `user_id`).
   * `value`: TEXT.

### 4.2 Strict Transaction Boundaries & Atomic Commit Rules

#### Rule 1: Atomic Mutation & Command Enqueueing
Whenever the user performs an action (create, edit, complete, delete), the local model change and the queue command **MUST execute within the same SQLite transaction**:

```sql
BEGIN IMMEDIATE;
-- 1. Mutate local task
UPDATE tasks 
SET is_completed = 1, local_updated_at = 1776000000000, sync_state = 'locally_updated'
WHERE id = 'server_task_123';

-- 2. Enqueue outbound command
INSERT INTO sync_commands (uuid, type, temp_id, args_json, status, created_at)
VALUES ('550e8400-e29b-41d4-a716-446655440000', 'item_complete', NULL, '{"id": "server_task_123"}', 'pending', 1776000000000);
COMMIT;
```
* **Failure Guard**: If the app crashes or battery dies at this exact instant, the database state remains consistent. It is impossible to produce a "UI modified but action forgotten" state.

#### Rule 2: Persistent Command UUIDs & Idempotency
* Every command generates a persistent UUID (`QUuid::createUuid()`) upon creation.
* If a network timeout occurs after Todoist has executed the commands, the client resends the exact same batch with identical UUIDs.
* Todoist's server deduplicates repeated UUIDs and returns the cached `sync_status`, preventing duplicate item creation.

#### Rule 3: Temporary ID (`temp_id`) Reconciliation
1. **Creation**: When the user creates a task offline, client generates `temp_id = "temp_uuid_<uuid>"`. The local task record is inserted with `id = temp_id`.
2. **Dependent Operations**: If the user immediately edits or completes this task while still offline, subsequent commands in `sync_commands` set `args_json.id = temp_id`.
3. **Server Mapping**: Upon syncing, Todoist responds with:
   ```json
   {
     "temp_id_mapping": {
       "temp_uuid_xxx": "8492048123"
     },
     "sync_status": {
       "cmd_uuid_1": "ok"
     }
   }
   ```
4. **Atomic Remap**: In a single SQLite transaction:
   * Update `tasks` table: replace `temp_uuid_xxx` with `8492048123`.
   * Update pending/in-flight commands in `sync_commands`: replace `temp_uuid_xxx` with `8492048123` in `args_json`.

#### Rule 4: Atomic Sync Token & Reconciliation
During incremental sync pull:
* Applying remote updates, removing successful commands from `sync_commands`, and saving the new `sync_token` **MUST occur inside a single SQLite transaction**.
* If the sync process is interrupted mid-write, the transaction rolls back, preserving the previous `sync_token` so the next sync can cleanly re-fetch the missed delta.

### 4.3 Conflict Resolution Strategies

| Scenario | Local State | Remote State | Resolution Strategy |
| :--- | :--- | :--- | :--- |
| **Non-overlapping field edit** | Title edited | Due date changed | **Field-level Merge**: Local title retained, remote due date applied. |
| **Same field concurrent edit** | Title edited ("A") | Title edited ("B") | **Last-Write-Wins (LWW)** based on updated timestamps; if unresolved, client local edit takes precedence on push. |
| **Local edit vs Remote delete** | Edited offline | Deleted on server | **Remote Delete Wins**: Local task marked deleted; user notified via transient toast. |
| **Local delete vs Remote edit** | Deleted offline | Edited on server | **Local Delete Wins**: Server item deleted on next push. |

### 4.4 Partial Batch Failures & Error Handling
Todoist returns `sync_status` as a map of `command_uuid -> "ok" | error_object`:
* **Success (`"ok"`)**: Delete command from `sync_commands`.
* **Transient Error (Network drop, HTTP 5xx, HTTP 429)**: Retain in `sync_commands`, schedule exponential backoff retry (e.g., 2s, 4s, 8s, up to 60s max) with random jitter.
* **Permanent Client Error (HTTP 400, item not found, invalid argument)**: Move command to `dead_letter_queue` table, mark local task with a sync warning badge, and log error details. Do not block subsequent valid commands.

---

## 5. Build, Packaging & Runtime Environment

### 5.1 Toolchain & SDK Configuration
* **SDK**: BlackBerry 10 Native SDK (BB10 NDK) version 2.1.0.
* **Host Environment**: Linux x86_64 or containerized build environment (`bbndk-env.sh`).
* **Target Architecture**: `armle-v7` (QNX Neutrino 6.6 RTOS).
* **Compiler**: QNX GCC 4.8.x.
* **Framework**: Qt 4.8.6 (BlackBerry Cascades framework).

### 5.2 Deployment & Sideloading in Post-Sunset Era
BlackBerry shut down its official consumer signing and developer portal services in January 2022. Therefore, deployment relies on:
1. **Developer Mode**: Target BlackBerry Passport must have Development Mode enabled in Settings.
2. **Debug Token / Offline Sideloading**:
   * Generate an offline debug token or sideload `.bar` packages using tools like `blackberry-deploy`, Sachesi, or direct command line via USB/Wi-Fi.
   * On rooted devices, binaries and configuration can be directly verified and tested via SSH.

### 5.3 Application Lifecycle & Sync Execution
1. **Foreground / Active Sync**:
   * Sync triggered automatically on application cold start.
   * Manual Pull-to-Refresh on the task list.
   * Debounced sync triggered 3 seconds after local user mutations settle.
2. **Minimized State (Active Frame)**:
   * When minimized to the Active Frame, an internal `QTimer` polls for changes at a conservative interval (e.g., every 5 minutes if Wi-Fi is connected, 15 minutes on cellular).
3. **Headless / Background Processing**:
   * Post-MVP exploration: In BB10, persistent background execution requires a separate headless service definition in `bar-descriptor.xml`.
   * For MVP, background sync while app is terminated is out of scope; synchronization executes when the app is active or running in an Active Frame.

---

## 6. Functional Boundaries & MVP Scope

### 6.1 MVP Scope vs Post-MVP

| Feature Area | Included in MVP | Deferred to Post-MVP |
| :--- | :--- | :--- |
| **Tasks** | View Inbox & Projects, Create task, Edit title & description, Toggle complete, Delete task | Subtasks / Parent-child hierarchy, Task reordering |
| **Dates & Scheduling** | View and set due date / time | Complex natural language recurrence rules parsing locally |
| **Projects** | List projects, assign task to project | Create/edit/archive projects |
| **Labels & Filters** | Read-only display of labels | Label management, advanced filter syntax |
| **UI Experience** | Native Cascades ListView, Pull-to-refresh, Physical keyboard shortcuts (Space to toggle, Enter to open, Del to delete) | Complex swipe gestures, custom themes |

### 6.2 Authentication & Credential Storage
* **Method**: Todoist Personal API Token (Bearer Token).
* **Setup**: User enters or pastes the Personal API Token in the initial Settings / Setup screen.
* **Security**:
  * Stored on-device in application private storage (`data/` sandbox) via QNX keystore (`bps/keyval`) or encrypted local SQLite database.
* **Session Management**:
  * If the API returns HTTP 401 Unauthorized, notify the user, pause the sync engine, and prompt for token re-entry.

---

## 7. Modular System Architecture & Concurrency

```
+-----------------------------------------------------------------------------------+
|                               UI Layer (Main Thread)                              |
|   - Cascades QML Views (ListView, TaskDetailSheet, Settings)                      |
|   - C++ ViewModels (TaskListModel : QAbstractListModel)                           |
+-----------------------------------------------------------------------------------+
                                   | Signals / Slots
+-----------------------------------------------------------------------------------+
|                        Core Controller & Sync Engine                              |
|   - SyncEngine (Worker Thread): Orchestrates push/pull loops                      |
|   - CommandDispatcher: Serializes commands and monitors sync_status               |
+-----------------------------------------------------------------------------------+
          |                                                   |
+------------------------------+             +--------------------------------------+
|  Storage Layer (SQLite)      |             |  Transport Layer (Worker Thread)     |
|  - TaskRepository            |             |  - NetworkClient (libcurl/mbedtls)   |
|  - SyncCommandQueue          |             |  - Bundled Mozilla cacert.pem        |
|  - Atomic Transactions       |             |  - HTTP Keep-Alive / Retry Manager   |
+------------------------------+             +--------------------------------------+
```

### Thread Safety Rules
1. **UI Thread Unblocked**: All network I/O, TLS handshakes, and heavy JSON parsing execute on dedicated worker threads.
2. **Database Concurrency**: The SQLite database runs in WAL (Write-Ahead Logging) mode. Read queries from the UI model and write operations from the Sync Engine avoid lock starvation.
3. **Inter-Thread Communication**: Qt's asynchronous signal/slot mechanism communicates state changes between the `SyncEngine` worker thread and UI `TaskListModel`.

---

## 8. Verifiable Acceptance Test Scenarios

1. **Hardware TLS Probe (Pre-flight Gate)**:
   * Deploy a minimal CLI/NDK probe binary to the physical Passport.
   * Verify direct HTTPS request to `https://api.todoist.com/api/v1/sync` using embedded `cacert.pem`. Validate TLS 1.2 handshake, SNI, and valid JSON response.
2. **Offline Create -> Edit -> Sync Flow**:
   * Disconnect device from network.
   * Create task "Task A" (`temp_id` generated).
   * Edit "Task A" description to "Detail 1".
   * Reconnect network, trigger sync.
   * Verify server receives single task with "Detail 1", and local DB atomically updates `temp_id` to server ID.
3. **Dropped Response Recovery (Idempotency)**:
   * Simulate network failure immediately after server processes POST but before client receives HTTP 200.
   * Trigger retry.
   * Verify that Todoist does not create a duplicate task and returns existing `sync_status`.
4. **Application Crash Resilience**:
   * Simulate power loss or SIGKILL immediately after a task creation in UI.
   * On restart, verify database consistency: either both task and sync command exist, or neither exists.
