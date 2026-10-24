#include "test_harness.hpp"
#include "todoist/db_manager.hpp"
#include "todoist/task_repository.hpp"
#include "todoist/sync_command_queue.hpp"
#include "todoist/temp_id_reconciler.hpp"
#include "todoist/sync_metadata_repo.hpp"
#include "todoist/uuid_util.hpp"
#include <cstdio>
#include <vector>
#include <map>
#include <pthread.h>

using namespace todoist;

bool testDatabaseInit() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());
    ASSERT_EQ(db.getSchemaVersion(), 1);
    db.close();
    return true;
}

bool testAtomicTaskCreation() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    Task t;
    t.content = "Buy groceries";
    t.description = "Milk, eggs, and bread";
    t.priority = 3;
    t.project_id = "inbox";
    t.due_date = "2026-10-15T18:00:00Z";

    std::string cmdUuid = "cmd-uuid-001";
    ASSERT_TRUE(repo.createTaskLocally(t, cmdUuid));

    // Verify task exists with temp_id
    ASSERT_FALSE(t.id.empty());
    ASSERT_EQ(t.sync_state, SyncState::LocallyCreated);

    Task fetched;
    ASSERT_TRUE(repo.getTaskById(t.id, fetched));
    ASSERT_EQ(fetched.content, "Buy groceries");
    ASSERT_EQ(fetched.description, "Milk, eggs, and bread");
    ASSERT_EQ(fetched.priority, 3);
    ASSERT_EQ(fetched.due_date, "2026-10-15T18:00:00Z");
    ASSERT_EQ(fetched.is_completed, false);

    // Verify sync_commands table has corresponding item_add command
    ASSERT_EQ(queue.getPendingCount(), 1);
    std::vector<SyncCommand> pending;
    ASSERT_TRUE(queue.getPendingCommands(pending));
    ASSERT_EQ(pending.size(), 1);
    ASSERT_EQ(pending[0].uuid, "cmd-uuid-001");
    ASSERT_EQ(pending[0].type, "item_add");
    ASSERT_EQ(pending[0].temp_id, t.id);

    return true;
}

bool testAtomicTaskUpdate() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    Task t;
    t.content = "Initial title";
    repo.createTaskLocally(t);

    ASSERT_TRUE(repo.updateTaskLocally(t.id, "Updated title", "Added details", 4, "2026-10-20", "cmd-uuid-002"));

    Task fetched;
    ASSERT_TRUE(repo.getTaskById(t.id, fetched));
    ASSERT_EQ(fetched.content, "Updated title");
    ASSERT_EQ(fetched.description, "Added details");
    ASSERT_EQ(fetched.priority, 4);
    ASSERT_EQ(fetched.due_date, "2026-10-20");

    // Queue now has 2 commands: item_add and item_update
    ASSERT_EQ(queue.getPendingCount(), 2);
    std::vector<SyncCommand> pending;
    ASSERT_TRUE(queue.getPendingCommands(pending));
    ASSERT_EQ(pending[1].uuid, "cmd-uuid-002");
    ASSERT_EQ(pending[1].type, "item_update");

    return true;
}

bool testAtomicTaskCompletion() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    Task t;
    t.content = "Complete me";
    repo.createTaskLocally(t);

    ASSERT_TRUE(repo.setTaskCompletedLocally(t.id, true, "cmd-uuid-003"));

    Task fetched;
    ASSERT_TRUE(repo.getTaskById(t.id, fetched));
    ASSERT_EQ(fetched.is_completed, true);

    // Active tasks query should exclude completed task
    std::vector<Task> activeTasks;
    ASSERT_TRUE(repo.getAllActiveTasks(activeTasks));
    ASSERT_EQ(activeTasks.size(), 0);

    return true;
}

bool testAtomicTaskDeletion() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    // Case 1: Locally created task deleted before sync
    Task localOnly;
    localOnly.content = "Mistake task";
    repo.createTaskLocally(localOnly);
    ASSERT_EQ(queue.getPendingCount(), 1);

    ASSERT_TRUE(repo.deleteTaskLocally(localOnly.id));
    // Should be cleanly deleted with NO pending commands remaining
    Task query;
    ASSERT_FALSE(repo.getTaskById(localOnly.id, query));
    ASSERT_EQ(queue.getPendingCount(), 0);

    // Case 2: Synced task deleted
    Task syncedTask;
    syncedTask.id = "server-id-888";
    syncedTask.server_id = "server-id-888";
    syncedTask.content = "Existing task on server";
    syncedTask.sync_state = SyncState::Synced;
    repo.upsertRemoteTask(syncedTask);

    ASSERT_TRUE(repo.deleteTaskLocally("server-id-888", "cmd-del-001"));
    ASSERT_TRUE(repo.getTaskById("server-id-888", query));
    ASSERT_EQ(query.sync_state, SyncState::LocallyDeleted);

    // An item_delete command should be queued
    ASSERT_EQ(queue.getPendingCount(), 1);
    std::vector<SyncCommand> pending;
    ASSERT_TRUE(queue.getPendingCommands(pending));
    ASSERT_EQ(pending[0].type, "item_delete");

    return true;
}

bool testTransactionRollbackSafety() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    // Simulate an aborted transaction
    {
        DatabaseManager::ScopedTransaction tx(db);
        // Direct execute without committing
        db.execute("INSERT INTO tasks (id, content, local_updated_at, sync_state) VALUES ('temp_fake', 'Fake', 1000, 'locally_created');");
        // tx destructs here without commit() -> must rollback
    }

    Task query;
    ASSERT_FALSE(repo.getTaskById("temp_fake", query));

    return true;
}

bool testCommandQueueLifecycle() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    Task t;
    t.content = "Test command queue";
    repo.createTaskLocally(t, "cmd-test-1");

    ASSERT_EQ(queue.getPendingCount(), 1);
    ASSERT_EQ(queue.getInFlightCount(), 0);

    // Transition to in_flight
    std::vector<std::string> uuids;
    uuids.push_back("cmd-test-1");
    ASSERT_TRUE(queue.markCommandsInFlight(uuids));
    ASSERT_EQ(queue.getPendingCount(), 0);
    ASSERT_EQ(queue.getInFlightCount(), 1);

    // Transient failure: resets to pending with incremented retry count
    ASSERT_TRUE(queue.markCommandFailedTransient("cmd-test-1", "HTTP 429 Rate Limited"));
    ASSERT_EQ(queue.getPendingCount(), 1);
    ASSERT_EQ(queue.getInFlightCount(), 0);

    std::vector<SyncCommand> pending;
    ASSERT_TRUE(queue.getPendingCommands(pending));
    ASSERT_EQ(pending[0].retry_count, 1);
    ASSERT_EQ(pending[0].last_error, "HTTP 429 Rate Limited");

    // Permanent failure: moves to dead letter queue
    ASSERT_TRUE(queue.markCommandFailedPermanent("cmd-test-1", "HTTP 400 Bad Request"));
    ASSERT_EQ(queue.getPendingCount(), 0);
    ASSERT_EQ(queue.getDeadLetterCount(), 1);

    return true;
}

bool testTempIdReconciliation() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);
    TempIdReconciler reconciler(db);

    // 1. Create task offline -> assigned temp_uuid_abc
    Task t;
    t.id = "temp_uuid_abc";
    t.content = "Offline created task";
    ASSERT_TRUE(repo.createTaskLocally(t, "cmd-add-1"));

    // 2. Edit description offline while still unacknowledged
    ASSERT_TRUE(repo.updateTaskLocally("temp_uuid_abc", "Offline created task", "Updated notes", 2, "", "cmd-update-1"));

    // 3. Mark completed offline
    ASSERT_TRUE(repo.setTaskCompletedLocally("temp_uuid_abc", true, "cmd-comp-1"));

    // Server returns temp_id_mapping
    std::map<std::string, std::string> mapping;
    mapping["temp_uuid_abc"] = "server_task_9999";

    ASSERT_TRUE(reconciler.reconcile(mapping));

    // Verify task table is remapped
    Task oldQuery;
    ASSERT_FALSE(repo.getTaskById("temp_uuid_abc", oldQuery));

    Task newQuery;
    ASSERT_TRUE(repo.getTaskById("server_task_9999", newQuery));
    ASSERT_EQ(newQuery.server_id, "server_task_9999");
    ASSERT_EQ(newQuery.content, "Offline created task");
    ASSERT_EQ(newQuery.description, "Updated notes");
    ASSERT_EQ(newQuery.is_completed, true);

    // Verify pending commands referencing temp_uuid_abc are updated
    std::vector<SyncCommand> pending;
    ASSERT_TRUE(queue.getPendingCommands(pending));
    ASSERT_EQ(pending.size(), 3);

    // The update command should now reference server_task_9999 in args_json
    ASSERT_TRUE(pending[1].args_json.find("server_task_9999") != std::string::npos);
    ASSERT_TRUE(pending[1].args_json.find("temp_uuid_abc") == std::string::npos);

    // The complete command should now reference server_task_9999 in args_json
    ASSERT_TRUE(pending[2].args_json.find("server_task_9999") != std::string::npos);
    ASSERT_TRUE(pending[2].args_json.find("temp_uuid_abc") == std::string::npos);

    return true;
}

bool testDeltaIngestionAndToken() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);
    SyncMetadataRepository metaRepo(db, repo, queue);

    ASSERT_EQ(metaRepo.getSyncToken(), "*");

    // Create a local command that will be acknowledged in this delta
    Task localT;
    repo.createTaskLocally(localT, "cmd-to-ack");
    ASSERT_EQ(queue.getPendingCount(), 1);

    // Server sends: 2 upserted tasks, 0 deleted tasks, 1 acknowledged command, new sync_token
    Task r1;
    r1.id = "srv-1";
    r1.server_id = "srv-1";
    r1.content = "Remote task 1";
    r1.sync_state = SyncState::Synced;

    Task r2;
    r2.id = "srv-2";
    r2.server_id = "srv-2";
    r2.content = "Remote task 2";
    r2.sync_state = SyncState::Synced;

    std::vector<Task> upserted;
    upserted.push_back(r1);
    upserted.push_back(r2);
    std::vector<std::string> deleted;
    std::vector<std::string> acked;
    acked.push_back("cmd-to-ack");
    std::string nextSyncToken = "sync_token_v1_cursor_abcdef";

    ASSERT_TRUE(metaRepo.applyDeltaAndSaveToken(upserted, deleted, acked, nextSyncToken));

    // Verify token updated
    ASSERT_EQ(metaRepo.getSyncToken(), "sync_token_v1_cursor_abcdef");

    // Verify tasks exist locally
    Task q1, q2;
    ASSERT_TRUE(repo.getTaskById("srv-1", q1));
    ASSERT_EQ(q1.content, "Remote task 1");
    ASSERT_TRUE(repo.getTaskById("srv-2", q2));
    ASSERT_EQ(q2.content, "Remote task 2");

    // Verify acknowledged command removed
    ASSERT_EQ(queue.getPendingCount(), 0);

    return true;
}

bool testComplexJsonEscaping() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository repo(db);
    SyncCommandQueue queue(db);

    Task t;
    t.content = "Buy \"Milk\" & Bread\nSpecial characters: \\ and \t tab";
    t.description = "Line 1\nLine 2 with \"quotes\" and \\ slashes";
    ASSERT_TRUE(repo.createTaskLocally(t, "cmd-esc-01"));

    Task query;
    ASSERT_TRUE(repo.getTaskById(t.id, query));
    ASSERT_EQ(query.content, "Buy \"Milk\" & Bread\nSpecial characters: \\ and \t tab");
    ASSERT_EQ(query.description, "Line 1\nLine 2 with \"quotes\" and \\ slashes");

    std::vector<SyncCommand> pending;
    ASSERT_TRUE(queue.getPendingCommands(pending));
    ASSERT_EQ(pending.size(), 1);
    ASSERT_TRUE(pending[0].args_json.find("\\\"Milk\\\"") != std::string::npos);

    return true;
}

bool testOnDiskWalMode() {
    const std::string dbFile = "test_wal_todoist.db";
    std::remove(dbFile.c_str());

    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(dbFile));
        ASSERT_TRUE(db.initializeSchema());

        TaskRepository repo(db);
        Task t;
        t.content = "Persisted task on disk";
        ASSERT_TRUE(repo.createTaskLocally(t));
        db.close();
    }

    // Reopen and verify persistence
    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(dbFile));
        TaskRepository repo(db);
        std::vector<Task> tasks;
        ASSERT_TRUE(repo.getAllActiveTasks(tasks));
        ASSERT_EQ(tasks.size(), 1);
        ASSERT_EQ(tasks[0].content, "Persisted task on disk");
        db.close();
    }

    std::remove(dbFile.c_str());
    return true;
}

// --- M1 Round 1 Review Regression Tests (R001 - R011) ---

static int releaseBlockerHandler(void* p, int) {
    static_cast<DatabaseManager*>(p)->rollback();
    return 0;
}

// R001: Writes must not continue after transaction begin failure
bool testR001FailedBeginDoesNotPersist() {
    const std::string path = "test_r001_lock.db";
    std::remove(path.c_str());

    DatabaseManager db, blocker;
    ASSERT_TRUE(db.open(path));
    ASSERT_TRUE(db.initializeSchema());
    ASSERT_TRUE(blocker.open(path));

    TaskRepository repo(db);
    Task first;
    ASSERT_TRUE(repo.createTaskLocally(first, "dup-cmd-uuid"));

    // Hold an immediate transaction on blocker connection
    ASSERT_TRUE(blocker.beginImmediate());
    sqlite3_busy_handler(db.handle(), releaseBlockerHandler, &blocker);

    Task second;
    bool result = repo.createTaskLocally(second, "dup-cmd-uuid");
    ASSERT_FALSE(result);

    Task fetched;
    ASSERT_FALSE(repo.getTaskById(second.id, fetched));

    blocker.close();
    db.close();
    std::remove(path.c_str());
    return true;
}

// R002: Separate transaction ownership across threads
struct WorkerPayloadR002 {
    DatabaseManager* db;
    bool result;
    std::string id;
};

static void* workerR002(void* p) {
    WorkerPayloadR002* w = static_cast<WorkerPayloadR002*>(p);
    TaskRepository r(*w->db);
    Task t;
    w->result = r.createTaskLocally(t, "worker-command");
    w->id = t.id;
    return NULL;
}

bool testR002CrossThreadIsolation() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());
    ASSERT_TRUE(db.beginImmediate());

    WorkerPayloadR002 w;
    w.db = &db;
    w.result = false;
    pthread_t thread;
    pthread_create(&thread, NULL, workerR002, &w);
    pthread_join(thread, NULL);

    ASSERT_FALSE(w.result);
    ASSERT_TRUE(db.rollback());

    TaskRepository r(db);
    Task fetched;
    ASSERT_FALSE(r.getTaskById(w.id, fetched));
    return true;
}

// R003: Preserve deletion intent for an in-flight created task
bool testR003InFlightTaskDeletionPreservesIntent() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);

    Task t;
    ASSERT_TRUE(r.createTaskLocally(t, "inflight-add-cmd"));
    std::vector<std::string> ids;
    ids.push_back("inflight-add-cmd");
    ASSERT_TRUE(q.markCommandsInFlight(ids));

    ASSERT_TRUE(r.deleteTaskLocally(t.id, "delete-cmd"));

    ASSERT_EQ(q.getInFlightCount(), 1);
    ASSERT_EQ(q.getPendingCount(), 1);

    Task fetched;
    ASSERT_TRUE(r.getTaskById(t.id, fetched));
    ASSERT_EQ(fetched.sync_state, SyncState::LocallyDeleted);
    return true;
}

// R004: Cancel commands by task reference, not JSON text
bool testR004CancelCommandsByTaskReference() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);

    Task a, b;
    ASSERT_TRUE(r.createTaskLocally(a, "add-a"));
    b.content = a.id;
    ASSERT_TRUE(r.createTaskLocally(b, "add-b"));

    ASSERT_TRUE(r.deleteTaskLocally(a.id));

    Task fetchedB;
    ASSERT_TRUE(r.getTaskById(b.id, fetchedB));
    ASSERT_EQ(q.getPendingCount(), 1);

    std::vector<SyncCommand> pending;
    ASSERT_TRUE(q.getPendingCommands(pending));
    ASSERT_EQ(pending[0].uuid, "add-b");
    return true;
}

// R005: Reconcile structural ID fields only
bool testR005ReconcileStructuralIdOnly() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);
    TempIdReconciler rec(db);

    Task a, b;
    a.id = "temp_uuid_a";
    ASSERT_TRUE(r.createTaskLocally(a, "add-a"));
    b.content = "literal temp_uuid_a suffix";
    ASSERT_TRUE(r.createTaskLocally(b, "add-b"));

    std::map<std::string, std::string> mapping;
    mapping[a.id] = "server-a";
    ASSERT_TRUE(rec.reconcile(mapping));

    std::vector<SyncCommand> pending;
    ASSERT_TRUE(q.getPendingCommands(pending));
    bool foundB = false;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (pending[i].uuid == "add-b") {
            foundB = true;
            ASSERT_TRUE(pending[i].args_json.find("literal temp_uuid_a suffix") != std::string::npos);
        }
    }
    ASSERT_TRUE(foundB);
    return true;
}

// R006: Propagate every SQLite batch execution failure
bool testR006BatchFailureRollback() {
    // 1. Remap collision failure
    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(":memory:"));
        ASSERT_TRUE(db.initializeSchema());

        TaskRepository r(db);
        SyncCommandQueue q(db);
        TempIdReconciler rec(db);

        Task a;
        a.id = "temp_uuid_a";
        ASSERT_TRUE(r.createTaskLocally(a, "add-a"));

        Task b;
        b.id = "server-a";
        b.server_id = "server-a";
        b.content = "Existing remote";
        ASSERT_TRUE(r.upsertRemoteTask(b));

        std::map<std::string, std::string> mapping;
        mapping[a.id] = "server-a";
        bool result = rec.reconcile(mapping);
        ASSERT_FALSE(result);

        Task fetchedA;
        ASSERT_TRUE(r.getTaskById("temp_uuid_a", fetchedA));
        std::vector<SyncCommand> pending;
        ASSERT_TRUE(q.getPendingCommands(pending));
        ASSERT_EQ(pending[0].temp_id, "temp_uuid_a");
    }

    // 2. Acknowledged batch failure rolls back cursor advance
    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(":memory:"));
        ASSERT_TRUE(db.initializeSchema());

        TaskRepository r(db);
        SyncCommandQueue q(db);
        SyncMetadataRepository meta(db, r, q);

        Task t;
        ASSERT_TRUE(r.createTaskLocally(t, "ack-cmd-1"));
        ASSERT_TRUE(db.execute("CREATE TRIGGER block_ack BEFORE DELETE ON sync_commands BEGIN SELECT RAISE(ABORT, 'injected failure'); END;"));

        std::vector<Task> updates;
        std::vector<std::string> deleted;
        std::vector<std::string> acked;
        acked.push_back("ack-cmd-1");

        bool result = meta.applyDeltaAndSaveToken(updates, deleted, acked, "advanced-token");
        ASSERT_FALSE(result);
        ASSERT_EQ(q.getPendingCount(), 1);
        ASSERT_EQ(meta.getSyncToken(), "*");
    }
    return true;
}

// R007: Mapping must retain pending local edits
bool testR007MappingRetainsPendingEdits() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);
    TempIdReconciler rec(db);

    Task t;
    t.id = "temp_uuid_a";
    t.content = "original title";
    ASSERT_TRUE(r.createTaskLocally(t, "add-a"));
    ASSERT_TRUE(r.updateTaskLocally(t.id, "new local title", "", 1, "", "update-a"));

    std::map<std::string, std::string> mapping;
    mapping[t.id] = "server-a";
    ASSERT_TRUE(rec.reconcile(mapping));
    ASSERT_TRUE(q.markCommandAcknowledged("add-a"));

    Task server;
    server.id = "server-a";
    server.server_id = "server-a";
    server.content = "original title";
    ASSERT_TRUE(r.upsertRemoteTask(server));

    Task fetched;
    ASSERT_TRUE(r.getTaskById("server-a", fetched));
    ASSERT_EQ(fetched.content, "new local title");
    ASSERT_EQ(fetched.sync_state, SyncState::LocallyUpdated);
    ASSERT_EQ(q.getPendingCount(), 1);
    return true;
}

// R008: Preserve independent field changes in remote and local updates
bool testR008FieldLevelNonConflictingMerge() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);

    Task t;
    t.id = "server-a";
    t.server_id = "server-a";
    t.content = "original";
    t.local_updated_at = 1;
    ASSERT_TRUE(r.upsertRemoteTask(t));

    ASSERT_TRUE(r.updateTaskLocally(t.id, "local title", "", 1, "", "local-update"));

    Task fetched;
    ASSERT_TRUE(r.getTaskById(t.id, fetched));

    t.due_date = "2026-10-20";
    t.local_updated_at = fetched.local_updated_at - 1;
    ASSERT_TRUE(r.upsertRemoteTask(t));

    Task merged;
    ASSERT_TRUE(r.getTaskById(t.id, merged));
    ASSERT_EQ(merged.content, "local title");
    ASSERT_EQ(merged.due_date, "2026-10-20");
    ASSERT_EQ(q.getPendingCount(), 1);
    ASSERT_EQ(merged.sync_state, SyncState::LocallyUpdated);
    return true;
}

// R009: Keep transaction state recoverable after failed commit
bool testR009FailedCommitRollbackRecoverability() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.execute("CREATE TABLE parent(id INTEGER PRIMARY KEY); CREATE TABLE child(parent_id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED);"));

    ASSERT_TRUE(db.beginImmediate());
    ASSERT_TRUE(db.execute("INSERT INTO child VALUES(999);"));
    bool commitSuccess = db.commit();
    ASSERT_FALSE(commitSuccess);

    bool rollbackSuccess = db.rollback();
    ASSERT_TRUE(rollbackSuccess);
    ASSERT_EQ(sqlite3_get_autocommit(db.handle()), 1);

    ASSERT_TRUE(db.beginImmediate());
    ASSERT_TRUE(db.execute("INSERT INTO parent VALUES(1);"));
    ASSERT_TRUE(db.commit());
    return true;
}

// R010: Represent explicit due-date removal in outbound command
bool testR010DueDateExplicitRemoval() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);

    Task t;
    t.id = "server-a";
    t.server_id = "server-a";
    t.content = "original";
    t.due_date = "2026-10-20";
    ASSERT_TRUE(r.upsertRemoteTask(t));

    ASSERT_TRUE(r.updateTaskLocally(t.id, t.content, "", 1, "", "clear-due"));

    std::vector<SyncCommand> cmds;
    ASSERT_TRUE(q.getPendingCommands(cmds));
    ASSERT_EQ(cmds.size(), 1);
    ASSERT_TRUE(cmds[0].args_json.find("\"due_date\":null") != std::string::npos);
    return true;
}

// R011: Avoid re-locking mutex during database reopen with active transaction
bool testR011ReopenActiveTransactionDoesNotDeadlock() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());
    ASSERT_TRUE(db.beginImmediate());

    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());
    return true;
}

// R002: Standalone queue writes respect transaction ownership across threads
struct StandaloneAckPayload {
    DatabaseManager* db;
    bool result;
};

static void* standaloneAckWorker(void* p) {
    StandaloneAckPayload* w = static_cast<StandaloneAckPayload*>(p);
    SyncCommandQueue q(*w->db);
    w->result = q.markCommandAcknowledged("cmd-ack-test");
    return NULL;
}

bool testR002StandaloneQueueWriteIsolation() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);

    Task t;
    ASSERT_TRUE(r.createTaskLocally(t, "cmd-ack-test"));
    ASSERT_EQ(q.getPendingCount(), 1);

    // Thread A starts a transaction
    ASSERT_TRUE(db.beginImmediate());

    // Thread B attempts standalone acknowledge while Thread A holds transaction
    StandaloneAckPayload w;
    w.db = &db;
    w.result = true;
    pthread_t thread;
    pthread_create(&thread, NULL, standaloneAckWorker, &w);
    pthread_join(thread, NULL);

    // Thread B must be rejected while foreign transaction is active
    ASSERT_FALSE(w.result);

    // Thread A rolls back; command must remain pending
    ASSERT_TRUE(db.rollback());
    ASSERT_EQ(q.getPendingCount(), 1);

    // When no transaction is held, standalone acknowledge succeeds
    ASSERT_TRUE(q.markCommandAcknowledged("cmd-ack-test"));
    ASSERT_EQ(q.getPendingCount(), 0);
    return true;
}

// R006: Baseline statement failures roll back atomic delta batch
bool testR006BaselineStatementFailureRollback() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);
    SyncMetadataRepository meta(db, r, q);

    Task remote;
    remote.id = "server-a";
    remote.server_id = "server-a";
    remote.content = "base task";
    remote.local_updated_at = 1;

    // Injected trigger causing failure on task_baselines INSERT
    ASSERT_TRUE(db.execute(
        "CREATE TRIGGER fail_baseline_insert BEFORE INSERT ON task_baselines "
        "BEGIN SELECT RAISE(ABORT, 'injected baseline insert failure'); END;"));

    std::vector<Task> tasks(1, remote);
    std::vector<Task> emptyTasks;
    std::vector<std::string> empty;
    bool result = meta.applyDeltaAndSaveToken(tasks, empty, empty, "advanced-token");
    ASSERT_FALSE(result);
    ASSERT_EQ(meta.getSyncToken(), "*");

    Task fetched;
    ASSERT_FALSE(r.getTaskById(remote.id, fetched));

    // Remove trigger, then verify delta ingestion succeeds
    ASSERT_TRUE(db.execute("DROP TRIGGER fail_baseline_insert;"));
    result = meta.applyDeltaAndSaveToken(tasks, empty, empty, "advanced-token");
    ASSERT_TRUE(result);
    ASSERT_EQ(meta.getSyncToken(), "advanced-token");
    ASSERT_TRUE(r.getTaskById(remote.id, fetched));

    // Injected trigger causing failure on task_baselines DELETE
    ASSERT_TRUE(db.execute(
        "CREATE TRIGGER fail_baseline_delete BEFORE DELETE ON task_baselines "
        "BEGIN SELECT RAISE(ABORT, 'injected baseline delete failure'); END;"));
    std::vector<std::string> deleted(1, remote.id);
    result = meta.applyDeltaAndSaveToken(emptyTasks, deleted, empty, "deleted-token");
    ASSERT_FALSE(result);
    ASSERT_EQ(meta.getSyncToken(), "advanced-token");
    ASSERT_TRUE(r.getTaskById(remote.id, fetched));

    return true;
}

// R008: Outgoing intent alignment and missing-baseline fallback
bool testR008OutgoingIntentAndMissingBaseline() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);

    Task t;
    t.id = "server-a";
    t.server_id = "server-a";
    t.content = "base";
    t.local_updated_at = 1;
    ASSERT_TRUE(r.upsertRemoteTask(t));

    // 1. Title edit only: outbound command does NOT clear due date
    ASSERT_TRUE(r.updateTaskLocally(t.id, "local title", "", 1, "", "update-1"));
    std::vector<SyncCommand> cmds;
    ASSERT_TRUE(q.getPendingCommands(cmds));
    ASSERT_EQ(cmds.size(), 1);
    ASSERT_TRUE(cmds[0].args_json.find("\"due_date\":null") == std::string::npos);

    // Remote brings independent due date
    Task remoteDue = t;
    remoteDue.due_date = "2026-10-20";
    Task localTask;
    ASSERT_TRUE(r.getTaskById(t.id, localTask));
    remoteDue.local_updated_at = localTask.local_updated_at - 1;
    ASSERT_TRUE(r.upsertRemoteTask(remoteDue));

    ASSERT_TRUE(r.getTaskById(t.id, localTask));
    ASSERT_EQ(localTask.due_date, "2026-10-20");
    ASSERT_EQ(localTask.content, "local title");

    // 2. Newer remote title wins: outbound command aligns with remote winner
    Task remoteNewer = remoteDue;
    remoteNewer.content = "remote newer title";
    remoteNewer.local_updated_at = localTask.local_updated_at + 100;
    ASSERT_TRUE(r.upsertRemoteTask(remoteNewer));

    ASSERT_TRUE(r.getTaskById(t.id, localTask));
    ASSERT_EQ(localTask.content, "remote newer title");

    ASSERT_TRUE(q.getPendingCommands(cmds));
    ASSERT_EQ(cmds.size(), 1);
    ASSERT_TRUE(cmds[0].args_json.find("remote newer title") != std::string::npos);

    // 3. Missing baseline: unsent local edit is preserved against older remote delta
    Task taskNoBase;
    taskNoBase.id = "server-b";
    taskNoBase.server_id = "server-b";
    taskNoBase.content = "original";
    taskNoBase.local_updated_at = 1;
    ASSERT_TRUE(r.upsertRemoteTask(taskNoBase));

    ASSERT_TRUE(r.updateTaskLocally(taskNoBase.id, "unsent title", "", 1, "", "update-2"));
    ASSERT_TRUE(db.execute("DELETE FROM task_baselines;"));

    // Older remote delta arrives
    ASSERT_TRUE(r.upsertRemoteTask(taskNoBase));
    Task fetchedNoBase;
    ASSERT_TRUE(r.getTaskById(taskNoBase.id, fetchedNoBase));
    ASSERT_EQ(fetchedNoBase.content, "unsent title");

    return true;
}

// R006: Queue alignment statement failure rolls back atomic delta batch
bool testR006QueueAlignmentFailureRollback() {
    DatabaseManager db;
    ASSERT_TRUE(db.open(":memory:"));
    ASSERT_TRUE(db.initializeSchema());

    TaskRepository r(db);
    SyncCommandQueue q(db);
    SyncMetadataRepository meta(db, r, q);

    Task remote;
    remote.id = "server-a";
    remote.server_id = "server-a";
    remote.content = "base title";
    remote.local_updated_at = 1;
    ASSERT_TRUE(r.upsertRemoteTask(remote));

    ASSERT_TRUE(r.updateTaskLocally(remote.id, "local title", "", 1, "", "update-align-cmd"));
    ASSERT_TRUE(meta.setSyncToken("old-token"));

    // Inject trigger causing failure when updating args_json on sync_commands during alignment
    ASSERT_TRUE(db.execute(
        "CREATE TRIGGER fail_align BEFORE UPDATE OF args_json ON sync_commands "
        "BEGIN SELECT RAISE(ABORT, 'injected alignment failure'); END;"));

    Task localTask;
    ASSERT_TRUE(r.getTaskById(remote.id, localTask));

    Task winningRemote = remote;
    winningRemote.content = "remote winner";
    winningRemote.local_updated_at = localTask.local_updated_at + 1000;

    std::vector<Task> tasks(1, winningRemote);
    std::vector<std::string> empty;
    bool result = meta.applyDeltaAndSaveToken(tasks, empty, empty, "advanced-token");

    // Must fail and roll back everything
    ASSERT_FALSE(result);
    ASSERT_EQ(meta.getSyncToken(), "old-token");

    Task localAfter;
    ASSERT_TRUE(r.getTaskById(remote.id, localAfter));
    ASSERT_EQ(localAfter.content, "local title");

    std::vector<SyncCommand> cmds;
    ASSERT_TRUE(q.getPendingCommands(cmds));
    ASSERT_EQ(cmds.size(), 1);
    ASSERT_TRUE(cmds[0].args_json.find("\"local title\"") != std::string::npos);

    // Drop trigger, then verify delta application succeeds
    ASSERT_TRUE(db.execute("DROP TRIGGER fail_align;"));
    result = meta.applyDeltaAndSaveToken(tasks, empty, empty, "advanced-token");
    ASSERT_TRUE(result);
    ASSERT_EQ(meta.getSyncToken(), "advanced-token");

    ASSERT_TRUE(r.getTaskById(remote.id, localAfter));
    ASSERT_EQ(localAfter.content, "remote winner");

    ASSERT_TRUE(q.getPendingCommands(cmds));
    ASSERT_EQ(cmds.size(), 1);
    ASSERT_TRUE(cmds[0].args_json.find("\"remote winner\"") != std::string::npos);

    return true;
}

// R008: Retried commands preserve payload; in-flight conflicts enqueue fresh corrective commands
bool testR008RetryAndInFlightWinningRemote() {
    // 1. Transient failure / retried command preserves original payload and UUID
    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(":memory:"));
        ASSERT_TRUE(db.initializeSchema());

        TaskRepository r(db);
        SyncCommandQueue q(db);

        Task remote;
        remote.id = "server-a";
        remote.server_id = "server-a";
        remote.content = "base";
        remote.local_updated_at = 1;
        ASSERT_TRUE(r.upsertRemoteTask(remote));

        ASSERT_TRUE(r.updateTaskLocally(remote.id, "local title", "", 1, "", "cmd-retry-1"));

        std::vector<std::string> inFlightIds(1, "cmd-retry-1");
        ASSERT_TRUE(q.markCommandsInFlight(inFlightIds));
        ASSERT_TRUE(q.markCommandFailedTransient("cmd-retry-1", "connection dropped"));

        Task localTask;
        ASSERT_TRUE(r.getTaskById(remote.id, localTask));

        Task winningRemote = remote;
        winningRemote.content = "remote winner";
        winningRemote.local_updated_at = localTask.local_updated_at + 1000;
        ASSERT_TRUE(r.upsertRemoteTask(winningRemote));

        std::vector<SyncCommand> pending;
        ASSERT_TRUE(q.getPendingCommands(pending));
        // Expect cmd-retry-1 (with original payload, retry_count = 1) + 1 corrective command
        ASSERT_EQ(pending.size(), 2);

        bool foundOrig = false;
        bool foundCorrect = false;
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].uuid == "cmd-retry-1") {
                foundOrig = true;
                ASSERT_EQ(pending[i].retry_count, 1);
                ASSERT_TRUE(pending[i].args_json.find("\"local title\"") != std::string::npos);
            } else {
                foundCorrect = true;
                ASSERT_EQ(pending[i].retry_count, 0);
                ASSERT_TRUE(pending[i].args_json.find("\"remote winner\"") != std::string::npos);
            }
        }
        ASSERT_TRUE(foundOrig);
        ASSERT_TRUE(foundCorrect);
    }

    // 2. In-flight command preserves payload while fresh corrective command is enqueued
    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(":memory:"));
        ASSERT_TRUE(db.initializeSchema());

        TaskRepository r(db);
        SyncCommandQueue q(db);

        Task remote;
        remote.id = "server-b";
        remote.server_id = "server-b";
        remote.content = "base";
        remote.local_updated_at = 1;
        ASSERT_TRUE(r.upsertRemoteTask(remote));

        ASSERT_TRUE(r.updateTaskLocally(remote.id, "local title b", "", 1, "", "cmd-inflight-2"));

        std::vector<std::string> inFlightIds(1, "cmd-inflight-2");
        ASSERT_TRUE(q.markCommandsInFlight(inFlightIds));

        Task localTask;
        ASSERT_TRUE(r.getTaskById(remote.id, localTask));

        Task winningRemote = remote;
        winningRemote.content = "remote winner b";
        winningRemote.local_updated_at = localTask.local_updated_at + 1000;
        ASSERT_TRUE(r.upsertRemoteTask(winningRemote));

        Task fetched;
        ASSERT_TRUE(r.getTaskById(remote.id, fetched));
        ASSERT_EQ(fetched.content, "remote winner b");

        // 1. While predecessor is in_flight, the corrective command is blocked
        ASSERT_EQ(q.getPendingCount(), 1);
        std::vector<SyncCommand> pending;
        ASSERT_TRUE(q.getPendingCommands(pending));
        ASSERT_EQ(pending.size(), 0);

        // 2. When predecessor fails transiently, old retry is selected first
        ASSERT_TRUE(q.markCommandFailedTransient("cmd-inflight-2", "response lost"));
        ASSERT_EQ(q.getPendingCount(), 2);
        ASSERT_TRUE(q.getPendingCommands(pending));
        ASSERT_EQ(pending.size(), 2);
        ASSERT_EQ(pending[0].uuid, "cmd-inflight-2");
        ASSERT_TRUE(pending[0].args_json.find("\"local title b\"") != std::string::npos);

        // 3. Mark old command in flight again and verify correction is blocked once more
        std::vector<std::string> retryFlight(1, "cmd-inflight-2");
        ASSERT_TRUE(q.markCommandsInFlight(retryFlight));
        ASSERT_TRUE(q.getPendingCommands(pending));
        ASSERT_EQ(pending.size(), 0);

        // 4. Resolve predecessor: acknowledge old command
        ASSERT_TRUE(q.markCommandAcknowledged("cmd-inflight-2"));

        // 5. After predecessor resolves, corrective command becomes eligible
        ASSERT_EQ(q.getPendingCount(), 1);
        ASSERT_TRUE(q.getPendingCommands(pending));
        ASSERT_EQ(pending.size(), 1);
        ASSERT_TRUE(pending[0].uuid != "cmd-inflight-2");
        ASSERT_TRUE(pending[0].args_json.find("\"remote winner b\"") != std::string::npos);

        // 6. Acknowledge corrective command and verify clean finish
        ASSERT_TRUE(q.markCommandAcknowledged(pending[0].uuid));
        ASSERT_EQ(q.getPendingCount(), 0);
        ASSERT_TRUE(r.getTaskById(remote.id, fetched));
        ASSERT_EQ(fetched.content, "remote winner b");
    }

    return true;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Todoist Storage Engine - Test Suite (M1)" << std::endl;
    std::cout << "========================================" << std::endl;

    RUN_TEST(testDatabaseInit);
    RUN_TEST(testAtomicTaskCreation);
    RUN_TEST(testAtomicTaskUpdate);
    RUN_TEST(testAtomicTaskCompletion);
    RUN_TEST(testAtomicTaskDeletion);
    RUN_TEST(testTransactionRollbackSafety);
    RUN_TEST(testCommandQueueLifecycle);
    RUN_TEST(testTempIdReconciliation);
    RUN_TEST(testDeltaIngestionAndToken);
    RUN_TEST(testComplexJsonEscaping);
    RUN_TEST(testOnDiskWalMode);

    RUN_TEST(testR001FailedBeginDoesNotPersist);
    RUN_TEST(testR002CrossThreadIsolation);
    RUN_TEST(testR002StandaloneQueueWriteIsolation);
    RUN_TEST(testR003InFlightTaskDeletionPreservesIntent);
    RUN_TEST(testR004CancelCommandsByTaskReference);
    RUN_TEST(testR005ReconcileStructuralIdOnly);
    RUN_TEST(testR006BatchFailureRollback);
    RUN_TEST(testR006BaselineStatementFailureRollback);
    RUN_TEST(testR006QueueAlignmentFailureRollback);
    RUN_TEST(testR007MappingRetainsPendingEdits);
    RUN_TEST(testR008FieldLevelNonConflictingMerge);
    RUN_TEST(testR008OutgoingIntentAndMissingBaseline);
    RUN_TEST(testR008RetryAndInFlightWinningRemote);
    RUN_TEST(testR009FailedCommitRollbackRecoverability);
    RUN_TEST(testR010DueDateExplicitRemoval);
    RUN_TEST(testR011ReopenActiveTransactionDoesNotDeadlock);

    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Tests run: " << g_testsRun
              << ", Passed: " << g_testsPassed
              << ", Failed: " << g_testsFailed << std::endl;

    return (g_testsFailed == 0) ? 0 : 1;
}
