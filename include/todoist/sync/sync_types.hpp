#ifndef TODOIST_SYNC_TYPES_HPP
#define TODOIST_SYNC_TYPES_HPP

#include "todoist/types.hpp"
#include <string>
#include <vector>
#include <map>
#include <ostream>

namespace todoist {

namespace SyncEngineState {
    enum Type {
        Idle = 0,
        Syncing = 1,
        RetryPending = 2,
        AuthError = 3
    };
}
typedef SyncEngineState::Type SyncEngineStateType;

inline std::string syncEngineStateToString(SyncEngineStateType state) {
    switch (state) {
        case SyncEngineState::Idle: return "Idle";
        case SyncEngineState::Syncing: return "Syncing";
        case SyncEngineState::RetryPending: return "RetryPending";
        case SyncEngineState::AuthError: return "AuthError";
    }
    return "Idle";
}

inline std::ostream& operator<<(std::ostream& os, SyncEngineStateType state) {
    return os << syncEngineStateToString(state);
}

namespace SyncCycleStatus {
    enum Type {
        Success = 0,
        RetryPending = 1,
        AuthError = 2,
        Busy = 3,
        FatalError = 4
    };
}
typedef SyncCycleStatus::Type SyncCycleStatusType;

struct SyncCycleResult {
    SyncCycleStatusType status;
    std::string errorMessage;
    int httpStatusCode;
    bool hasFullSyncCatchUpNeeded;
    size_t commandsAcknowledgedCount;
    size_t commandsFailedCount;
    size_t tasksUpsertedCount;
    size_t tasksDeletedCount;

    SyncCycleResult()
        : status(SyncCycleStatus::FatalError)
        , errorMessage("Uninitialized cycle result")
        , httpStatusCode(0)
        , hasFullSyncCatchUpNeeded(false)
        , commandsAcknowledgedCount(0)
        , commandsFailedCount(0)
        , tasksUpsertedCount(0)
        , tasksDeletedCount(0)
    {}

    bool isSuccess() const {
        return status == SyncCycleStatus::Success;
    }
};

struct FrozenRequest {
    std::vector<SyncCommand> inFlightCommands;
    std::string syncToken;
    std::vector<std::string> resourceTypes;
    std::string serializedBody;
    std::string serializedJsonBody; // Backward-compatible alias for serializedBody
    std::string contentType;

    FrozenRequest()
        : inFlightCommands()
        , syncToken("*")
        , resourceTypes()
        , serializedBody("")
        , serializedJsonBody("")
        , contentType("application/x-www-form-urlencoded")
    {}
};

struct CommandOutcome {
    std::string uuid;
    bool isAck;
    bool isPermanent;
    int httpCode;
    int domainCode;
    std::string error;

    CommandOutcome()
        : uuid("")
        , isAck(false)
        , isPermanent(false)
        , httpCode(0)
        , domainCode(0)
        , error("")
    {}
};

struct ParsedDeltaResponse {
    std::string syncToken;
    bool isFullSync;
    std::vector<Task> upsertedTasks;
    std::vector<std::string> deletedTaskIds;
    std::map<std::string, std::string> tempIdMapping;
    std::map<std::string, CommandOutcome> commandOutcomes;
    std::map<std::string, std::string> projectNames;
    std::vector<std::string> deletedProjectIds;
    std::string inboxProjectId;
    bool hasProjectsField;
    bool isValid;
    std::string validationError;

    ParsedDeltaResponse()
        : syncToken("")
        , isFullSync(false)
        , upsertedTasks()
        , deletedTaskIds()
        , tempIdMapping()
        , commandOutcomes()
        , projectNames()
        , deletedProjectIds()
        , inboxProjectId("")
        , hasProjectsField(false)
        , isValid(false)
        , validationError("")
    {}
};

} // namespace todoist

#endif // TODOIST_SYNC_TYPES_HPP
