#ifndef TODOIST_SYNC_COMMAND_QUEUE_HPP
#define TODOIST_SYNC_COMMAND_QUEUE_HPP

#include "todoist/types.hpp"
#include "todoist/db_manager.hpp"
#include <vector>
#include <string>

namespace todoist {

class SyncCommandQueue {
public:
    explicit SyncCommandQueue(DatabaseManager& dbManager);

    // Queue Retrieval
    bool getPendingCommands(std::vector<SyncCommand>& commandsOut, int limit = 50);
    int getPendingCount();
    int getInFlightCount();
    int getDeadLetterCount();

    // State Transitions
    bool markCommandsInFlight(const std::vector<std::string>& commandUuids);
    bool markCommandAcknowledged(const std::string& commandUuid);
    bool markBatchAcknowledged(const std::vector<std::string>& commandUuids);
    bool markCommandFailedTransient(const std::string& commandUuid, const std::string& errorMsg);
    bool markCommandFailedPermanent(const std::string& commandUuid, const std::string& errorReason);

private:
    DatabaseManager& m_db;

    SyncCommandQueue(const SyncCommandQueue&);
    SyncCommandQueue& operator=(const SyncCommandQueue&);
};

} // namespace todoist

#endif // TODOIST_SYNC_COMMAND_QUEUE_HPP
