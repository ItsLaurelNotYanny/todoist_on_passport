# RFC: Native Cascades UI, ViewModels, Active Frame & Full-System Assembly (Milestone 4)

## 1. Metadata & Overview

| Field | Value |
| :--- | :--- |
| **RFC Title** | Native Cascades UI, ViewModels, Active Frame & Full-System Assembly for BlackBerry Passport |
| **Milestone Context** | Milestone 4 (`todoist_ui`, `todoist-passport`, `todoist-passport.bar`) |
| **Author** | agy (Implementation Lead) |
| **Reviewer & Architecture Partner** | Codex (Review Lead & Architecture Partner) |
| **Status** | Milestone 4 Architecture & Implementation Specification |
| **Target Runtime** | BlackBerry 10 OS 10.3.3.3216 (QNX Neutrino 6.6 RTOS, ARMv7-A / Krait 400, 1440×1440 1:1 Viewport) |
| **Host Toolchain** | GCC/Clang C++03 (`todoist_ui` core + `test_ui`) and `bb10-c++` + Qt 4.8.6 `moc` (`todoist-passport` + `blackberry-nativepackager`) |

---

## 2. Executive Summary & Architectural Goals

Milestone 4 completes the end-to-end engineering assembly of **Todoist on BlackBerry 10 Passport**, connecting the verified foundation layers from Milestones 1–3 into a responsive, offline-first native BlackBerry 10 Cascades application:

1. **Milestone 1 (`todoist_storage`)**: Transactional SQLite WAL persistence (`DatabaseManager`, `TaskRepository`, `SyncCommandQueue`, `TempIdReconciler`, `SyncMetadataRepository`).
2. **Milestone 2 (`todoist_sync`)**: Deterministic Todoist Sync API v1 payload construction, response ingestion, field-level conflict resolution, and command queue state transitions (`SyncEngine`).
3. **Milestone 3 (`bb10_tls` & `todoist_network`)**: Symbol-isolated OpenSSL 3.5.9 + libcurl 8.23.0 TLS 1.3 transport (`CurlSyncTransport`), thread-safe cancellation (`CancellationToken`), lock-protected event queue (`SyncNotificationQueue`), and background POSIX worker (`SyncWorkerThread`).
4. **Milestone 4 (`todoist_ui` & `todoist-passport`)**:
   - **Host-Portable Presenter & State Machine (`TodoistAppController`)**: Pure C++03 presentation controller orchestrating local task CRUD, project/date/completion filtering, square-screen sorting, physical QWERTY keyboard shortcut dispatch, 3-second mutation sync debouncing, non-blocking worker event draining, credential management, and Active Frame lifecycle transitions.
   - **Qt 4.8.6 / Cascades Model & Bridge Layer (`CascadesTaskListModel`, `QtTaskListModel`, `CascadesAppBridge`)**: Exposes `bb::cascades::DataModel` for native Cascades `ListView`, `QAbstractListModel` for standard Qt model consumers, and `Q_PROPERTY` / `Q_INVOKABLE` bindings for QML.
   - **1440×1440 Square-Viewport QML UI & Active Frame (`assets/main.qml`, `assets/ActiveFrameCover.qml`)**: High-density 1:1 layout with filter navigation bar, task list, create/detail/settings modal sheets, physical keyboard `Shortcut` bindings, and minimized `SceneCover` Active Frame.
   - **Development BAR Packaging (`bar-descriptor.xml`, `scripts/check-package.py`)**: Produces `build/bb10/todoist-passport.bar` containing the symbol-isolated `armle-v7` ELF executable, QML assets, 144×144 launcher icon, bundled Mozilla `cacert.pem`, and third-party license notices.

---

## 3. Modular Layering & Threading Contract

```
+---------------------------------------------------------------------------------------+
|                         Main UI Thread (Qt 4.8 Event Loop)                            |
|                                                                                       |
|  +----------------------------------------+   +------------------------------------+  |
|  | Cascades QML Scene (1440x1440)         |   | Active Frame (bb::cascades::       |  |
|  | - assets/main.qml (ListView, Sheets,   |   |   SceneCover)                      |  |
|  |   Shortcuts: Space/Enter/Delete/C/R)   |   | - assets/ActiveFrameCover.qml      |  |
|  +-------------------+--------------------+   +-----------------+------------------+  |
|                      | Q_PROPERTY / Q_INVOKABLE / Signals       |                     |
|                      v                                          v                     |
|  +---------------------------------------------------------------------------------+  |
|  | CascadesAppBridge (QObject) + CascadesTaskListModel (bb::cascades::DataModel)   |  |
|  | + QtTaskListModel (QAbstractListModel) + QTimer (100ms UI tick)                 |  |
|  +----------------------------------------+----------------------------------------+  |
|                                           |                                           |
|                                           v                                           |
|  +---------------------------------------------------------------------------------+  |
|  | TodoistAppController (Pure C++03 Host-Testable Presenter & Lifecycle Engine)    |  |
|  | - Local atomic CRUD via TaskRepository (bounded retry on concurrent WAL lock)   |  |
|  | - Filter/Sort pipeline (Inbox, Today, All, Completed, Project, Search)          |  |
|  | - Keyboard shortcut dispatcher (modal-sheet guard prevents background mutation) |  |
|  | - 3000ms mutation sync debounce & 5m (Wi-Fi) / 15m (Cellular) Active Frame poll |  |
|  +-------------------+------------------------------------------+------------------+  |
+----------------------|------------------------------------------|---------------------+
                       | SQLite WAL (Owner-Guarded Transactions)  | SyncNotificationQueue
                       v                                          | (Non-blocking drain)
+----------------------------------------------+                  |
| Storage Layer (M1: todoist_storage)          |                  |
| - DatabaseManager (WAL, busy_timeout=5000ms) |                  |
| - TaskRepository / SyncCommandQueue          |                  |
| - TempIdReconciler / SyncMetadataRepository  |                  |
+----------------------+-----------------------+                  |
                       ^                                          |
                       | Atomic Delta Ingestion                   | push(SyncEventSnapshot)
+----------------------+------------------------------------------+---------------------+
|                      Background POSIX Worker Thread (M3: SyncWorkerThread)            |
|  - Executes SyncEngine::executeSyncCycle(CurlSyncTransport) off the UI thread         |
|  - Full TLS 1.3 handshake, SNI, ALPN, cacert.pem verification, and JSON parsing       |
|  - Saturating full-jitter exponential backoff & immediate reachability wakeup         |
+---------------------------------------------------------------------------------------+
```

### 3.1 Strict Non-Blocking UI Thread Rules
1. **Network I/O Off Main Thread**: All HTTPS POST requests, TLS 1.3 handshakes, and delta response JSON parsing execute exclusively inside `SyncWorkerThread`.
2. **Bounded SQLite WAL Access**: Local UI mutations (`createTask`, `updateTask`, `toggleTaskCompleted`, `deleteTask`) execute on the local SQLite database in WAL mode via `TaskRepository`. If `SyncWorkerThread` holds an active `DatabaseManager` write transaction during delta ingestion, `DatabaseManager` waits on its condition variable (`m_txCond`), and `TodoistAppController` additionally wraps UI write calls with a bounded short retry helper so transient worker commit windows never drop a user action.
3. **Asynchronous Event Draining**: `SyncWorkerThread` publishes state transitions (`WORKER_STATE_SYNCING`, `WORKER_STATE_SUCCESS`, `WORKER_STATE_BACKOFF_WAIT`, `WORKER_STATE_FATAL_ERROR`, `WORKER_STATE_IDLE`) into `SyncNotificationQueue`. On the UI thread, `TodoistAppController::tick(nowMs)` drains queued snapshots without blocking, reloads visible tasks on `WORKER_STATE_SUCCESS`, and emits model change notifications.

---

## 4. Core Presenter & ViewModel Specification (`TodoistAppController`)

### 4.1 Filter Modes & Deterministic Square-Viewport Sorting
`TodoistAppController` supports five primary filter modes plus live substring search:
- `FilterMode::Inbox` (`"inbox"`): Incomplete tasks where `project_id == "inbox"`.
- `FilterMode::Today` (`"today"`): Incomplete tasks with a non-empty `due_date` that is less than or equal to the current ISO-8601 date (`YYYY-MM-DD`, including overdue tasks).
- `FilterMode::All` (`"all"`): All incomplete tasks across all projects.
- `FilterMode::Completed` (`"completed"`): Completed tasks (`is_completed == true`), optionally scoped to the active project.
- `FilterMode::Project` (`"project"`): Incomplete tasks matching a selected `project_id`.
- **Search Query**: Case-insensitive substring filter applied across `content`, `description`, and `project_id`.

**Deterministic Ordering**:
To maximize information density on the 1440×1440 screen, visible tasks are sorted deterministically:
1. Incomplete tasks precede completed tasks (`is_completed == false` before `true`).
2. Higher Todoist API priority precedes lower priority (`priority` descending: `4` = P1 Urgent, `3` = P2 High, `2` = P3 Medium, `1` = P4 Normal).
3. Tasks with a due date precede tasks without a due date; earlier ISO-8601 due dates precede later due dates (`due_date` ascending).
4. Most recently updated tasks precede older tasks (`local_updated_at` descending), with `id` ascending as a final deterministic tie-breaker.

### 4.2 Selection Stability Across `temp_id` Reconciliation
When an offline-created task (`temp_uuid_...`) is selected in the UI and a background sync cycle reconciles its temporary ID to a permanent Todoist server ID:
- `TodoistAppController` attempts to retain selection by matching `m_selectedTaskId`.
- If `m_selectedTaskId` was remapped to a new permanent ID during delta ingestion, `reloadTasks()` falls back to preserving the user's clamped `m_selectedIndex` and updates `m_selectedTaskId` to the reconciled item at that position, preventing focus loss.

### 4.3 Physical QWERTY Keyboard Shortcut Contract
The BlackBerry Passport features a hardware capacitive QWERTY keyboard with dedicated key events. `TodoistAppController::handleKeyShortcut(key, nowMs)` implements the following bindings:

| Physical Key | Shortcut Identifier(s) | Action | Modal Sheet Guard |
| :--- | :--- | :--- | :--- |
| **Space** | `"Space"`, `" "` | Toggle completion state of selected task (`setTaskCompletedLocally`) and schedule 3s debounced sync | Ignored when any modal sheet (`Create`, `Detail`, `Settings`) is open |
| **Enter** | `"Enter"`, `"Return"` | Open Task Detail / Edit sheet populated with selected task fields | Ignored when any modal sheet is already open |
| **Backspace / Del** | `"Delete"`, `"Backspace"`, `"Del"` | Delete selected task locally (`deleteTaskLocally`) and schedule 3s debounced sync | Ignored when any modal sheet is open |
| **C / N** | `"c"`, `"C"`, `"n"`, `"N"` | Open Quick Create Task sheet | Ignored when any modal sheet is open |
| **R** | `"r"`, `"R"` | Trigger immediate manual sync (`triggerSyncNow`) | Ignored when any modal sheet is open |
| **J / Down** | `"j"`, `"J"`, `"Down"` | Move task selection down one row | Ignored when any modal sheet is open |
| **K / Up** | `"k"`, `"K"`, `"Up"` | Move task selection up one row | Ignored when any modal sheet is open |
| **Escape** | `"Escape"`, `"Esc"` | Dismiss any open modal sheet (`closeSheets()`) | Active only when a modal sheet is open |

The task ListView disables its built-in `SystemShortcuts.ScrollDownOneScreen`
action so Space reaches the application completion shortcut. In 1.0.8 there is
no in-app deletion recovery or clear selected-filter highlight; both are planned.

### 4.4 Sync Triggers, Debouncing & Active Frame Lifecycle Polling
1. **Cold Start Sync**: During `initialize(nowMs)`, if a valid API token is stored in `sync_metadata` (`key = 'api_token'`) and network reachability is online, `triggerSyncNow(nowMs)` is invoked immediately.
2. **Debounced Local Mutation Sync (3,000 ms)**: Every local create, edit, completion toggle, or delete arms a coalesced debounce deadline (`nowMs + m_debounceWindowMs`, default `3000` ms). Subsequent mutations within the window slide the deadline forward so rapid keyboard actions batch into a single `/api/v1/sync` round-trip.
3. **Manual Pull-to-Refresh / Sync Action**: Invoking `triggerSyncNow(nowMs)` (via the UI Refresh button, pull-to-refresh gesture, or `R` shortcut) cancels any pending debounce timer and immediately signals `SyncWorkerThread::requestSync()`.
4. **Minimized Active Frame Polling**:
   - When the user minimizes the application to the BlackBerry 10 home screen (`AppLifecycleState::ActiveFrameThumbnail` or `AppLifecycleState::InvisibleBackground`), `TodoistAppController` switches to conservative periodic polling:
     - **Wi-Fi (`NetworkConnectionType::WiFi`)**: Every **5 minutes** (`300,000` ms).
     - **Cellular (`NetworkConnectionType::Cellular`)**: Every **15 minutes** (`900,000` ms).
     - **Offline (`NetworkConnectionType::Offline`)**: Periodic polling paused until reachability is restored.
   - `ActiveFrameSnapshot` is recomputed on every task mutation and sync event, providing the top 4 incomplete tasks, total incomplete task count, today/overdue count, pending outbound command count, and sync status headline to `SceneCover`.

### 4.5 Authentication & HTTP 401/403 Recovery
- Personal API Token is AES-encrypted before storage in `sync_metadata` under `"api_token"`. The master key is stored in a private file; the task database is ordinary SQLite, not database-wide encryption.
- `getMaskedApiToken()` returns a redacted representation (e.g., `"••••••••1234"`) for safe UI rendering.
- If `SyncWorkerThread` reports `WORKER_STATE_FATAL_ERROR` with HTTP `401` or `403` (or `SyncEngineState::AuthError`), `TodoistAppController` latches `requiresAuth = true`, pauses background polling, opens the Settings / Token sheet (`settingsSheetOpen = true`), and displays an authentication recovery prompt. Entering a new non-empty token clears the latch and immediately resumes synchronization.

---

## 5. Native BAR Packaging & Verification (`bar-descriptor.xml`)

- **Package Identifier**: `com.todoist.passport`
- **Architecture**: `armle-v7`
- **Entry Executable**: `build/bb10/todoist-passport` (`type="Qnx/Elf"`)
- **Bundled Assets**:
  - `assets/main.qml`: 1440×1440 Cascades primary UI scene.
  - `assets/ActiveFrameCover.qml`: Minimized Active Frame `SceneCover` UI.
  - `assets/icon.png`: `144×144` launcher icon (`assets/icon.png`), as included in the release package.
  - `certs/cacert.pem`: Bundled Mozilla Root CA bundle (`vendor/sources/cacert.pem`).
  - `licenses/`: Bundled `LICENSE.txt`, `curl.txt`, and `openssl.txt`.
- **Permissions**: Minimal privilege boundary consisting exclusively of `run_native` and `access_internet`.
- **Automated Gate Verification (`scripts/check-package.py`)**: Verifies ZIP integrity, absence of Android `.apk`/`.dex` payloads, exact permission set, `armle-v7` manifest headers, presence of all required assets/certificates/licenses, and symbol isolation on `todoist-passport`.
