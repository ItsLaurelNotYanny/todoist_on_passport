#include "todoist/ui/todoist_app_controller.hpp"
#include "todoist/uuid_util.hpp"
#include "test_harness.hpp"

#include <stdio.h>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace todoist;

namespace {

std::string createTempUiDbPath(const char* prefix) {
    char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/todoist_test_ui_%s_%ld.db",
             prefix, static_cast<long>(getpid()));
    return std::string(buf);
}

void cleanupDbFiles(const std::string& path) {
    ::remove(path.c_str());
    ::remove((path + "-wal").c_str());
    ::remove((path + "-shm").c_str());
}

class MockUiSyncTransport : public ISyncTransport {
public:
    MockUiSyncTransport()
        : statusCode(200)
        , responseBody("{\"sync_token\":\"tok-ui-1\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}")
        , errorMessage("")
        , callCount(0)
    {
        pthread_mutex_init(&m_mutex, NULL);
    }

    virtual ~MockUiSyncTransport() {
        pthread_mutex_destroy(&m_mutex);
    }

    void configureResponse(int code, const std::string& body, const std::string& err = "") {
        pthread_mutex_lock(&m_mutex);
        statusCode = code;
        responseBody = body;
        errorMessage = err;
        pthread_mutex_unlock(&m_mutex);
    }

    int getCallCount() {
        pthread_mutex_lock(&m_mutex);
        int c = callCount;
        pthread_mutex_unlock(&m_mutex);
        return c;
    }

    virtual HttpResponse executeSyncPost(const std::string& endpointUrl,
                                         const std::string& requestBody,
                                         const std::string& bearerToken) {
        pthread_mutex_lock(&m_mutex);
        ++callCount;
        lastEndpointUrl = endpointUrl;
        lastRequestBody = requestBody;
        lastBearerToken = bearerToken;

        HttpResponse resp;
        resp.statusCode = statusCode;
        resp.body = responseBody;
        resp.errorMessage = errorMessage;
        pthread_mutex_unlock(&m_mutex);
        return resp;
    }

private:
    pthread_mutex_t m_mutex;
    int statusCode;
    std::string responseBody;
    std::string errorMessage;
    int callCount;
    std::string lastEndpointUrl;
    std::string lastRequestBody;
    std::string lastBearerToken;
};

bool waitForWorkerDrain(TodoistAppController& ctrl, uint64_t nowMs, int maxWaitMs = 3000) {
    int elapsed = 0;
    while (elapsed < maxWaitMs) {
        ctrl.tick(nowMs);
        if (!ctrl.isSyncing() && ctrl.workerThread().getState() != WORKER_STATE_SYNCING) {
            ctrl.tick(nowMs);
            return true;
        }
        usleep(15000);
        elapsed += 15;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Test 1: Cold-Start Initialization, Crash Recovery & Token Persistence
// ---------------------------------------------------------------------------
bool testColdStartRecoveryAndTokenPersistence() {
    std::string dbPath = createTempUiDbPath("cold_start");
    cleanupDbFiles(dbPath);

    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(dbPath));
        ASSERT_TRUE(db.initializeSchema());

        TaskRepository repo(db);
        SyncCommandQueue queue(db);
        SyncMetadataRepository meta(db, repo, queue);

        Task t;
        t.id = "temp_uuid_crash_1";
        t.content = "Recovered task after crash";
        t.priority = 3;
        t.project_id = "inbox";
        ASSERT_TRUE(repo.createTaskLocally(t, "cmd-crash-1"));

        std::vector<std::string> uuids;
        uuids.push_back("cmd-crash-1");
        ASSERT_TRUE(queue.markCommandsInFlight(uuids));
        ASSERT_EQ(1, queue.getInFlightCount());

        ASSERT_TRUE(meta.setMetadata("api_token", "secret_todoist_token_abcd"));
        db.close();
    }

    {
        DatabaseManager db;
        ASSERT_TRUE(db.open(dbPath));
        MockUiSyncTransport transport;
        transport.configureResponse(
            200,
            "{\"sync_token\":\"tok-cold-1\",\"full_sync\":false,\"items\":[],"
            "\"temp_id_mapping\":{\"temp_uuid_crash_1\":\"srv-101\"},"
            "\"sync_status\":{\"cmd-crash-1\":\"ok\"}}"
        );

        TodoistAppController ctrl(db, transport);
        ASSERT_TRUE(ctrl.initialize(1000, true));
        ASSERT_TRUE(ctrl.hasApiToken());
        ASSERT_EQ(std::string("••••••••abcd"), ctrl.getMaskedApiToken());
        ASSERT_EQ(1, ctrl.getTotalSyncTriggers());

        ASSERT_TRUE(waitForWorkerDrain(ctrl, 1100));
        ASSERT_EQ(std::string("Synced"), ctrl.getStatusText());
        ASSERT_EQ(0, ctrl.getPendingCommandCount());
        ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
        ASSERT_EQ(std::string("srv-101"), ctrl.getVisibleTasks()[0].id);

        // Reject invalid tokens (empty or containing newline/spaces)
        ASSERT_FALSE(ctrl.setApiToken("   ", 1200));
        ASSERT_FALSE(ctrl.setApiToken("bad\ntoken", 1200));
        ASSERT_TRUE(ctrl.hasApiToken());

        // Clear token
        ASSERT_TRUE(ctrl.clearApiToken());
        ASSERT_FALSE(ctrl.hasApiToken());
        ASSERT_TRUE(ctrl.requiresAuth());
        ASSERT_EQ(std::string("API token required"), ctrl.getStatusText());

        ctrl.shutdown();
        db.close();
    }

    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: Square-Screen Filtering, Search & Deterministic Priority/Date Sort
// ---------------------------------------------------------------------------
bool testFilteringSearchAndDeterministicSorting() {
    std::string dbPath = createTempUiDbPath("filter_sort");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ctrl.setTodayDateIso("2026-10-22");

    // Validate ISO date guard
    ASSERT_FALSE(ctrl.createTask("Invalid date task", "", 2, "2026-02-30", "inbox", 1010));
    ASSERT_FALSE(ctrl.createTask("Invalid format task", "", 2, "22-10-2026", "inbox", 1010));
    ASSERT_FALSE(ctrl.createTask("   ", "Empty title", 2, "", "inbox", 1010));

    // Create tasks in inbox and work project
    ASSERT_TRUE(ctrl.createTask("Low priority undated", "note alpha", 1, "", "inbox", 1020));
    ASSERT_TRUE(ctrl.createTask("Urgent later due", "note beta", 4, "2026-10-25", "inbox", 1030));
    ASSERT_TRUE(ctrl.createTask("Urgent due today", "critical passport UI", 4, "2026-10-22", "inbox", 1040));
    ASSERT_TRUE(ctrl.createTask("High overdue work task", "qnx build", 3, "2026-10-20", "work", 1050));

    // Inbox filter: 3 tasks sorted by priority DESC (4 before 1), then earlier due_date first
    ctrl.setFilter(FilterMode::Inbox);
    const std::vector<TaskViewItem>& inboxTasks = ctrl.getVisibleTasks();
    ASSERT_EQ(3, static_cast<int>(inboxTasks.size()));
    ASSERT_EQ(std::string("Urgent due today"), inboxTasks[0].content);
    ASSERT_EQ(std::string("P1"), inboxTasks[0].priorityLabel());
    ASSERT_EQ(std::string("Urgent later due"), inboxTasks[1].content);
    ASSERT_EQ(std::string("Low priority undated"), inboxTasks[2].content);
    ASSERT_EQ(std::string("P4"), inboxTasks[2].priorityLabel());

    // Today filter: includes today ("2026-10-22") and overdue ("2026-10-20") across all projects
    ctrl.setFilter(FilterMode::Today);
    const std::vector<TaskViewItem>& todayTasks = ctrl.getVisibleTasks();
    ASSERT_EQ(2, static_cast<int>(todayTasks.size()));
    ASSERT_EQ(std::string("Urgent due today"), todayTasks[0].content);
    ASSERT_EQ(std::string("High overdue work task"), todayTasks[1].content);

    // Project filter: "work"
    ctrl.setFilter(FilterMode::Project, "work");
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(std::string("High overdue work task"), ctrl.getVisibleTasks()[0].content);

    // Search query filter across all tasks
    ctrl.setFilter(FilterMode::All);
    ASSERT_EQ(4, static_cast<int>(ctrl.getVisibleTasks().size()));
    ctrl.setSearchQuery("PASSPORT");
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(std::string("Urgent due today"), ctrl.getVisibleTasks()[0].content);
    ctrl.setSearchQuery("");

    // Complete one task and check Completed filter
    std::string completedId = ctrl.getVisibleTasks()[0].id;
    ASSERT_TRUE(ctrl.toggleTaskCompleted(completedId, 1100));
    ASSERT_EQ(3, ctrl.getTotalIncompleteCount());
    ASSERT_EQ(1, ctrl.getCompletedCount());

    ctrl.setFilter(FilterMode::Completed);
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(completedId, ctrl.getVisibleTasks()[0].id);

    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 3: Physical QWERTY Keyboard Shortcuts & Modal-Sheet Guard
// ---------------------------------------------------------------------------
bool testPhysicalKeyboardShortcutsAndModalGuard() {
    std::string dbPath = createTempUiDbPath("shortcuts");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.setApiToken("shortcut_test_token_1234", 1000));

    // Press 'c' -> opens Create Task Sheet
    ASSERT_FALSE(ctrl.isCreateSheetOpen());
    ASSERT_TRUE(ctrl.handleKeyShortcut("c", 1010));
    ASSERT_TRUE(ctrl.isCreateSheetOpen());

    ASSERT_TRUE(ctrl.createTask("First task", "desc 1", 4, "2026-10-22", "inbox", 1020));
    ASSERT_FALSE(ctrl.isCreateSheetOpen());
    ASSERT_TRUE(ctrl.createTask("Second task", "desc 2", 2, "2026-10-23", "inbox", 1030));

    // Newly created "Second task" (priority 2) is at index 1 and automatically selected
    ASSERT_EQ(2, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(1, ctrl.getSelectedIndex());
    ASSERT_EQ(std::string("Second task"), ctrl.getSelectedTask()->content);

    // Move selection up with 'k', down with 'j', and back up with 'k'
    ASSERT_TRUE(ctrl.handleKeyShortcut("k", 1035));
    ASSERT_EQ(0, ctrl.getSelectedIndex());
    ASSERT_EQ(std::string("First task"), ctrl.getSelectedTask()->content);

    ASSERT_TRUE(ctrl.handleKeyShortcut("j", 1040));
    ASSERT_EQ(1, ctrl.getSelectedIndex());
    ASSERT_EQ(std::string("Second task"), ctrl.getSelectedTask()->content);

    ASSERT_TRUE(ctrl.handleKeyShortcut("k", 1050));
    ASSERT_EQ(0, ctrl.getSelectedIndex());
    ASSERT_EQ(std::string("First task"), ctrl.getSelectedTask()->content);

    // Press 'Enter' -> opens Detail Sheet for selected task ("First task")
    ASSERT_TRUE(ctrl.handleKeyShortcut("Enter", 1060));
    ASSERT_TRUE(ctrl.isDetailSheetOpen());
    ASSERT_EQ(std::string("First task"), ctrl.getDetailTask().content);

    // CRITICAL MODAL GUARD CHECK:
    // While Detail Sheet is open, pressing Space, Backspace, Delete, C, J, K must NOT
    // complete or delete background tasks! Only Escape dismisses the sheet.
    ASSERT_FALSE(ctrl.handleKeyShortcut("Space", 1065));
    ASSERT_FALSE(ctrl.handleKeyShortcut("Delete", 1066));
    ASSERT_FALSE(ctrl.handleKeyShortcut("Backspace", 1067));
    ASSERT_EQ(2, ctrl.getTotalIncompleteCount());

    // Escape closes the open Detail Sheet
    ASSERT_TRUE(ctrl.handleKeyShortcut("Escape", 1070));
    ASSERT_FALSE(ctrl.isDetailSheetOpen());

    // Now on main list: 'Space' completes selected task ("First task")
    ASSERT_TRUE(ctrl.handleKeyShortcut("Space", 1080));
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(std::string("Second task"), ctrl.getSelectedTask()->content);

    // 'Delete' deletes the remaining selected task ("Second task")
    ASSERT_TRUE(ctrl.handleKeyShortcut("Delete", 1090));
    ASSERT_EQ(0, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(-1, ctrl.getSelectedIndex());

    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: Coalesced 3-Second Mutation Debounce & Detail-Sheet Deferral
// ---------------------------------------------------------------------------
bool testDebouncedMutationSyncAndDeferral() {
    std::string dbPath = createTempUiDbPath("debounce");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ctrl.setDebounceWindowMs(3000);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.setApiToken("debounce_token_5678", 1000));
    int baselineTriggers = ctrl.getTotalSyncTriggers();

    // Mutation 1 at t=2000 -> deadline = 5000
    ASSERT_TRUE(ctrl.createTask("Task A", "", 2, "", "inbox", 2000));
    ASSERT_TRUE(ctrl.isDebouncePending());
    ASSERT_EQ(static_cast<uint64_t>(5000), ctrl.getDebounceDeadlineMs());

    // Tick at t=4000 -> should NOT trigger sync yet
    ctrl.tick(4000);
    ASSERT_EQ(baselineTriggers, ctrl.getTotalSyncTriggers());

    // Mutation 2 at t=4500 -> slides deadline to 7500
    ASSERT_TRUE(ctrl.createTask("Task B", "", 3, "", "inbox", 4500));
    ASSERT_EQ(static_cast<uint64_t>(7500), ctrl.getDebounceDeadlineMs());

    // Tick at t=6000 -> still coalescing
    ctrl.tick(6000);
    ASSERT_EQ(baselineTriggers, ctrl.getTotalSyncTriggers());

    // If user opens Detail Sheet before 7500, debounce waits until sheet closes
    ASSERT_TRUE(ctrl.openSelectedTaskDetail());
    ctrl.tick(8000);
    ASSERT_EQ(baselineTriggers, ctrl.getTotalSyncTriggers());

    // Closing or saving the sheet allows the coalesced sync to fire
    ctrl.closeSheets(8000);
    ctrl.tick(8000);
    ASSERT_EQ(baselineTriggers + 1, ctrl.getTotalSyncTriggers());
    ASSERT_FALSE(ctrl.isDebouncePending());

    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 5: Async Worker Sync, temp_id Reconciliation & Selection Preservation
// ---------------------------------------------------------------------------
bool testAsyncWorkerTempIdReconciliationInUi() {
    std::string dbPath = createTempUiDbPath("reconcile_ui");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, true));

    // Create offline task
    ASSERT_TRUE(ctrl.createTask("Draft offline task", "initial note", 4, "2026-10-22", "inbox", 1100));
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    std::string tempId = ctrl.getVisibleTasks()[0].id;
    ASSERT_TRUE(tempId.find("temp_uuid_") == 0);
    ASSERT_EQ(std::string("Local New"), ctrl.getVisibleTasks()[0].syncBadgeText);

    // Select and open Detail Sheet on the temporary task
    ASSERT_TRUE(ctrl.openSelectedTaskDetail());
    ASSERT_EQ(tempId, ctrl.getDetailTask().id);

    // Fetch queued command UUID to build mock server acknowledgement
    std::vector<SyncCommand> cmds;
    ASSERT_TRUE(ctrl.commandQueue().getPendingCommands(cmds));
    ASSERT_EQ(1, static_cast<int>(cmds.size()));
    std::string cmdUuid = cmds[0].uuid;

    std::ostringstream resp;
    resp << "{\"sync_token\":\"tok-rec-2\",\"full_sync\":false,\"items\":[],"
         << "\"temp_id_mapping\":{\"" << tempId << "\":\"server_perm_9001\"},"
         << "\"sync_status\":{\"" << cmdUuid << "\":\"ok\"}}";
    transport.configureResponse(200, resp.str());

    // Trigger sync while Detail Sheet is open
    ASSERT_TRUE(ctrl.setApiToken("valid_sync_token_9001", 1200));
    ASSERT_TRUE(waitForWorkerDrain(ctrl, 1300));

    // Verify UI model and Detail Sheet ID both reconciled to "server_perm_9001"
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(std::string("server_perm_9001"), ctrl.getVisibleTasks()[0].id);
    ASSERT_EQ(std::string("Synced"), ctrl.getVisibleTasks()[0].syncBadgeText);
    ASSERT_EQ(std::string("server_perm_9001"), ctrl.getSelectedTaskId());

    // Saving an edit using the reconciled ID succeeds cleanly
    ASSERT_TRUE(ctrl.updateTask(ctrl.getSelectedTaskId(),
                                "Updated after reconciliation",
                                "updated note",
                                4,
                                "2026-10-24",
                                1400));
    ASSERT_EQ(std::string("Updated after reconciliation"), ctrl.getVisibleTasks()[0].content);
    ASSERT_EQ(std::string("Local Edit"), ctrl.getVisibleTasks()[0].syncBadgeText);

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 6: Active Frame (SceneCover) Snapshot & Minimized Lifecycle Polling
// ---------------------------------------------------------------------------
bool testActiveFrameSnapshotAndLifecyclePolling() {
    std::string dbPath = createTempUiDbPath("active_frame");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.setApiToken("cover_token_7777", 1000));
    ctrl.setTodayDateIso("2026-10-22");

    // Create 6 incomplete tasks -> Active Frame snapshot caps topTasks at 4
    for (int i = 1; i <= 6; ++i) {
        std::ostringstream title;
        title << "Cover Task " << i;
        ASSERT_TRUE(ctrl.createTask(title.str(), "", (i % 4) + 1, "2026-10-22", "inbox", 1000 + i * 10));
    }

    const ActiveFrameSnapshot& snap = ctrl.getActiveFrameSnapshot();
    ASSERT_EQ(6, snap.totalIncompleteCount);
    ASSERT_EQ(6, snap.todayIncompleteCount);
    ASSERT_EQ(4, static_cast<int>(snap.topTasks.size()));
    ASSERT_EQ(4, snap.topTasks[0].priority);

    // Transition to minimized ActiveFrameThumbnail on Wi-Fi (5m = 300,000 ms interval)
    ctrl.setNetworkConnectionType(NetworkConnectionType::WiFi, 2000);
    ctrl.setLifecycleState(AppLifecycleState::ActiveFrameThumbnail, 2000);
    ASSERT_EQ(static_cast<uint64_t>(300000), ctrl.getActivePollIntervalMs());

    int triggersAfterMinimize = ctrl.getTotalSyncTriggers();
    // Complete the flushed sync so m_isSyncing becomes false
    ctrl.notificationQueue().push(SyncEventSnapshot(WORKER_STATE_SUCCESS, 200, "OK", 2, 6));
    ctrl.tick(2100);
    ASSERT_FALSE(ctrl.isSyncing());

    // Before 5 minutes (at +299,000 ms): no background poll
    ctrl.tick(2000 + 299000);
    ASSERT_EQ(triggersAfterMinimize, ctrl.getTotalSyncTriggers());

    // At +300,000 ms: Wi-Fi Active Frame poll triggers!
    ctrl.tick(2000 + 300000);
    ASSERT_EQ(triggersAfterMinimize + 1, ctrl.getTotalSyncTriggers());
    ctrl.notificationQueue().push(SyncEventSnapshot(WORKER_STATE_SUCCESS, 200, "OK", 302, 0));
    ctrl.tick(2000 + 300100);

    // Switch to Cellular (15m = 900,000 ms interval)
    ctrl.setNetworkConnectionType(NetworkConnectionType::Cellular, 400000);
    ASSERT_EQ(static_cast<uint64_t>(900000), ctrl.getActivePollIntervalMs());

    // Switch to Offline -> interval = 0, headline shows Offline
    ctrl.setNetworkConnectionType(NetworkConnectionType::Offline, 450000);
    ASSERT_EQ(static_cast<uint64_t>(0), ctrl.getActivePollIntervalMs());
    ASSERT_TRUE(ctrl.getActiveFrameSnapshot().statusHeadline.find("Offline") == 0);

    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 7: HTTP 401 Authentication Error & Token Re-Entry Recovery
// ---------------------------------------------------------------------------
bool testHttp401AuthRecoveryFlow() {
    std::string dbPath = createTempUiDbPath("auth_401");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;
    transport.configureResponse(401, "{\"error\":\"Unauthorized\"}");

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, true));
    ASSERT_TRUE(ctrl.setApiToken("revoked_token_0000", 1100));

    ASSERT_TRUE(waitForWorkerDrain(ctrl, 1200));
    ASSERT_TRUE(ctrl.requiresAuth());
    ASSERT_TRUE(ctrl.isSettingsSheetOpen());
    ASSERT_EQ(1, transport.getCallCount());

    // Subsequent background ticks while requiresAuth is latched make ZERO network calls
    ctrl.setLifecycleState(AppLifecycleState::ActiveFrameThumbnail, 1500);
    ctrl.tick(1500 + 500000);
    ASSERT_EQ(1, transport.getCallCount());

    // Entering a valid new token clears requiresAuth, closes Settings sheet, and syncs
    transport.configureResponse(
        200,
        "{\"sync_token\":\"tok-auth-ok\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}"
    );
    ASSERT_TRUE(ctrl.setApiToken("fresh_valid_token_8888", 2000));
    ASSERT_TRUE(waitForWorkerDrain(ctrl, 2100));

    ASSERT_FALSE(ctrl.requiresAuth());
    ASSERT_FALSE(ctrl.isSettingsSheetOpen());
    ASSERT_EQ(std::string("Synced"), ctrl.getStatusText());
    ASSERT_EQ(2, transport.getCallCount());

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 8: Token Revocation & Reachability Wakeup Suppression (M4-R001)
// ---------------------------------------------------------------------------
bool testTokenRevocationAndReachabilityWakeup() {
    std::string dbPath = createTempUiDbPath("token_revoke");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, true));
    ASSERT_TRUE(ctrl.setApiToken("VALID_TOKEN_1", 1000));
    ASSERT_TRUE(waitForWorkerDrain(ctrl, 1100));
    ASSERT_EQ(1, transport.getCallCount());

    // Clear token
    ASSERT_TRUE(ctrl.clearApiToken());
    ASSERT_FALSE(ctrl.hasApiToken());
    ASSERT_TRUE(ctrl.syncEngine().getBearerToken().empty());
    ASSERT_TRUE(ctrl.requiresAuth());

    // Offline -> WiFi transition must NOT wake worker or make network calls
    int before = transport.getCallCount();
    ctrl.setNetworkConnectionType(NetworkConnectionType::Offline, 2000);
    ctrl.setNetworkConnectionType(NetworkConnectionType::WiFi, 2100);
    for (int i = 0; i < 20; ++i) {
        ctrl.tick(2100 + i * 10);
        usleep(5000);
    }
    ASSERT_EQ(before, transport.getCallCount());
    ASSERT_TRUE(ctrl.requiresAuth());

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 9: Replacement Token Ordering & Auth Suspension (M4-R002)
// ---------------------------------------------------------------------------
bool testReplacementTokenOrderingAndAuthSuspension() {
    std::string dbPath = createTempUiDbPath("token_replace");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.setApiToken("INITIAL_TOKEN", 1000));

    // Simulate 401 auth error notification from worker
    ctrl.notificationQueue().push(SyncEventSnapshot(WORKER_STATE_FATAL_ERROR, 401, "Unauthorized", 1, 0));
    ctrl.tick(1100);
    ASSERT_TRUE(ctrl.requiresAuth());

    // Mutating a task and waiting for debounce deadline must NOT clear requiresAuth
    ctrl.closeSheets(1200);
    ASSERT_TRUE(ctrl.createTask("Task during auth suspension", "", 1, "", "inbox", 1300));
    int beforeTriggers = ctrl.getTotalSyncTriggers();
    ctrl.tick(4500); // Past debounce deadline
    ASSERT_EQ(beforeTriggers, ctrl.getTotalSyncTriggers());
    ASSERT_TRUE(ctrl.requiresAuth()); // Must remain suspended!

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 10: Deterministic temp_id Mapping with Duplicate Titles (M4-R003)
// ---------------------------------------------------------------------------
bool testDeterministicTempIdMappingWithDuplicateTitles() {
    std::string dbPath = createTempUiDbPath("dup_titles");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));

    Task a;
    a.id = "temp_uuid_a";
    a.content = "Identical Title";
    a.project_id = "inbox";
    Task b = a;
    b.id = "temp_uuid_b";

    ASSERT_TRUE(ctrl.taskRepository().createTaskLocally(a));
    ASSERT_TRUE(ctrl.taskRepository().createTaskLocally(b));
    ASSERT_TRUE(ctrl.reloadTasks());

    ASSERT_TRUE(ctrl.selectTaskById(b.id));
    ASSERT_TRUE(ctrl.openSelectedTaskDetail());
    ASSERT_EQ(std::string("temp_uuid_b"), ctrl.getSelectedTaskId());
    ASSERT_EQ(std::string("temp_uuid_b"), ctrl.getDetailTask().id);

    std::map<std::string, std::string> mapping;
    mapping[a.id] = "server_perm_a";
    mapping[b.id] = "server_perm_b";

    TempIdReconciler reconciler(db);
    ASSERT_TRUE(reconciler.reconcile(mapping));
    ASSERT_TRUE(ctrl.reloadTasks());

    // Must map deterministically to server_perm_b, NOT server_perm_a!
    ASSERT_EQ(std::string("server_perm_b"), ctrl.getSelectedTaskId());
    ASSERT_EQ(std::string("server_perm_b"), ctrl.getDetailTask().id);

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 11: Committed Snapshot Isolation & Contended Latency (M4-R005, M4-R006)
// ---------------------------------------------------------------------------
struct ForeignTxnContext {
    DatabaseManager* db;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool ready;
    bool release;
};

void* foreignTxnWorker(void* arg) {
    ForeignTxnContext* ctx = static_cast<ForeignTxnContext*>(arg);
    ctx->db->beginImmediate();
    ctx->db->execute("UPDATE tasks SET content='DIRTY_UNCOMMITTED';");
    pthread_mutex_lock(&ctx->mutex);
    ctx->ready = true;
    pthread_cond_broadcast(&ctx->cond);
    while (!ctx->release) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
    ctx->db->rollback();
    return NULL;
}

bool testCommittedSnapshotIsolation() {
    std::string dbPath = createTempUiDbPath("snapshot_iso");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.createTask("COMMITTED_ORIGINAL", "", 1, "", "inbox", 1000));

    ForeignTxnContext ctx;
    ctx.db = &db;
    ctx.ready = false;
    ctx.release = false;
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, foreignTxnWorker, &ctx));
    pthread_mutex_lock(&ctx.mutex);
    while (!ctx.ready) {
        pthread_cond_wait(&ctx.cond, &ctx.mutex);
    }
    pthread_mutex_unlock(&ctx.mutex);

    // During active foreign transaction, UI reload must NOT read uncommitted dirty row
    ASSERT_TRUE(ctrl.reloadTasks());
    ASSERT_EQ(std::string("COMMITTED_ORIGINAL"), ctrl.getVisibleTasks()[0].content);

    // Contended mutation must return false promptly without 500ms sleep
    int64_t t0 = util::currentEpochMs();
    bool created = ctrl.createTask("CONTENDED", "", 1, "", "inbox", 2000);
    int64_t elapsed = util::currentEpochMs() - t0;
    ASSERT_FALSE(created);
    ASSERT_TRUE(elapsed < 50);

    pthread_mutex_lock(&ctx.mutex);
    ctx.release = true;
    pthread_cond_broadcast(&ctx.cond);
    pthread_mutex_unlock(&ctx.mutex);
    pthread_join(th, NULL);

    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.mutex);

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 12: Protected Token Storage Encryption (M4-R007)
// ---------------------------------------------------------------------------
bool testTokenProtectedStorageEncryption() {
    std::string dbPath = createTempUiDbPath("token_encrypt");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.setApiToken("MY_SECRET_BEARER_TOKEN_9999", 1000));

    // Stored token row must be encrypted with "enc:v1:", NOT plaintext!
    std::string rawStored;
    ASSERT_TRUE(ctrl.metadataRepository().getMetadata("api_token", rawStored));
    ASSERT_TRUE(rawStored.find("enc:v1:") == 0);
    ASSERT_TRUE(rawStored != "MY_SECRET_BEARER_TOKEN_9999");

    ctrl.shutdown();
    db.close();

    // Verify reopening decrypts correctly
    DatabaseManager db2;
    ASSERT_TRUE(db2.open(dbPath));
    TodoistAppController ctrl2(db2, transport);
    ASSERT_TRUE(ctrl2.initialize(2000, false));
    ASSERT_TRUE(ctrl2.hasApiToken());
    ASSERT_EQ(std::string("••••••••9999"), ctrl2.getMaskedApiToken());

    ctrl2.shutdown();
    db2.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 13: Local Date Rollover Across Midnight (M4-R009)
// ---------------------------------------------------------------------------
bool testDateRolloverAcrossMidnight() {
    std::string dbPath = createTempUiDbPath("midnight_roll");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    std::string todayReal = ctrl.getTodayDateIso();
    ctrl.setTodayDateIso("2000-01-01");

    ASSERT_TRUE(ctrl.createTask("Tomorrow task", "", 1, todayReal, "inbox", 1000));
    ctrl.setFilter(FilterMode::Today);
    ASSERT_EQ(0, static_cast<int>(ctrl.getVisibleTasks().size()));

    // Simulate midnight rollover triggered by tick()
    ctrl.tick(2000);
    ASSERT_EQ(todayReal, ctrl.getTodayDateIso());
    ASSERT_EQ(1, static_cast<int>(ctrl.getVisibleTasks().size()));
    ASSERT_EQ(std::string("Tomorrow task"), ctrl.getVisibleTasks()[0].content);

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 14: Server Inbox Project Identity Resolution (M4-R010)
// ---------------------------------------------------------------------------
bool testInboxProjectIdentityResolution() {
    std::string dbPath = createTempUiDbPath("server_inbox");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));
    ASSERT_TRUE(ctrl.getInboxProjectId().empty()); // Initial unconfigured state

    // Empty or whitespace ID must be rejected
    ASSERT_FALSE(ctrl.setInboxProjectId(""));
    ASSERT_FALSE(ctrl.setInboxProjectId("   "));

    // Configure a neutral opaque inbox project ID
    ASSERT_TRUE(ctrl.setInboxProjectId("  opaque_acc_99  "));
    ASSERT_EQ(std::string("opaque_acc_99"), ctrl.getInboxProjectId());

    // Remote inbox item carrying server's opaque ID "opaque_acc_99"
    Task remoteInbox;
    remoteInbox.id = "srv_task_inbox_1";
    remoteInbox.server_id = remoteInbox.id;
    remoteInbox.content = "Server inbox item";
    remoteInbox.project_id = "opaque_acc_99";
    ASSERT_TRUE(ctrl.taskRepository().upsertRemoteTask(remoteInbox));

    // Remote other project item containing keyword "inbox" in ID
    Task remoteKeyword;
    remoteKeyword.id = "srv_task_keyword_2";
    remoteKeyword.server_id = remoteKeyword.id;
    remoteKeyword.content = "Work task";
    remoteKeyword.project_id = "project_inbox_review";
    ASSERT_TRUE(ctrl.taskRepository().upsertRemoteTask(remoteKeyword));

    // Remote other project item
    Task remoteOther;
    remoteOther.id = "srv_task_other_3";
    remoteOther.server_id = remoteOther.id;
    remoteOther.content = "Other project item";
    remoteOther.project_id = "another_project_999";
    ASSERT_TRUE(ctrl.taskRepository().upsertRemoteTask(remoteOther));

    ASSERT_TRUE(ctrl.reloadTasks());
    ctrl.setFilter(FilterMode::Inbox);

    // Visible in Inbox must include server inbox item and exclude keyword/other projects
    const std::vector<TaskViewItem>& visible = ctrl.getVisibleTasks();
    ASSERT_EQ(1, static_cast<int>(visible.size()));
    ASSERT_EQ(std::string("srv_task_inbox_1"), visible[0].id);

    // Test transactional failure: injected trigger aborts write, in-memory ID preserved
    ASSERT_TRUE(db.execute("CREATE TEMP TRIGGER reject_inbox_test BEFORE INSERT ON sync_metadata WHEN NEW.key='inbox_project_id' BEGIN SELECT RAISE(ABORT,'injected'); END;"));
    ASSERT_FALSE(ctrl.setInboxProjectId("replacement_failed"));
    ASSERT_EQ(std::string("opaque_acc_99"), ctrl.getInboxProjectId());
    ASSERT_TRUE(db.execute("DROP TRIGGER reject_inbox_test;"));

    ctrl.shutdown();

    // Verify persistence across restart
    TodoistAppController ctrl2(db, transport);
    ASSERT_TRUE(ctrl2.initialize(1000, false));
    ASSERT_EQ(std::string("opaque_acc_99"), ctrl2.getInboxProjectId());
    ctrl2.shutdown();

    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

// ---------------------------------------------------------------------------
// Test 15: Complete ISO Date Validation Bounds (M4-R011)
// ---------------------------------------------------------------------------
bool testIsoDateValidationBounds() {
    // Valid formats
    ASSERT_TRUE(TodoistAppController::isValidIsoDate(""));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22"));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22T14:30"));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22 14:30"));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22T14:30:45"));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22T14:30:45Z"));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22T14:30:45+05:00"));
    ASSERT_TRUE(TodoistAppController::isValidIsoDate("2026-10-22T14:30:45-08:00"));

    // Invalid formats: malformed suffix, out-of-range hours/minutes, invalid calendar dates
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22Tgarbage"));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22T"));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22 "));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22T25:00:00"));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22T12:60:00"));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22T12:30:60"));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-10-22T12:30:00+25:00"));
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-02-29")); // 2026 is not a leap year
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("2026-04-31")); // April has 30 days
    ASSERT_FALSE(TodoistAppController::isValidIsoDate("garbage-date"));

    return true;
}

// ---------------------------------------------------------------------------
// Test 16: Available Projects & Physical Shortcut Navigation (M4-R012)
// ---------------------------------------------------------------------------
bool testAvailableProjectsAndKeyboardNavigation() {
    std::string dbPath = createTempUiDbPath("avail_projects");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    MockUiSyncTransport transport;

    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));

    ASSERT_TRUE(ctrl.createTask("Task in Work", "", 1, "", "work", 1000));
    ASSERT_TRUE(ctrl.createTask("Task in Home", "", 1, "", "home", 1010));
    ASSERT_TRUE(ctrl.reloadTasks());

    const std::vector<std::string>& projects = ctrl.getAvailableProjects();
    ASSERT_TRUE(projects.size() >= 2);

    // Keyboard shortcuts 'n' and 'N' open create sheet
    ASSERT_FALSE(ctrl.isCreateSheetOpen());
    ASSERT_TRUE(ctrl.handleKeyShortcut("n", 1020));
    ASSERT_TRUE(ctrl.isCreateSheetOpen());
    ASSERT_TRUE(ctrl.handleKeyShortcut("Escape", 1025));
    ASSERT_FALSE(ctrl.isCreateSheetOpen());

    ASSERT_TRUE(ctrl.handleKeyShortcut("N", 1030));
    ASSERT_TRUE(ctrl.isCreateSheetOpen());
    ASSERT_TRUE(ctrl.handleKeyShortcut("Escape", 1035));
    ASSERT_FALSE(ctrl.isCreateSheetOpen());

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

static bool testProjectDisplayNameAndTouchToggle() {
    std::string dbPath = createTempUiDbPath("m4_dev_fixes");
    cleanupDbFiles(dbPath);

    DatabaseManager db;
    ASSERT_TRUE(db.open(dbPath));
    ASSERT_TRUE(db.initializeSchema());

    MockUiSyncTransport transport;
    TodoistAppController ctrl(db, transport);
    ASSERT_TRUE(ctrl.initialize(1000, false));

    // 1. Test Project Name resolution (M4-DEV-001)
    ASSERT_TRUE(ctrl.setProjectName("6X7fqH39MwjmwV4q", "Work Project"));
    ASSERT_EQ(ctrl.getProjectDisplayName("6X7fqH39MwjmwV4q"), std::string("Work Project"));
    ASSERT_EQ(ctrl.getProjectDisplayName("inbox"), std::string("inbox"));
    ASSERT_EQ(ctrl.getProjectDisplayName("unknown_id"), std::string("unknown_id"));

    // 2. Create task with server project ID and verify view item has display name
    ASSERT_TRUE(ctrl.createTask("Review QML", "Check colors", 4, "2026-10-25", "6X7fqH39MwjmwV4q", 1000));
    ctrl.setFilter(FilterMode::All);
    ASSERT_TRUE(ctrl.reloadTasks());
    const std::vector<TaskViewItem>& tasks = ctrl.getVisibleTasks();
    ASSERT_EQ(tasks.size(), static_cast<size_t>(1));
    ASSERT_EQ(tasks[0].projectId, std::string("6X7fqH39MwjmwV4q"));
    ASSERT_EQ(tasks[0].projectDisplayName, std::string("Work Project"));
    ASSERT_FALSE(tasks[0].isCompleted);

    // 3. Test Touch toggle and Space shortcut completion retention (M4-DEV-002)
    std::string taskId = tasks[0].id;
    ASSERT_TRUE(ctrl.toggleTaskCompleted(taskId, 1010));
    ctrl.setFilter(FilterMode::Completed);
    ASSERT_TRUE(ctrl.reloadTasks());
    ASSERT_EQ(ctrl.getVisibleTasks().size(), static_cast<size_t>(1));
    ASSERT_TRUE(ctrl.getVisibleTasks()[0].isCompleted);

    ASSERT_TRUE(ctrl.selectTaskById(taskId));
    ASSERT_TRUE(ctrl.handleKeyShortcut("Space", 1020));
    ctrl.setFilter(FilterMode::All);
    ASSERT_TRUE(ctrl.reloadTasks());
    ASSERT_EQ(ctrl.getVisibleTasks().size(), static_cast<size_t>(1));
    ASSERT_FALSE(ctrl.getVisibleTasks()[0].isCompleted);

    // 4. Verify project discovered from metadata even with zero tasks
    ASSERT_TRUE(ctrl.setProjectName("proj_empty_123", "Empty Project"));
    ASSERT_TRUE(ctrl.reloadTasks());
    const std::vector<std::string>& projs = ctrl.getAvailableProjects();
    bool foundEmpty = false;
    for (size_t i = 0; i < projs.size(); ++i) {
        if (projs[i] == "proj_empty_123") foundEmpty = true;
    }
    ASSERT_TRUE(foundEmpty);
    ASSERT_EQ(ctrl.getProjectDisplayName("proj_empty_123"), std::string("Empty Project"));

    // 5. Verify resolveProjectId
    ASSERT_EQ(ctrl.resolveProjectId("inbox"), std::string("inbox"));
    ASSERT_EQ(ctrl.resolveProjectId("Work Project"), std::string("6X7fqH39MwjmwV4q"));
    ASSERT_EQ(ctrl.resolveProjectId("6X7fqH39MwjmwV4q"), std::string("6X7fqH39MwjmwV4q"));
    ASSERT_EQ(ctrl.resolveProjectId("Empty Project"), std::string("proj_empty_123"));

    // 6. Verify triggerFullSync resets sync_token to "*" and triggers sync
    ctrl.setApiToken("test_token_xyz");
    ctrl.metadataRepository().setSyncToken("advanced_tok_before_full");
    ASSERT_EQ(ctrl.metadataRepository().getSyncToken(), std::string("advanced_tok_before_full"));
    ASSERT_TRUE(ctrl.triggerFullSync(1050));
    ASSERT_EQ(ctrl.metadataRepository().getSyncToken(), std::string("*"));

    // 7. Verify triggerFullSync handles busy database promptly without blocking (M5-R001)
    DatabaseManager writer;
    ASSERT_TRUE(writer.open(dbPath));
    ASSERT_TRUE(writer.beginImmediate());
    int64_t t0 = todoist::util::currentEpochMs();
    ASSERT_FALSE(ctrl.triggerFullSync(1060));
    int64_t dt = todoist::util::currentEpochMs() - t0;
    ASSERT_TRUE(dt < 200); // Must return promptly with zero timeout
    ASSERT_EQ(ctrl.getLastErrorText(), std::string("Database busy; please try again"));
    ASSERT_TRUE(writer.rollback());
    writer.close();

    ctrl.shutdown();
    db.close();
    cleanupDbFiles(dbPath);
    return true;
}

} // anonymous namespace

int main() {
    std::cout << "=========================================" << std::endl;
    std::cout << " Running Todoist M4 UI & System Suite    " << std::endl;
    std::cout << "=========================================" << std::endl;

    RUN_TEST(testColdStartRecoveryAndTokenPersistence);
    RUN_TEST(testFilteringSearchAndDeterministicSorting);
    RUN_TEST(testPhysicalKeyboardShortcutsAndModalGuard);
    RUN_TEST(testDebouncedMutationSyncAndDeferral);
    RUN_TEST(testAsyncWorkerTempIdReconciliationInUi);
    RUN_TEST(testActiveFrameSnapshotAndLifecyclePolling);
    RUN_TEST(testHttp401AuthRecoveryFlow);
    RUN_TEST(testTokenRevocationAndReachabilityWakeup);
    RUN_TEST(testReplacementTokenOrderingAndAuthSuspension);
    RUN_TEST(testDeterministicTempIdMappingWithDuplicateTitles);
    RUN_TEST(testCommittedSnapshotIsolation);
    RUN_TEST(testTokenProtectedStorageEncryption);
    RUN_TEST(testDateRolloverAcrossMidnight);
    RUN_TEST(testInboxProjectIdentityResolution);
    RUN_TEST(testIsoDateValidationBounds);
    RUN_TEST(testAvailableProjectsAndKeyboardNavigation);
    RUN_TEST(testProjectDisplayNameAndTouchToggle);

    std::cout << "=========================================" << std::endl;
    std::cout << " Summary: " << g_testsPassed << "/" << g_testsRun
              << " passed, " << g_testsFailed << " failed." << std::endl;
    std::cout << "=========================================" << std::endl;
    return (g_testsFailed == 0) ? 0 : 1;
}
