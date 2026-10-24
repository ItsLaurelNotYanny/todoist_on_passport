#ifndef TODOIST_TASK_REPOSITORY_HPP
#define TODOIST_TASK_REPOSITORY_HPP

#include "todoist/types.hpp"
#include "todoist/db_manager.hpp"
#include <string>
#include <vector>

namespace todoist {

class TaskRepository {
public:
    explicit TaskRepository(DatabaseManager& dbManager);

    // Atomic Local Mutations (Guaranteed single SQLite transaction)
    bool createTaskLocally(Task& task, std::string commandUuid = "");
    bool updateTaskLocally(const std::string& taskId,
                           const std::string& newContent,
                           const std::string& newDescription,
                           int priority,
                           const std::string& dueDate,
                           std::string commandUuid = "");
    bool setTaskCompletedLocally(const std::string& taskId,
                                 bool completed,
                                 std::string commandUuid = "");
    bool deleteTaskLocally(const std::string& taskId,
                           std::string commandUuid = "");

    // Queries
    bool getTaskById(const std::string& taskId, Task& taskOut);
    bool getAllActiveTasks(std::vector<Task>& tasksOut);
    bool getTasksByProject(const std::string& projectId, std::vector<Task>& tasksOut, bool includeCompleted = false);

    // Remote Delta Ingestion (Called by SyncEngine during pull reconciliation)
    bool upsertRemoteTask(const Task& remoteTask);
    bool deleteRemoteTask(const std::string& taskId);

private:
    DatabaseManager& m_db;

    TaskRepository(const TaskRepository&);
    TaskRepository& operator=(const TaskRepository&);
};

} // namespace todoist

#endif // TODOIST_TASK_REPOSITORY_HPP
