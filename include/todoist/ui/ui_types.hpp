#ifndef TODOIST_UI_TYPES_HPP
#define TODOIST_UI_TYPES_HPP

#include "todoist/types.hpp"
#include <string>
#include <vector>
#include <stdint.h>

namespace todoist {

namespace FilterMode {
    enum Type {
        Inbox = 0,
        Today = 1,
        All = 2,
        Completed = 3,
        Project = 4
    };
}
typedef FilterMode::Type FilterModeType;

inline std::string filterModeToString(FilterModeType mode) {
    switch (mode) {
        case FilterMode::Inbox: return "inbox";
        case FilterMode::Today: return "today";
        case FilterMode::All: return "all";
        case FilterMode::Completed: return "completed";
        case FilterMode::Project: return "project";
    }
    return "inbox";
}

inline FilterModeType stringToFilterMode(const std::string& s) {
    if (s == "today" || s == "Today") return FilterMode::Today;
    if (s == "all" || s == "All") return FilterMode::All;
    if (s == "completed" || s == "Completed") return FilterMode::Completed;
    if (s == "project" || s == "Project") return FilterMode::Project;
    return FilterMode::Inbox;
}

namespace AppLifecycleState {
    enum Type {
        Foreground = 0,
        ActiveFrameThumbnail = 1,
        InvisibleBackground = 2
    };
}
typedef AppLifecycleState::Type AppLifecycleStateType;

namespace NetworkConnectionType {
    enum Type {
        WiFi = 0,
        Cellular = 1,
        Offline = 2
    };
}
typedef NetworkConnectionType::Type NetworkConnectionTypeVal;

struct TaskViewItem {
    std::string id;
    std::string serverId;
    std::string content;
    std::string description;
    bool isCompleted;
    int priority; // Todoist API priority: 4 = P1 (Urgent), 3 = P2 (High), 2 = P3 (Medium), 1 = P4 (Normal)
    std::string dueDate;
    std::string projectId;
    std::string projectDisplayName;
    int64_t localUpdatedAt;
    SyncStateType syncState;
    std::string syncBadgeText;
    bool isSelected;

    TaskViewItem()
        : isCompleted(false)
        , priority(1)
        , projectId("inbox")
        , projectDisplayName("inbox")
        , localUpdatedAt(0)
        , syncState(SyncState::Synced)
        , syncBadgeText("Synced")
        , isSelected(false)
    {}

    // Maps Todoist API priority (4..1) to user-facing P1..P4 badge
    std::string priorityLabel() const {
        switch (priority) {
            case 4: return "P1";
            case 3: return "P2";
            case 2: return "P3";
            default: return "P4";
        }
    }

    // Color hex for 1440x1440 square-screen high-contrast priority badge
    std::string priorityColorHex() const {
        switch (priority) {
            case 4: return "#E44332"; // P1 Urgent Crimson
            case 3: return "#F59E0B"; // P2 High Amber
            case 2: return "#3B82F6"; // P3 Medium Blue
            default: return "#64748B"; // P4 Normal Slate
        }
    }
};

struct ActiveFrameSnapshot {
    int totalIncompleteCount;
    int todayIncompleteCount;
    int pendingMutationCount;
    int deadLetterCount;
    bool isSyncing;
    bool requiresAuth;
    std::string statusHeadline;
    std::vector<TaskViewItem> topTasks;

    ActiveFrameSnapshot()
        : totalIncompleteCount(0)
        , todayIncompleteCount(0)
        , pendingMutationCount(0)
        , deadLetterCount(0)
        , isSyncing(false)
        , requiresAuth(false)
        , statusHeadline("Setup Token")
    {}
};

} // namespace todoist

#endif // TODOIST_UI_TYPES_HPP
