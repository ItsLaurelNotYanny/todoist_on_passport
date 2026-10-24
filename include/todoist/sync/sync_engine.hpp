#ifndef TODOIST_SYNC_ENGINE_HPP
#define TODOIST_SYNC_ENGINE_HPP

#include "todoist/sync/sync_types.hpp"
#include "todoist/sync/sync_transport_interface.hpp"
#include "todoist/sync/sync_payload_builder.hpp"
#include "todoist/sync/sync_response_parser.hpp"
#include "todoist/db_manager.hpp"
#include "todoist/task_repository.hpp"
#include "todoist/sync_command_queue.hpp"
#include "todoist/temp_id_reconciler.hpp"
#include "todoist/sync_metadata_repo.hpp"
#include <pthread.h>
#include <string>
#include <vector>
#include <set>

namespace todoist {

class SyncEngine {
public:
    SyncEngine(DatabaseManager& db,
               TaskRepository& taskRepo,
               SyncCommandQueue& cmdQueue,
               TempIdReconciler& tempIdReconciler,
               SyncMetadataRepository& metadataRepo,
               const std::string& endpointUrl = "https://api.todoist.com/api/v1/sync",
               const std::string& bearerToken = "");
    ~SyncEngine();

    // Runs startup recovery: resets orphaned in_flight rows back to pending with retry_count incremented.
    // Must be called once before accepting user mutations or sync cycles.
    bool initialize();

    void setBearerToken(const std::string& token);
    std::string getBearerToken() const;

    void setEndpointUrl(const std::string& url);
    std::string getEndpointUrl() const;

    void setMaxBatchSize(size_t batchSize);
    size_t getMaxBatchSize() const;

    SyncEngineStateType getState() const;

    // Deterministic single-cycle sync execution.
    // Guarded against overlapping reentrancy (returns Busy immediately on overlap).
    // Zero background timers, sleeps, or network scheduling in M2.
    SyncCycleResult executeSyncCycle(ISyncTransport& transport);

    // Multi-batch queue draining helper:
    // Sequentially executes cycles while eligible unblocked commands remain.
    // Halts immediately upon encountering RetryPending or AuthError without spinning.
    SyncCycleResult drainPendingQueue(ISyncTransport& transport, int maxBatches = 10);

    // Cascades create failure to dependent commands referencing temp_id (returns checked bool)
    bool cascadeCreateFailure(const std::string& tempId, const std::string& errorMsg);

private:
    DatabaseManager& m_db;
    TaskRepository& m_taskRepo;
    SyncCommandQueue& m_cmdQueue;
    TempIdReconciler& m_tempIdReconciler;
    SyncMetadataRepository& m_metadataRepo;

    std::string m_endpointUrl;
    std::string m_bearerToken;
    size_t m_maxBatchSize;

    SyncEngineStateType m_state;
    bool m_cycleActive;
    mutable pthread_mutex_t m_mutex;

    // Helper functions for atomic dispatch pipeline
    bool resetInFlightBatchToPending(const std::vector<SyncCommand>& batch, bool incrementRetry);
    bool getPreRequestCleanTaskIds(std::set<std::string>& outTaskIds);
    bool advanceBaselinesForAcknowledgedCommands(const std::vector<SyncCommand>& inFlightCommands,
                                                const std::vector<std::string>& ackUuids,
                                                const std::map<std::string, std::string>& validatedMapping);
    bool finalizeAcknowledgedTaskStates(const std::vector<SyncCommand>& inFlightCommands,
                                        const std::vector<std::string>& ackUuids,
                                        const std::map<std::string, std::string>& validatedMapping);
    bool pruneAbsentSyncedTasksWithProvenance(const std::set<std::string>& preRequestCleanTaskIds,
                                              const std::set<std::string>& requestTouchedTaskIds,
                                              const std::vector<Task>& incomingTasks);
    bool hasUnmappedProjects();

    SyncEngine(const SyncEngine&);
    SyncEngine& operator=(const SyncEngine&);
};

} // namespace todoist

#endif // TODOIST_SYNC_ENGINE_HPP
