# Todoist on BlackBerry Passport: Current Architecture and Sync Reference

This document describes the implemented application. For downloads, installation
and first-run setup, use the [README](../README.md#installation). TLS API contracts
and runnable examples are in the [bb10_tls guide](../share/bb10_tls/README.md).

## 1. Product Scope

An unofficial native Todoist client for BlackBerry Passport, using C++03/POSIX
and Qt 4.8/Cascades. The tested release device runs BB10 OS 10.3.3.3216 / QNX 6.6
and is rooted. The release BAR is an unsigned development package; unrooted
installation has not been qualified.

The application supports task creation, editing, completion and deletion,
priorities, due dates, project selection, search, keyboard navigation, offline
changes and synchronization. Its minimized Active Frame shows task counts and a
short preview. A separate service that syncs after the app is terminated is not
implemented. Labels, subtasks, project administration and multi-account support
are not promised by the current UI.

In 1.0.8, in-app accidental-deletion recovery and a clearly highlighted active
tab are not implemented. They are the next-update priorities. Backspace is also
a task-deletion shortcut in the list; modal-sheet guards do not constitute a
complete solution to the reported text-editing conflict.

## 2. Authentication and Local Storage

Users supply their own personal API token through **Token -> Save**, then use
**Sync [R]**. Registration and token retrieval happen in the official Todoist web
app, not through a browser OAuth flow in this client.

The token is AES-encrypted before persistence in `sync_metadata` under
`api_token`. A private master-key file is managed by the application; this is not
a QNX system-keystore integration. The task database remains ordinary SQLite.

The storage implementation uses SQLite WAL transactions with tasks, command
queue (`sync_commands`), baselines, metadata and dead-letter commands
(`dead_letter_commands`). Project names and the user's inbox ID are stored in
metadata. See the storage and sync implementation for exact schema contracts;
this document does not prescribe a second schema.

## 3. Sync Protocol

The endpoint is `https://api.todoist.com/api/v1/sync`. Requests are HTTP POST
with bearer authentication and form URL-encoded `sync_token`, `resource_types`
and `commands`. The current resource selection is `["items", "projects", "user"]`.
The first full sync uses `sync_token=*`; later requests use the persisted cursor.

Local mutations atomically update task state and queue an outbound command.
Commands carry durable UUIDs and temporary IDs for offline-created tasks. At most
one unresolved command per task is claimed in a batch. A response is validated
before applying ID mappings, remote task changes, acknowledgments and the next
cursor in one checked outer transaction.

| Local operation | Wire command |
| :--- | :--- |
| Create | `item_add` |
| Edit | `item_update` |
| Complete | `item_close` |
| Reopen | `item_uncomplete` |
| Delete | `item_delete` |

Retries preserve command identity. Field-level merge uses stored baselines;
explicit remote deletions are authoritative. Dirty local tasks are protected
from full-sync absence pruning. Deleted tasks have no in-app user recovery flow
in the current release. Do not confuse reopening a completed task with recovering
a deleted task.

## 4. Transport and Concurrency

The shipped transport is static libcurl 8.23.0 with isolated OpenSSL 3.5.9,
bundled Mozilla CA roots, hostname verification and TLS 1.2/1.3 support. No
external gateway, proxy service, system Qt SSL fallback or Mbed TLS backend is
required by the application. `bb10_tls` is this project's reusable TLS subsystem,
with public-PKI and peer-fingerprint policies; Todoist uses public PKI.

`SyncWorkerThread` performs network I/O and response processing. It publishes
value snapshots to `SyncNotificationQueue`; the UI drains them on its timer.
The UI also performs local repository operations. `DatabaseManager` enforces
transaction ownership across these accesses; the database is not exclusively
owned by the network worker.

Local changes schedule a 3-second debounce. Manual sync and pull-to-refresh can
request an immediate cycle. Cold start sync requires a saved valid token and
connectivity. While minimized, polling is 5 minutes on Wi-Fi or 15 minutes on
cellular, paused offline. Worker-owned retry uses capped exponential backoff
with jitter; fatal authentication/configuration failures require corrective
user action rather than an endless retry loop.

## 5. Build and Packaging

Host checks use Linux x86_64, GCC/G++ or Clang and the dependencies listed in the
README. The native build uses Docker image `bb10-native-dev:local`, built from
[the included recipe](../docker/Dockerfile.bb10). It provides Clang-based
`bb10-cc`/`bb10-c++` drivers, BB10 10.3.1 SDK inputs, Qt 4.8/Cascades and the BAR
packager. The host needs no separate NDK installation.

```bash
python3 scripts/dependencies.py --fetch
./check.sh
docker build -t bb10-native-dev:local -f docker/Dockerfile.bb10 docker
./scripts/build-bb10.sh
```

Run these commands from the project root. The standard build output is
`build/bb10/todoist-passport.bar`; release attachments use the public version in
the filename. Rebuilding may change archive metadata and the checksum. A host
gate pass, ARM build and device runtime confirmation are distinct evidence.

## 6. Verification References

- Storage: `tests/storage/test_storage_all.cpp`
- Sync: `tests/sync/test_sync_all.cpp`
- TLS: `tests/tls/test_bb10_tls_all.cpp`
- Network: `tests/network/test_network_all.cpp`
- UI controller: `tests/ui/test_ui_all.cpp`
- Installed-package consumer: `tests/standalone_consumer/test_consumer.cpp`
- Legacy/modern crypto coexistence: `tests/target_coexistence/test_target_coexistence.cpp`
- BAR integrity and payload: `scripts/check-package.py`

These checks cover their configured scenarios. They do not replace device checks
for Cascades key routing, installation or visual behavior.
