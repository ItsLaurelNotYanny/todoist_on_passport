#ifndef TODOIST_SYNC_METADATA_REPO_HPP
#define TODOIST_SYNC_METADATA_REPO_HPP

#include "todoist/types.hpp"
#include "todoist/db_manager.hpp"
#include "todoist/task_repository.hpp"
#include "todoist/sync_command_queue.hpp"
#include <string>
#include <vector>

namespace todoist {

class SyncMetadataRepository {
public:
    SyncMetadataRepository(DatabaseManager& dbManager,
                           TaskRepository& taskRepo,
                           SyncCommandQueue& commandQueue);

    bool getMetadata(const std::string& key, std::string& valueOut);
    bool setMetadata(const std::string& key, const std::string& value);
    bool deleteMetadata(const std::string& key);

    std::string getSyncToken();
    bool setSyncToken(const std::string& token);

    // Atomic Delta Ingestion:
    // Applies remote task changes, deletes, command pruning, and sync_token update in ONE single transaction
    bool applyDeltaAndSaveToken(const std::vector<Task>& upsertedTasks,
                                const std::vector<std::string>& deletedTaskIds,
                                const std::vector<std::string>& acknowledgedCommandUuids,
                                const std::string& newSyncToken);

private:
    DatabaseManager& m_db;
    TaskRepository& m_taskRepo;
    SyncCommandQueue& m_commandQueue;

    SyncMetadataRepository(const SyncMetadataRepository&);
    SyncMetadataRepository& operator=(const SyncMetadataRepository&);
};

} // namespace todoist

#endif // TODOIST_SYNC_METADATA_REPO_HPP
