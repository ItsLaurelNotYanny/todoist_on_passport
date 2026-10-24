# Milestone 2: Sync Engine & State Machine (M2) — Architecture & Design Specification (Revision 3)

## 1. Overview & Architectural Boundaries

Milestone 2 (`todoist_sync`) implements the synchronization engine, protocol adapter, and state machine for the native offline-first Todoist client on BlackBerry 10 Passport (QNX 6.6).

### 1.1 Layer Separation & Ownership Boundaries
1. **`todoist_storage` (Milestone 1, Accepted)**: Local transactional SQLite storage with WAL mode, atomic local mutations, FIFO outbound command queue (`sync_commands`), and `temp_id` remapping.
2. **`todoist_sync` (Milestone 2, Current)**: Protocol payload builder, wire-to-domain adapter, response validator, atomic single-transaction dispatcher, deterministic sync state machine, and multi-batch queue draining. Background threading is explicitly deferred to integration (M3/M4).
3. **`todoist_network` (Milestone 3, Transport & Retry Scheduler)**: Platform-specific network transport (libcurl / isolated OpenSSL 3.5.9 TLS 1.2+), HTTP client, Bearer token auth, network reachability monitoring, exponential backoff timer scheduling, and background thread integration.
4. **Cascades UI (Milestone 4, Presentation)**: QML views (1440×1440), C++ `QAbstractListModel` adapters, and keyboard navigation.

```
+--------------------------------------------------------------------------------------------------+
|                          Integration & Presentation (Milestones 3 & 4)                           |
|    - Network Transport (libcurl/OpenSSL), Bearer auth, exponential backoff timer scheduler       |
|    - Cascades QML UI, TaskListModel, pull-to-refresh, background worker orchestration            |
+--------------------------------------------------------------------------------------------------+
                                                | Synchronous Single-Cycle API / Queue Drain
+--------------------------------------------------------------------------------------------------+
|                            todoist_sync: Core Deterministic Sync Engine                          |
|                                                                                                  |
|   +-----------------------+     +------------------------+     +-----------------------------+   |
|   |   SyncStateMachine    |     |   SyncPayloadBuilder   |     |     SyncResponseParser      |   |
|   |   - Idle              |     |   - Atomic claim tx    |     |     - Strict typed validate |   |
|   |   - Syncing           |     |   - Per-task filter    |     |     - Outcome-gated map     |   |
|   |   - RetryPending      |     |   - Frozen wire payload|     |     - Baseline delta merge  |   |
|   |   - AuthError         |     +------------------------+     +-----------------------------+   |
|   +-----------------------+                 |                                 |                  |
+---------------------------------------------|---------------------------------|------------------+
          |                                   |                                 |
          v                                   v                                 v
+-------------------------------+   +--------------------+          +------------------------------+
|   todoist_storage (M1)        |   |   ISyncTransport   |          |    Single Outer Transaction  |
|   - TaskRepository            |   |   (Abstract Seam)  |          |    1. Outcome-gated TempId map|
|   - SyncCommandQueue          |   |   - Synchronous POST|         |    2. Cascade failed creates |
|   - SyncMetadataRepository    |   |   - Header inject  |          |    3. Delta item merge       |
|   - TempIdReconciler          |   +--------------------+          |    4. Acks & ack pruning     |
+-------------------------------+                                   |    5. Tombstone-aware final  |
                                                                    |    6. Provenance absence prune|
                                                                    |    7. Token Advance          |
                                                                    +------------------------------+
```

### 1.2 Invariant Constraints
- **C++03 & POSIX Purity**: Compiled under `-std=c++03 -Wall -Wextra -Wpedantic -Wno-long-long`. Reentrancy guards use POSIX/C++03-compatible volatile flags / mutex guards.
- **Queue Contract Preservation**: Adheres strictly to M1's accepted per-task serialization and FIFO ordering (verified under R008). In-flight commands are tracked accurately; retries maintain dependency order.
- **Single Checked Outer Transaction**: All response operations (reconciliation, cascade failures, delta upserts, command pruning, tombstone-aware finalization, provenance-guarded absence pruning, and token persistence) execute within a single outer `DatabaseManager::ScopedTransaction`. Any nested failure aborts the transaction completely.

---

## 2. Storage-to-Wire & Wire-to-Domain Protocol Adapter (M2-D003, Closed; M2-R012 Corrected)

### 2.1 Request Schema & Encoding
- **Encoding**: Official API v1 form URL-encoded request body (`Content-Type: application/x-www-form-urlencoded`).
- **Wire Parameters**:
  - `sync_token`: String, current cursor or `*` for initial full sync.
  - `resource_types`: JSON array serialized and URL-encoded, e.g. `%5B%22items%22%5D`.
  - `commands`: JSON array of commands serialized and URL-encoded.
- **Top-Level Wire Structure**:
  ```http
  POST /api/v1/sync HTTP/1.1
  Content-Type: application/x-www-form-urlencoded

  sync_token=dG9rZW5fZXhhbXBsZQ%3D%3D&resource_types=%5B%22items%22%5D&commands=%5B...%5D
  ```
- **Resource Selection**: The current integrated request uses `resource_types: ["items", "projects", "user"]`. Project names and the user inbox ID are persisted in `sync_metadata`; a separate projects table is not required.

### 2.2 Outbound Command Mapping
Each internal `SyncCommand` is translated to the official Todoist API v1 schema:

| Internal Command | Wire `type` | Wire `args` Structure | Field Rules |
| :--- | :--- | :--- | :--- |
| `item_add` | `item_add` | `{"content": string, "priority": int, ...}` | If `project_id == "inbox"`, omit `project_id`. If `due_date` is non-empty, format as `"due": {"date": "..."}`; if empty, omit `due`. Include `"temp_id": string`. |
| `item_update` | `item_update` | `{"id": string, ...}` | Only changed fields included. Clearing due date: `"due": null`. Opaque server/temp ID preserved. |
| `item_complete` | `item_close` | `{"id": string}` | Official API v1 uses `item_close` to mark complete. |
| `item_uncomplete` | `item_uncomplete` | `{"id": string}` | Reopens a completed item. |
| `item_delete` | `item_delete` | `{"id": string}` | Permanent server deletion. |

### 2.3 Inbound Task & Timestamp Mapping
- `id`: Opaque string (preserved verbatim).
- `content`: UTF-8 string.
- `description`: UTF-8 string (empty if null/omitted).
- `checked` / `is_completed`: Boolean or integer (0/1) mapped to `Task::is_completed`.
- `priority`: Integer 1 (Normal) to 4 (Urgent).
- `due`:
  - If object: extract `"date"` string (`YYYY-MM-DD` or full ISO datetime) -> `Task::due_date`.
  - If null or missing: `Task::due_date = ""`.
- `project_id`: String (defaults to `"inbox"` if missing or matches default inbox).
- `is_deleted`: Integer or boolean (1/true indicates deletion).
- `updated_at`: RFC3339 / ISO-8601 UTC string (`YYYY-MM-DDTHH:MM:SSZ`, with optional fractional seconds and timezone offsets `+HH:MM` / `-HH:MM`).
  - Parsed into integer epoch milliseconds using target-portable arithmetic (POSIX `gmtime` / portable integer time calculations).
  - **Timestamp Fallback Policy**: If `updated_at` is missing, unparsable, or malformed, set `remote_updated_at = 0` (epoch 0 sentinel). **Never substitute receipt time or current local time.** Because M1's LWW requires `remote > local` to win, a 0 sentinel guarantees a corrupt or missing remote timestamp can never defeat a valid local edit (`local_updated_at > 0`).

---

## 3. Durable Request Lifecycle & Claim Protocol (M2-D002, Closed)

### 3.1 Atomic Selection, Per-Task Filtering & Claim Boundary
All selection, per-task filtering, durable claiming, and snapshot generation occur inside a **single checked SQLite transaction**:
1. **Query Pending Rows**: Select pending commands ordered by `created_at ASC` (FIFO).
2. **Per-Task Serialization Filter**:
   - Track task IDs active in this batch.
   - For any task, allow **at most one unresolved command** in flight. If task A has pending commands $C_1$ and $C_2$, claim $C_1$ and defer $C_2$ to a future cycle.
   - Cap batch at `maxBatchSize` (default: 50).
3. **Durable In-Flight Claim**:
   - Update selected rows to `status = 'in_flight'` in `sync_commands`.
4. **Frozen Request Generation**:
   - Construct immutable `FrozenRequest` containing the exact command UUIDs and serialized wire arguments.
   - Stored arguments in `sync_commands` are immutable; wire arguments are reconstructed deterministically from stored fields.
5. **Commit Before I/O**:
   - Commit the transaction **before** passing the payload to `ISyncTransport`.

### 3.2 Startup Recovery
To ensure crashes or power loss during I/O do not erase send history or permanently block tasks:
- Run once during `SyncEngine::initialize()`, before accepting any user mutations or sync cycles:
  ```sql
  UPDATE sync_commands 
  SET status = 'pending', retry_count = retry_count + 1 
  WHERE status = 'in_flight';
  ```
- **Ambiguity Preservation**: Incrementing `retry_count` ensures commands abandoned during in-flight I/O are recognized as previously attempted (`retry_count > 0`), preventing conflicts or local delete pruning from treating them as never sent.

### 3.3 Cascading Create Failure
If an `item_add` command is permanently rejected (e.g. HTTP 400 validation error):
- In the same outer transaction, invoke `cascadeCreateFailure(tempId, errorMsg)`.
- Moves all pending and in-flight commands referencing `temp_id` (via `temp_id` or `args_json.id == temp_id`) to `dead_letter_commands` with `"Parent item_add failed permanently"`.
- `cascadeCreateFailure` returns `bool` and its success is strictly checked.

---

## 4. Single Outer Transaction & Response Dispatch

### 4.1 Response Pre-Validation (Before DB Lock)
Before acquiring SQLite locks, validate the raw response body in memory:
1. Valid JSON root object.
2. `sync_token` present and non-empty string.
3. `full_sync` present and boolean.
4. `items` present and valid array of objects.
5. `temp_id_mapping` present and valid object of string-to-string.
6. `sync_status` present and valid object of string-to-status.
*If validation fails*: Discard response, execute zero response writes, increment batch retry count in a separate recovery transition, and transition to `RetryPending`.

### 4.2 Outcome-Gated Mapping & In-Memory Correlation (M2-D004)
To prevent inconsistent responses from corrupting command retry representations:
1. **Request-Level Triage**:
   - `HTTP 401 Unauthorized`: Transition to `AuthError`. Reset in-flight batch to `pending` (preserving retry counts). No commands dead-lettered.
   - `HTTP 429 Too Many Requests`: Transition to `RetryPending`. Reset in-flight batch to `pending` with incremented retry count.
   - `HTTP 5xx / Network Timeout / Malformed Body`: Transient failure. Reset in-flight batch to `pending` with incremented retry count. Transition to `RetryPending`.
2. **In-Memory Command Outcome Classification**:
   - For every in-flight command:
     - Unsolicited UUIDs in `sync_status` are safely ignored.
     - Absent status for in-flight command is treated as an unresolved transient failure.
3. **Outcome-Gated Mapping Correlation**:
   - An entry `{temp_id: server_id}` from `temp_id_mapping` is accepted **only if** its corresponding `item_add` command in the frozen request received an explicit `"ok"` status in `sync_status`.
   - **Inconsistent Response Defense**:
     - Case A: An `item_add` has `{temp_id: server_id}` in `temp_id_mapping`, but its `sync_status` is missing, retryable error, or unknown.
     - Case B: An `item_add` received `"ok"` in `sync_status`, but its `temp_id` is missing from `temp_id_mapping`.
     - In either case, the response is **inconsistent and rejected**:
       - No database writes from the response payload are executed.
       - Temporary IDs, task IDs, and outbound arguments are left completely intact in SQLite.
       - A separate checked retry recovery transition resets in-flight commands to `pending` with incremented `retry_count`, preserving their exact original wire arguments and UUIDs for retry.
   - Unsolicited mappings (mappings for `temp_id`s not present in the in-flight request) are discarded before any repository call.

### 4.3 Single Checked Outer Transaction
```cpp
DatabaseManager::ScopedTransaction tx(m_db);
if (!tx.isActive()) {
    return false;
}

// 1. Outcome-Gated Temporary ID Reconciliation
// validatedMapping contains ONLY mappings for in-flight item_add commands that received "ok"
if (!validatedMapping.empty()) {
    if (!m_tempIdReconciler.reconcile(validatedMapping)) return false;
}

// 2. Command Triage & Cascading Failures
for (size_t i = 0; i < inFlightCommands.size(); ++i) {
    const SyncCommand& cmd = inFlightCommands[i];
    CommandResult res = commandOutcomes[cmd.uuid];
    if (res.isAck) {
        ackUuids.push_back(cmd.uuid);
    } else if (res.isPermanent) {
        if (!m_commandQueue.markCommandFailedPermanent(cmd.uuid, res.error)) return false;
        if (cmd.type == "item_add") {
            if (!cascadeCreateFailure(cmd.temp_id, res.error)) return false;
        }
    } else {
        if (!m_commandQueue.markCommandFailedTransient(cmd.uuid, res.error)) return false;
    }
}

// 3. Upsert Incoming Items (3-way baseline merge)
for (size_t i = 0; i < remoteTasks.size(); ++i) {
    if (!m_taskRepo.upsertRemoteTask(remoteTasks[i])) return false;
}

// 4. Remote Explicit Deletions (Authoritative)
for (size_t i = 0; i < remoteDeletedIds.size(); ++i) {
    if (!m_taskRepo.deleteRemoteTask(remoteDeletedIds[i])) return false;
}

// 5. Prune Acknowledged Commands
if (!ackUuids.empty()) {
    if (!m_commandQueue.markBatchAcknowledged(ackUuids)) return false;
}

// 6. Tombstone-Aware Task State Finalization (M2-D001)
if (!finalizeAcknowledgedTaskStates(inFlightCommands, ackUuids, validatedMapping)) return false;

// 7. Provenance-Guarded Full-Sync Absence Pruning (M2-D005, only when full_sync == true)
if (isFullSync) {
    if (!pruneAbsentSyncedTasksWithProvenance(preRequestSyncedTaskIds, remoteTasks)) return false;
}

// 8. Advance Sync Token
if (!m_syncMetadataRepo.setSyncToken(newSyncToken)) return false;

return tx.commit();
```

### 4.4 Tombstone-Aware Task State Finalization (M2-D001)
For every task associated with an acknowledged command in this request:
1. Resolve the task's current ID in SQLite (accounting for any newly reconciled server ID from `validatedMapping`).
2. Query `sync_commands` for remaining pending or in-flight commands referencing this task.
3. **Tombstone & Local Intent Priority**:
   - If the task is currently `LocallyDeleted` or has a remaining delete command in `sync_commands`:
     - **Keep `LocallyDeleted`!** It must never be reclassified as `Synced` or `LocallyUpdated`. It is pruned only when deletion itself is conclusively resolved and no remaining command needs the row.
   - Else if other remaining commands exist (e.g. user edited task locally during I/O):
     - Retain `LocallyUpdated` (or `LocallyCreated`) and preserve dirty state.
   - Else (no remaining local commands exist):
     - Transition `sync_state` to `Synced` and update `task_baselines` to match current task state.
4. **Acknowledged Tasks Omitted from Echo**: Tasks acknowledged by the server but omitted from `items` transition to `Synced` and update baselines cleanly.

### 4.5 Provenance-Guarded Full-Sync Absence Pruning (M2-D005)
When the server sends `full_sync == true`:
1. **Present Remote Items**: Always merged via three-way baseline merge (`TaskRepository::upsertRemoteTask`). Explicit remote deletes remain authoritative.
2. **Provenance-Guarded Candidate Selection**:
   - Eligible candidates for absence pruning are strictly restricted to:
     $$\text{Candidates} = \text{PreRequestCleanTasks} \setminus \text{RequestTouchedTasks}$$
   - `PreRequestCleanTasks`: Local tasks that had `sync_state == Synced` and zero unresolved queue commands before this request started.
   - `RequestTouchedTasks`: Any task that was in `inFlightCommands`, was newly mapped, or had commands acknowledged in this cycle.
   - Newly created/acknowledged tasks whose server echo is delayed are **never pruned**.
3. **Absent Dirty Tasks**: Any task with `sync_state != Synced` or with unresolved commands in `sync_commands` is **strictly preserved**.
4. **Delayed Snapshot Catch-Up**: Following a full sync, the engine exposes `hasFullSyncCatchUpNeeded = true` to allow caller/M3 to schedule an immediate incremental catch-up delta.

---

## 5. State Machine & Boundary with M3 (M2-D006, Closed)

- **M2 Deterministic Scope**:
  - State machine states: `Idle`, `Syncing`, `RetryPending`, `AuthError`.
  - Single-cycle API:
    ```cpp
    SyncCycleResult SyncEngine::executeSyncCycle(ISyncTransport& transport);
    ```
  - **Zero background timers, sleeps, or network scheduling in M2**: Unit tests run 100% synchronously and deterministically.
- **M3 Transport Scope**:
  - Owns exponential backoff timer scheduling, random jitter calculation, network monitoring, and triggering subsequent calls to `executeSyncCycle()`.

---

## 6. Single-Cycle & Multi-Batch Queue Draining Contracts (M2-D007, Closed)

### 6.1 Reentrancy & Single-Request Guard
- An internal reentrancy guard (`m_cycleActive`) guards `executeSyncCycle()`.
- Calling `executeSyncCycle()` while another cycle is active returns `SyncCycleResult::Busy` immediately without blocking or overlapping requests.

### 6.2 Multi-Batch Queue Draining (`drainPendingQueue`)
To drain queues larger than a single batch (e.g. 120 commands across batches of 50):
```cpp
SyncCycleResult SyncEngine::drainPendingQueue(ISyncTransport& transport, int maxBatches = 10);
```
- Loops executing `executeSyncCycle(transport)` while eligible unblocked commands remain.
- **Stop Conditions**:
  1. Queue is empty or no eligible commands remain (all remaining tasks are already in flight or retry) -> Returns `SyncCycleResult::Success`, state is `Idle`.
  2. Cycle returns `RetryPending` -> Draining **stops immediately**, leaving retries to M3. It never spins on retryable errors.
  3. Cycle returns `AuthError` -> Draining **stops immediately**, state is `AuthError`.
  4. Batch limit reached -> Returns `SyncCycleResult::Success`.

### 6.3 Threading Model Deferral
- Multi-threading, background worker threads (`pthread_create`), and transport cancellation are **explicitly deferred to M3/M4 integration**.
- In M2, `todoist_sync` is delivered as a 100% thread-agnostic, synchronous, deterministic engine.

---

## 7. Third-Party JSON Dependency (`picojson`)

- **Selected Header**: `picojson.h` (v1.3.0, BSD 2-Clause license).
- **Location**: `third_party/picojson/picojson.h`.
- **Compiler Compatibility**: Strictly C++03 compliant (`-std=c++03 -Wall -Wextra -Wpedantic`). The target build uses the Clang-based `bb10-c++` driver from the pinned Docker environment; host builds use GCC or Clang.

---

## 8. Concrete Acceptance Test Scenarios

The M2 test suite (`tests/sync/test_sync_all.cpp`) will execute synchronously against mock `ISyncTransport`:

1. **Tombstone-Aware Finalization & Cascading Rollback (M2-D001)**:
   - Task A starts synced; send update U; during I/O user calls local delete, leaving tombstone A and pending delete D. Receive `U: "ok"` and active remote echo. Assert: U pruned, D remains, A remains `LocallyDeleted` (not reclassified to Synced). After D succeeds, A is pruned.
   - Force failure in `cascadeCreateFailure` or token persistence. Assert full rollback: tasks, baselines, commands, and token remain at pre-dispatch state.
   - Acknowledge update without item echo -> transitions to `Synced`.
2. **Durable Claim & Startup Recovery (M2-D002)**:
   - Claim batch, simulate crash. Run startup recovery: assert `status` reset to `pending` and `retry_count` incremented.
   - Verify recovered create followed by local delete retains deletion intent (does not prune locally).
   - Verify per-task filter: task with 2 pending commands claims only first command in batch 1; second command claims in batch 2.
3. **Protocol Adapter & Timestamp Sentinel (M2-D003)**:
   - Ingest item with missing/malformed `updated_at`: assert timestamp defaults to 0 sentinel and local edit (timestamp > 0) wins LWW.
   - Outbound wire format: verify `item_close`, inbox omission, `"due": null` clearing, opaque string IDs.
4. **Pre-Validation & Outcome-Gated Mapping (M2-D004)**:
   - In-flight create U (temp_id T): response has `{mapping: {T: S}, status: {}}` (missing status) -> response rejected, no remap, T and U preserved with exact original arguments for retry.
   - Response has `{mapping: {T: S}, status: {U: retryable_error}}` -> no remap, U retained as pending retry.
   - Response has `{mapping: {}, status: {U: "ok"}}` -> rejected, U retained for retry.
   - Response has `{mapping: {T: S}, status: {U: "ok"}}` -> atomic remap and acknowledgement.
   - Unsolicited mapping outside batch -> safely ignored.
   - HTTP 401 -> transitions to `AuthError`, zero dead-letter rows.
5. **Provenance-Guarded Full-Sync Semantics (M2-D005)**:
   - Full reset where newly acknowledged create S has delayed echo (absent from items) -> S is strictly preserved with correct baseline; old absent clean row X is pruned; absent unsent create C and tombstone D remain protected.
   - Present dirty tasks undergo three-way baseline merge.
6. **Single Retry Owner (M2-D006)**:
   - Transient failure transitions to `RetryPending` with zero sleeps; subsequent cycle requires explicit trigger.
7. **Queue Draining (M2-D007)**:
   - Drain 120 commands across 3 cycles cleanly.
   - Transient error on batch 2 stops immediately without spinning.
   - Concurrent `executeSyncCycle` returns `Busy`.
