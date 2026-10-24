#ifndef TODOIST_TYPES_HPP
#define TODOIST_TYPES_HPP

#include <string>
#include <stdint.h>
#include <vector>
#include <ostream>

namespace todoist {

namespace SyncState {
    enum Type {
        Synced = 0,
        LocallyCreated = 1,
        LocallyUpdated = 2,
        LocallyDeleted = 3
    };
}
typedef SyncState::Type SyncStateType;

inline std::string syncStateToString(SyncStateType state) {
    switch (state) {
        case SyncState::Synced: return "synced";
        case SyncState::LocallyCreated: return "locally_created";
        case SyncState::LocallyUpdated: return "locally_updated";
        case SyncState::LocallyDeleted: return "locally_deleted";
    }
    return "synced";
}

inline std::ostream& operator<<(std::ostream& os, SyncStateType state) {
    return os << syncStateToString(state);
}

namespace CommandStatus {
    enum Type {
        Pending = 0,
        InFlight = 1,
        Failed = 2
    };
}
typedef CommandStatus::Type CommandStatusType;

inline std::string commandStatusToString(CommandStatusType status) {
    switch (status) {
        case CommandStatus::Pending: return "pending";
        case CommandStatus::InFlight: return "in_flight";
        case CommandStatus::Failed: return "failed";
    }
    return "pending";
}

inline SyncStateType stringToSyncState(const std::string& str) {
    if (str == "locally_created") return SyncState::LocallyCreated;
    if (str == "locally_updated") return SyncState::LocallyUpdated;
    if (str == "locally_deleted") return SyncState::LocallyDeleted;
    return SyncState::Synced;
}

inline std::ostream& operator<<(std::ostream& os, CommandStatusType status) {
    return os << commandStatusToString(status);
}

inline CommandStatusType stringToCommandStatus(const std::string& str) {
    if (str == "in_flight") return CommandStatus::InFlight;
    if (str == "failed") return CommandStatus::Failed;
    return CommandStatus::Pending;
}

struct Task {
    std::string id;              // Temporary UUID or server ID
    std::string server_id;       // Server assigned ID (empty if locally created and unacknowledged)
    std::string content;         // Task title / content
    std::string description;     // Task markdown description
    bool is_completed;           // 0 = open, 1 = completed
    int priority;                // 1 (Normal) to 4 (Urgent)
    std::string due_date;        // ISO-8601 string or empty
    std::string project_id;      // Project ID (default "inbox")
    int64_t local_updated_at;    // Epoch timestamp in milliseconds
    SyncStateType sync_state;    // Current sync state

    Task()
        : id("")
        , server_id("")
        , content("")
        , description("")
        , is_completed(false)
        , priority(1)
        , due_date("")
        , project_id("inbox")
        , local_updated_at(0)
        , sync_state(SyncState::Synced)
    {}
};

struct SyncCommand {
    std::string uuid;            // Persistent unique command UUID
    std::string type;            // item_add, item_update, item_complete, item_uncomplete, item_delete
    std::string temp_id;         // Temporary ID (used for item_add)
    std::string args_json;       // Serialized command arguments
    CommandStatusType status;    // pending, in_flight, failed
    int64_t created_at;          // Epoch timestamp in milliseconds
    int retry_count;             // Number of retries attempted
    std::string last_error;      // Most recent error message (if any)

    SyncCommand()
        : uuid("")
        , type("")
        , temp_id("")
        , args_json("")
        , status(CommandStatus::Pending)
        , created_at(0)
        , retry_count(0)
        , last_error("")
    {}
};

} // namespace todoist

#endif // TODOIST_TYPES_HPP
