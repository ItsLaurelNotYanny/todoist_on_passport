#ifndef TODOIST_APP_CONTROLLER_HPP
#define TODOIST_APP_CONTROLLER_HPP

#include "todoist/ui/ui_types.hpp"
#include "todoist/db_manager.hpp"
#include "todoist/task_repository.hpp"
#include "todoist/sync_command_queue.hpp"
#include "todoist/temp_id_reconciler.hpp"
#include "todoist/sync_metadata_repo.hpp"
#include "todoist/sync/sync_engine.hpp"
#include "todoist/sync/sync_transport_interface.hpp"
#include "todoist/network/curl_sync_transport.hpp"
#include "todoist/network/sync_notification_queue.hpp"
#include "todoist/network/sync_worker_thread.hpp"

#include <string>
#include <vector>
#include <stdint.h>

namespace todoist {

class TodoistAppController {
public:
    static const uint64_t DEFAULT_DEBOUNCE_MS = 3000ULL;
    static const uint64_t DEFAULT_WIFI_POLL_MS = 300000ULL;     // 5 minutes
    static const uint64_t DEFAULT_CELLULAR_POLL_MS = 900000ULL; // 15 minutes
    static const size_t MAX_ACTIVE_FRAME_TASKS = 4;

    TodoistAppController(DatabaseManager& db,
                         ISyncTransport& transport,
                         CurlSyncTransport* curlTransport = NULL,
                         const std::string& endpointUrl = "https://api.todoist.com/api/v1/sync");
    ~TodoistAppController();

    // Initializes schema, performs crash recovery on in_flight commands, loads stored token,
    // populates initial task/cover snapshots, starts background worker, and triggers cold-start sync.
    bool initialize(uint64_t nowMs = 0, bool startWorker = true);

    // Stops the background worker thread cleanly
    void shutdown();

    // Authentication & Token Persistence
    bool setApiToken(const std::string& token, uint64_t nowMs = 0);
    bool clearApiToken();
    bool hasApiToken() const;
    std::string getMaskedApiToken() const;
    bool requiresAuth() const { return m_requiresAuth; }

    // Local Atomic Task Mutations (Main Thread -> SQLite WAL + 3s Debounced Sync)
    bool createTask(const std::string& content,
                    const std::string& description,
                    int priority,
                    const std::string& dueDate,
                    const std::string& projectId,
                    uint64_t nowMs = 0);

    bool updateTask(const std::string& taskId,
                    const std::string& content,
                    const std::string& description,
                    int priority,
                    const std::string& dueDate,
                    uint64_t nowMs = 0);

    bool toggleTaskCompleted(const std::string& taskId, uint64_t nowMs = 0);
    bool toggleSelectedTaskCompleted(uint64_t nowMs = 0);

    bool deleteTask(const std::string& taskId, uint64_t nowMs = 0);
    bool deleteSelectedTask(uint64_t nowMs = 0);

    // Filtering, Search & Reloading
    void setFilter(FilterModeType mode, const std::string& projectId = "");
    void setFilterByName(const std::string& modeName, const std::string& projectId = "");
    FilterModeType getFilterMode() const { return m_filterMode; }
    std::string getFilterModeString() const { return filterModeToString(m_filterMode); }
    const std::string& getActiveProjectId() const { return m_activeProjectId; }

    void setSearchQuery(const std::string& query);
    const std::string& getSearchQuery() const { return m_searchQuery; }

    void setTodayDateIso(const std::string& isoDate);
    const std::string& getTodayDateIso() const { return m_todayDateIso; }

    bool isInboxProject(const std::string& projectId) const;
    bool setInboxProjectId(const std::string& id);
    const std::string& getInboxProjectId() const { return m_inboxProjectId; }
    std::string getProjectDisplayName(const std::string& projectId);
    std::string resolveProjectId(const std::string& nameOrId);
    bool setProjectName(const std::string& projectId, const std::string& name);

    bool reloadTasks();

    const std::vector<TaskViewItem>& getVisibleTasks() const { return m_visibleTasks; }
    const std::vector<std::string>& getAvailableProjects() const { return m_availableProjects; }

    // Selection Management (1440x1440 ListView & Physical Keyboard Navigation)
    bool selectTaskByIndex(int index);
    bool selectTaskById(const std::string& taskId);
    bool selectNextTask();
    bool selectPreviousTask();
    int getSelectedIndex() const { return m_selectedIndex; }
    const std::string& getSelectedTaskId() const { return m_selectedTaskId; }
    const TaskViewItem* getSelectedTask() const;

    // Sheet State Management (Create / Detail / Settings drawers)
    bool isCreateSheetOpen() const { return m_createSheetOpen; }
    bool isDetailSheetOpen() const { return m_detailSheetOpen; }
    bool isSettingsSheetOpen() const { return m_settingsSheetOpen; }
    bool isAnySheetOpen() const {
        return m_createSheetOpen || m_detailSheetOpen || m_settingsSheetOpen;
    }

    void openCreateSheet();
    bool openSelectedTaskDetail();
    bool openTaskDetailById(const std::string& taskId);
    void openSettingsSheet();
    void closeSheets(uint64_t nowMs = 0);
    const TaskViewItem& getDetailTask() const { return m_detailTask; }

    // Physical QWERTY Keyboard Shortcut Dispatcher
    // Supports: Space, Enter/Return, Delete/Backspace/Del, c/C/n/N, r/R, j/J/Down, k/K/Up, Escape/Esc
    bool handleKeyShortcut(const std::string& key, uint64_t nowMs = 0);

    // Sync Orchestration, Debounce & Background Worker Notification Drain
    bool triggerSyncNow(uint64_t nowMs = 0);
    bool triggerFullSync(uint64_t nowMs = 0);
    int tick(uint64_t nowMs);
    int drainWorkerNotifications(uint64_t nowMs = 0);

    bool isSyncing() const { return m_isSyncing; }
    bool isDebouncePending() const { return m_debouncePending; }
    uint64_t getDebounceDeadlineMs() const { return m_debounceDeadlineMs; }
    uint64_t getLastSyncedAtMs() const { return m_lastSyncedAtMs; }
    int getTotalSyncTriggers() const { return m_totalSyncTriggers; }

    const std::string& getStatusText() const { return m_statusText; }
    const std::string& getLastErrorText() const { return m_lastErrorText; }

    int getTotalIncompleteCount() const { return m_totalIncompleteCount; }
    int getTodayIncompleteCount() const { return m_todayIncompleteCount; }
    int getCompletedCount() const { return m_completedCount; }
    int getPendingCommandCount() const { return m_pendingCommandCount; }
    int getDeadLetterCount() const { return m_deadLetterCount; }

    // Application Lifecycle & Active Frame (SceneCover)
    void setLifecycleState(AppLifecycleStateType state, uint64_t nowMs = 0);
    AppLifecycleStateType getLifecycleState() const { return m_lifecycleState; }

    void setNetworkConnectionType(NetworkConnectionTypeVal type, uint64_t nowMs = 0);
    NetworkConnectionTypeVal getNetworkConnectionType() const { return m_networkType; }

    uint64_t getActivePollIntervalMs() const;
    const ActiveFrameSnapshot& getActiveFrameSnapshot() const { return m_activeFrameSnapshot; }

    // Configuration overrides (for deterministic testing)
    void setDebounceWindowMs(uint64_t ms) { m_debounceWindowMs = ms; }
    uint64_t getDebounceWindowMs() const { return m_debounceWindowMs; }
    void setBackgroundPollIntervalsMs(uint64_t wifiMs, uint64_t cellularMs) {
        m_wifiPollIntervalMs = wifiMs;
        m_cellularPollIntervalMs = cellularMs;
    }

    // Direct accessors for testing
    TaskRepository& taskRepository() { return m_taskRepo; }
    SyncCommandQueue& commandQueue() { return m_cmdQueue; }
    SyncMetadataRepository& metadataRepository() { return m_metaRepo; }
    SyncEngine& syncEngine() { return m_syncEngine; }
    SyncNotificationQueue& notificationQueue() { return m_notificationQueue; }
    SyncWorkerThread& workerThread() { return m_worker; }

    static bool isValidIsoDate(const std::string& dueDate);
    static std::string currentLocalDateIso();

private:
    TodoistAppController(const TodoistAppController&);
    TodoistAppController& operator=(const TodoistAppController&);

    void scheduleDebouncedSync(uint64_t nowMs);
    bool queryAllNonDeletedTasks(std::vector<Task>& outTasks);
    void rebuildVisibleTasksAndCover();
    static TaskViewItem toViewItem(const Task& task, bool selected, const std::string& projectDisplayName = "");

    DatabaseManager& m_db;
    TaskRepository m_taskRepo;
    SyncCommandQueue m_cmdQueue;
    TempIdReconciler m_tempIdReconciler;
    SyncMetadataRepository m_metaRepo;
    SyncEngine m_syncEngine;
    ISyncTransport& m_transport;
    CurlSyncTransport* m_curlTransport;
    SyncNotificationQueue m_notificationQueue;
    SyncWorkerThread m_worker;

    bool m_initialized;
    bool m_workerStarted;
    std::string m_apiToken;
    bool m_requiresAuth;
    bool m_isSyncing;

    FilterModeType m_filterMode;
    std::string m_activeProjectId;
    std::string m_inboxProjectId;
    std::string m_searchQuery;
    std::string m_todayDateIso;

    std::vector<Task> m_allTasks;
    std::vector<TaskViewItem> m_visibleTasks;
    std::vector<std::string> m_availableProjects;

    int m_selectedIndex;
    std::string m_selectedTaskId;

    bool m_createSheetOpen;
    bool m_detailSheetOpen;
    bool m_settingsSheetOpen;
    TaskViewItem m_detailTask;
    bool m_deferredDebounceWhileDetailOpen;

    uint64_t m_debounceWindowMs;
    bool m_debouncePending;
    uint64_t m_debounceDeadlineMs;

    AppLifecycleStateType m_lifecycleState;
    NetworkConnectionTypeVal m_networkType;
    uint64_t m_wifiPollIntervalMs;
    uint64_t m_cellularPollIntervalMs;
    uint64_t m_lastBackgroundPollMs;
    uint64_t m_lastSyncedAtMs;
    int m_totalSyncTriggers;

    int m_totalIncompleteCount;
    int m_todayIncompleteCount;
    int m_completedCount;
    int m_pendingCommandCount;
    int m_deadLetterCount;

    std::string m_statusText;
    std::string m_lastErrorText;
    ActiveFrameSnapshot m_activeFrameSnapshot;
};

} // namespace todoist

#endif // TODOIST_APP_CONTROLLER_HPP
