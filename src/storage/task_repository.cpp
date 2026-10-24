#include "todoist/task_repository.hpp"
#include "todoist/uuid_util.hpp"
#include <sstream>
#include <iostream>

namespace todoist {

namespace {

std::string escapeJson(const std::string& s) {
    std::ostringstream o;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) <= 0x1f) {
                    // Control characters
                } else {
                    o << c;
                }
        }
    }
    return o.str();
}

} // anonymous namespace

TaskRepository::TaskRepository(DatabaseManager& dbManager)
    : m_db(dbManager)
{
}

bool TaskRepository::createTaskLocally(Task& task, std::string commandUuid) {
    if (task.id.empty()) {
        task.id = util::generateTempId();
    }
    if (commandUuid.empty()) {
        commandUuid = util::generateUuidV4();
    }

    task.sync_state = SyncState::LocallyCreated;
    task.local_updated_at = util::currentEpochMs();

    // Serialize command args
    std::ostringstream jsonStream;
    jsonStream << "{\"content\":\"" << escapeJson(task.content) << "\""
               << ",\"description\":\"" << escapeJson(task.description) << "\""
               << ",\"priority\":" << task.priority
               << ",\"project_id\":\"" << escapeJson(task.project_id) << "\"";
    if (!task.due_date.empty()) {
        jsonStream << ",\"due_date\":\"" << escapeJson(task.due_date) << "\"";
    }
    jsonStream << "}";
    std::string argsJson = jsonStream.str();

    // Atomic SQLite Transaction
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    // 1. Insert into tasks
    sqlite3_stmt* taskStmt = NULL;
    const char* taskSql =
        "INSERT INTO tasks (id, server_id, content, description, is_completed, priority, due_date, project_id, local_updated_at, sync_state) "
        "VALUES (?, NULL, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(m_db.handle(), taskSql, -1, &taskStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(taskStmt, 1, task.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(taskStmt, 2, task.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(taskStmt, 3, task.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(taskStmt, 4, task.is_completed ? 1 : 0);
    sqlite3_bind_int(taskStmt, 5, task.priority);
    if (!task.due_date.empty()) {
        sqlite3_bind_text(taskStmt, 6, task.due_date.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(taskStmt, 6);
    }
    sqlite3_bind_text(taskStmt, 7, task.project_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(taskStmt, 8, task.local_updated_at);
    sqlite3_bind_text(taskStmt, 9, syncStateToString(task.sync_state).c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(taskStmt);
    sqlite3_finalize(taskStmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    // Insert baseline for conflict resolution
    sqlite3_stmt* baseStmt = NULL;
    const char* baseSql =
        "INSERT OR REPLACE INTO task_baselines (id, content, description, is_completed, priority, due_date, project_id, local_updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(m_db.handle(), baseSql, -1, &baseStmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(baseStmt, 1, task.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(baseStmt, 2, task.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(baseStmt, 3, task.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(baseStmt, 4, task.is_completed ? 1 : 0);
        sqlite3_bind_int(baseStmt, 5, task.priority);
        if (!task.due_date.empty()) {
            sqlite3_bind_text(baseStmt, 6, task.due_date.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(baseStmt, 6);
        }
        sqlite3_bind_text(baseStmt, 7, task.project_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(baseStmt, 8, task.local_updated_at);
        rc = sqlite3_step(baseStmt);
        sqlite3_finalize(baseStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }
    } else {
        return false;
    }

    // 2. Insert into sync_commands
    sqlite3_stmt* cmdStmt = NULL;
    const char* cmdSql =
        "INSERT INTO sync_commands (uuid, type, temp_id, args_json, status, created_at, retry_count) "
        "VALUES (?, 'item_add', ?, ?, 'pending', ?, 0);";

    if (sqlite3_prepare_v2(m_db.handle(), cmdSql, -1, &cmdStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(cmdStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cmdStmt, 2, task.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cmdStmt, 3, argsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(cmdStmt, 4, task.local_updated_at);

    rc = sqlite3_step(cmdStmt);
    sqlite3_finalize(cmdStmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    return tx.commit();
}

bool TaskRepository::updateTaskLocally(const std::string& taskId,
                                       const std::string& newContent,
                                       const std::string& newDescription,
                                       int priority,
                                       const std::string& dueDate,
                                       std::string commandUuid) {
    if (commandUuid.empty()) {
        commandUuid = util::generateUuidV4();
    }
    int64_t nowMs = util::currentEpochMs();

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    Task existing;
    if (!getTaskById(taskId, existing)) {
        return false;
    }

    SyncStateType newState = (existing.sync_state == SyncState::LocallyCreated)
                             ? SyncState::LocallyCreated
                             : SyncState::LocallyUpdated;

    // 1. Update task in database
    sqlite3_stmt* taskStmt = NULL;
    const char* taskSql =
        "UPDATE tasks SET content = ?, description = ?, priority = ?, due_date = ?, local_updated_at = ?, sync_state = ? "
        "WHERE id = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), taskSql, -1, &taskStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(taskStmt, 1, newContent.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(taskStmt, 2, newDescription.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(taskStmt, 3, priority);
    if (!dueDate.empty()) {
        sqlite3_bind_text(taskStmt, 4, dueDate.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(taskStmt, 4);
    }
    sqlite3_bind_int64(taskStmt, 5, nowMs);
    sqlite3_bind_text(taskStmt, 6, syncStateToString(newState).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(taskStmt, 7, taskId.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(taskStmt);
    sqlite3_finalize(taskStmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    // 2. Insert item_update command with actually changed fields
    std::ostringstream jsonStream;
    jsonStream << "{\"id\":\"" << escapeJson(taskId) << "\"";
    if (newContent != existing.content) {
        jsonStream << ",\"content\":\"" << escapeJson(newContent) << "\"";
    }
    if (newDescription != existing.description) {
        jsonStream << ",\"description\":\"" << escapeJson(newDescription) << "\"";
    }
    if (priority != existing.priority) {
        jsonStream << ",\"priority\":" << priority;
    }
    if (dueDate != existing.due_date) {
        if (!dueDate.empty()) {
            jsonStream << ",\"due_date\":\"" << escapeJson(dueDate) << "\"";
        } else {
            jsonStream << ",\"due_date\":null";
        }
    }
    jsonStream << "}";

    sqlite3_stmt* cmdStmt = NULL;
    const char* cmdSql =
        "INSERT INTO sync_commands (uuid, type, temp_id, args_json, status, created_at, retry_count) "
        "VALUES (?, 'item_update', NULL, ?, 'pending', ?, 0);";

    if (sqlite3_prepare_v2(m_db.handle(), cmdSql, -1, &cmdStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(cmdStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cmdStmt, 2, jsonStream.str().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(cmdStmt, 3, nowMs);

    rc = sqlite3_step(cmdStmt);
    sqlite3_finalize(cmdStmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    return tx.commit();
}

bool TaskRepository::setTaskCompletedLocally(const std::string& taskId,
                                             bool completed,
                                             std::string commandUuid) {
    if (commandUuid.empty()) {
        commandUuid = util::generateUuidV4();
    }
    int64_t nowMs = util::currentEpochMs();

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    Task existing;
    if (!getTaskById(taskId, existing)) {
        return false;
    }

    SyncStateType newState = (existing.sync_state == SyncState::LocallyCreated)
                             ? SyncState::LocallyCreated
                             : SyncState::LocallyUpdated;

    // 1. Update task completed status
    sqlite3_stmt* taskStmt = NULL;
    const char* taskSql =
        "UPDATE tasks SET is_completed = ?, local_updated_at = ?, sync_state = ? WHERE id = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), taskSql, -1, &taskStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(taskStmt, 1, completed ? 1 : 0);
    sqlite3_bind_int64(taskStmt, 2, nowMs);
    sqlite3_bind_text(taskStmt, 3, syncStateToString(newState).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(taskStmt, 4, taskId.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(taskStmt);
    sqlite3_finalize(taskStmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    // 2. Insert item_complete or item_uncomplete command
    std::string cmdType = completed ? "item_complete" : "item_uncomplete";
    std::string argsJson = "{\"id\":\"" + escapeJson(taskId) + "\"}";

    sqlite3_stmt* cmdStmt = NULL;
    const char* cmdSql =
        "INSERT INTO sync_commands (uuid, type, temp_id, args_json, status, created_at, retry_count) "
        "VALUES (?, ?, NULL, ?, 'pending', ?, 0);";

    if (sqlite3_prepare_v2(m_db.handle(), cmdSql, -1, &cmdStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(cmdStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cmdStmt, 2, cmdType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cmdStmt, 3, argsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(cmdStmt, 4, nowMs);

    rc = sqlite3_step(cmdStmt);
    sqlite3_finalize(cmdStmt);
    if (rc != SQLITE_DONE) {
        return false;
    }

    return tx.commit();
}

bool TaskRepository::deleteTaskLocally(const std::string& taskId,
                                       std::string commandUuid) {
    if (commandUuid.empty()) {
        commandUuid = util::generateUuidV4();
    }
    int64_t nowMs = util::currentEpochMs();

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    Task existing;
    if (!getTaskById(taskId, existing)) {
        return false;
    }

    if (existing.sync_state == SyncState::LocallyCreated) {
        // Query status and retry_count of item_add command to check if demonstrably unsent
        sqlite3_stmt* checkCmd = NULL;
        const char* checkSql =
            "SELECT status, retry_count FROM sync_commands WHERE temp_id = ? AND type = 'item_add';";
        bool isDemonstrablyUnsent = false;
        if (sqlite3_prepare_v2(m_db.handle(), checkSql, -1, &checkCmd, NULL) == SQLITE_OK) {
            sqlite3_bind_text(checkCmd, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(checkCmd) == SQLITE_ROW) {
                const unsigned char* st = sqlite3_column_text(checkCmd, 0);
                std::string status = st ? reinterpret_cast<const char*>(st) : "";
                int retries = sqlite3_column_int(checkCmd, 1);
                if (status == "pending" && retries == 0) {
                    isDemonstrablyUnsent = true;
                }
            }
            sqlite3_finalize(checkCmd);
        }

        if (isDemonstrablyUnsent) {
            // Unsent! Delete task locally and cancel pending commands referencing this temp_id
            sqlite3_stmt* delTask = NULL;
            if (sqlite3_prepare_v2(m_db.handle(), "DELETE FROM tasks WHERE id = ?;", -1, &delTask, NULL) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_text(delTask, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(delTask);
            sqlite3_finalize(delTask);
            if (rc != SQLITE_DONE) return false;

            sqlite3_stmt* delBase = NULL;
            if (sqlite3_prepare_v2(m_db.handle(), "DELETE FROM task_baselines WHERE id = ?;", -1, &delBase, NULL) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_text(delBase, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
            int rcBase = sqlite3_step(delBase);
            sqlite3_finalize(delBase);
            if (rcBase != SQLITE_DONE) return false;

            sqlite3_stmt* delCmd = NULL;
            const char* delCmdSql =
                "DELETE FROM sync_commands WHERE temp_id = ? OR json_extract(args_json, '$.id') = ?;";
            if (sqlite3_prepare_v2(m_db.handle(), delCmdSql, -1, &delCmd, NULL) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_text(delCmd, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(delCmd, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(delCmd);
            sqlite3_finalize(delCmd);
            if (rc != SQLITE_DONE) return false;

            return tx.commit();
        }
    }

    // In-flight create or existing synced/updated task: preserve tombstone and enqueue item_delete
    sqlite3_stmt* taskStmt = NULL;
    const char* taskSql =
        "UPDATE tasks SET sync_state = 'locally_deleted', local_updated_at = ? WHERE id = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), taskSql, -1, &taskStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(taskStmt, 1, nowMs);
    sqlite3_bind_text(taskStmt, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(taskStmt);
    sqlite3_finalize(taskStmt);
    if (rc != SQLITE_DONE) return false;

    std::string argsJson = "{\"id\":\"" + escapeJson(taskId) + "\"}";
    sqlite3_stmt* cmdStmt = NULL;
    const char* cmdSql =
        "INSERT INTO sync_commands (uuid, type, temp_id, args_json, status, created_at, retry_count) "
        "VALUES (?, 'item_delete', NULL, ?, 'pending', ?, 0);";

    if (sqlite3_prepare_v2(m_db.handle(), cmdSql, -1, &cmdStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(cmdStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cmdStmt, 2, argsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(cmdStmt, 3, nowMs);

    rc = sqlite3_step(cmdStmt);
    sqlite3_finalize(cmdStmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

bool TaskRepository::getTaskById(const std::string& taskId, Task& taskOut) {
    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT id, server_id, content, description, is_completed, priority, due_date, project_id, local_updated_at, sync_state "
        "FROM tasks WHERE id = ? LIMIT 1;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    taskOut.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const unsigned char* serverId = sqlite3_column_text(stmt, 1);
    taskOut.server_id = serverId ? reinterpret_cast<const char*>(serverId) : "";
    taskOut.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const unsigned char* desc = sqlite3_column_text(stmt, 3);
    taskOut.description = desc ? reinterpret_cast<const char*>(desc) : "";
    taskOut.is_completed = (sqlite3_column_int(stmt, 4) != 0);
    taskOut.priority = sqlite3_column_int(stmt, 5);
    const unsigned char* dueDate = sqlite3_column_text(stmt, 6);
    taskOut.due_date = dueDate ? reinterpret_cast<const char*>(dueDate) : "";
    taskOut.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    taskOut.local_updated_at = sqlite3_column_int64(stmt, 8);
    taskOut.sync_state = stringToSyncState(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)));

    sqlite3_finalize(stmt);
    return true;
}

bool TaskRepository::getAllActiveTasks(std::vector<Task>& tasksOut) {
    tasksOut.clear();
    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT id, server_id, content, description, is_completed, priority, due_date, project_id, local_updated_at, sync_state "
        "FROM tasks WHERE sync_state != 'locally_deleted' AND is_completed = 0 "
        "ORDER BY priority DESC, local_updated_at DESC;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task t;
        t.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const unsigned char* serverId = sqlite3_column_text(stmt, 1);
        t.server_id = serverId ? reinterpret_cast<const char*>(serverId) : "";
        t.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const unsigned char* desc = sqlite3_column_text(stmt, 3);
        t.description = desc ? reinterpret_cast<const char*>(desc) : "";
        t.is_completed = (sqlite3_column_int(stmt, 4) != 0);
        t.priority = sqlite3_column_int(stmt, 5);
        const unsigned char* dueDate = sqlite3_column_text(stmt, 6);
        t.due_date = dueDate ? reinterpret_cast<const char*>(dueDate) : "";
        t.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        t.local_updated_at = sqlite3_column_int64(stmt, 8);
        t.sync_state = stringToSyncState(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)));

        tasksOut.push_back(t);
    }

    sqlite3_finalize(stmt);
    return true;
}

bool TaskRepository::getTasksByProject(const std::string& projectId, std::vector<Task>& tasksOut, bool includeCompleted) {
    tasksOut.clear();
    sqlite3_stmt* stmt = NULL;
    std::string sql =
        "SELECT id, server_id, content, description, is_completed, priority, due_date, project_id, local_updated_at, sync_state "
        "FROM tasks WHERE sync_state != 'locally_deleted' AND project_id = ? ";
    if (!includeCompleted) {
        sql += "AND is_completed = 0 ";
    }
    sql += "ORDER BY priority DESC, local_updated_at DESC;";

    if (sqlite3_prepare_v2(m_db.handle(), sql.c_str(), -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, projectId.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task t;
        t.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const unsigned char* serverId = sqlite3_column_text(stmt, 1);
        t.server_id = serverId ? reinterpret_cast<const char*>(serverId) : "";
        t.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const unsigned char* desc = sqlite3_column_text(stmt, 3);
        t.description = desc ? reinterpret_cast<const char*>(desc) : "";
        t.is_completed = (sqlite3_column_int(stmt, 4) != 0);
        t.priority = sqlite3_column_int(stmt, 5);
        const unsigned char* dueDate = sqlite3_column_text(stmt, 6);
        t.due_date = dueDate ? reinterpret_cast<const char*>(dueDate) : "";
        t.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        t.local_updated_at = sqlite3_column_int64(stmt, 8);
        t.sync_state = stringToSyncState(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)));

        tasksOut.push_back(t);
    }

    sqlite3_finalize(stmt);
    return true;
}

bool TaskRepository::upsertRemoteTask(const Task& remoteTask) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    Task local;
    bool exists = getTaskById(remoteTask.id, local);

    // Check if there are pending/in-flight commands for this task
    bool hasPendingCommands = false;
    sqlite3_stmt* cmdCountStmt = NULL;
    const char* cmdCountSql =
        "SELECT count(*) FROM sync_commands "
        "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
        "AND status IN ('pending', 'in_flight');";
    if (sqlite3_prepare_v2(m_db.handle(), cmdCountSql, -1, &cmdCountStmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(cmdCountStmt, 1, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(cmdCountStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(cmdCountStmt) == SQLITE_ROW) {
            hasPendingCommands = (sqlite3_column_int(cmdCountStmt, 0) > 0);
        }
        sqlite3_finalize(cmdCountStmt);
    }

    if (exists) {
        if (local.sync_state == SyncState::LocallyDeleted) {
            // Local delete wins on push; do not resurrect
            return tx.commit();
        }

        if (local.sync_state == SyncState::LocallyUpdated || hasPendingCommands) {
            // Field-level merge against task_baselines
            Task baseline;
            bool hasBaseline = false;
            sqlite3_stmt* baseStmt = NULL;
            const char* baseSql =
                "SELECT content, description, priority, due_date, is_completed "
                "FROM task_baselines WHERE id = ?;";
            if (sqlite3_prepare_v2(m_db.handle(), baseSql, -1, &baseStmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(baseStmt, 1, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(baseStmt) == SQLITE_ROW) {
                    hasBaseline = true;
                    baseline.content = reinterpret_cast<const char*>(sqlite3_column_text(baseStmt, 0));
                    const unsigned char* d = sqlite3_column_text(baseStmt, 1);
                    baseline.description = d ? reinterpret_cast<const char*>(d) : "";
                    baseline.priority = sqlite3_column_int(baseStmt, 2);
                    const unsigned char* due = sqlite3_column_text(baseStmt, 3);
                    baseline.due_date = due ? reinterpret_cast<const char*>(due) : "";
                    baseline.is_completed = (sqlite3_column_int(baseStmt, 4) != 0);
                }
                sqlite3_finalize(baseStmt);
            }

            bool contentLocalDirty = false;
            bool descLocalDirty = false;
            bool priorityLocalDirty = false;
            bool dueLocalDirty = false;
            bool completedLocalDirty = false;

            if (hasBaseline) {
                contentLocalDirty = (local.content != baseline.content);
                descLocalDirty = (local.description != baseline.description);
                priorityLocalDirty = (local.priority != baseline.priority);
                dueLocalDirty = (local.due_date != baseline.due_date);
                completedLocalDirty = (local.is_completed != baseline.is_completed);
            }

            // Inspect pending or in-flight commands to derive dirty fields and ensure pending local intent is preserved
            sqlite3_stmt* pendingCmdStmt = NULL;
            const char* pendingCmdSql =
                "SELECT type, args_json FROM sync_commands "
                "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                "AND status IN ('pending', 'in_flight');";
            if (sqlite3_prepare_v2(m_db.handle(), pendingCmdSql, -1, &pendingCmdStmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(pendingCmdStmt, 1, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(pendingCmdStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(pendingCmdStmt) == SQLITE_ROW) {
                    const unsigned char* tText = sqlite3_column_text(pendingCmdStmt, 0);
                    const unsigned char* aText = sqlite3_column_text(pendingCmdStmt, 1);
                    std::string cmdType = tText ? reinterpret_cast<const char*>(tText) : "";
                    std::string cmdArgs = aText ? reinterpret_cast<const char*>(aText) : "";

                    if (cmdType == "item_add") {
                        contentLocalDirty = true;
                        descLocalDirty = true;
                        priorityLocalDirty = true;
                        dueLocalDirty = true;
                        completedLocalDirty = true;
                    } else if (cmdType == "item_update") {
                        if (cmdArgs.find("\"content\":") != std::string::npos) contentLocalDirty = true;
                        if (cmdArgs.find("\"description\":") != std::string::npos) descLocalDirty = true;
                        if (cmdArgs.find("\"priority\":") != std::string::npos) priorityLocalDirty = true;
                        if (cmdArgs.find("\"due_date\":") != std::string::npos) dueLocalDirty = true;
                    } else if (cmdType == "item_complete" || cmdType == "item_uncomplete" || cmdType == "item_close") {
                        completedLocalDirty = true;
                    }
                }
                sqlite3_finalize(pendingCmdStmt);
            }

            if (!hasBaseline) {
                baseline = local;
                if (contentLocalDirty && remoteTask.content != local.content) {
                    baseline.content = remoteTask.content;
                }
                if (descLocalDirty && remoteTask.description != local.description) {
                    baseline.description = remoteTask.description;
                }
                if (priorityLocalDirty && remoteTask.priority != local.priority) {
                    baseline.priority = remoteTask.priority;
                }
                if (dueLocalDirty && remoteTask.due_date != local.due_date) {
                    baseline.due_date = remoteTask.due_date;
                }
                if (completedLocalDirty && remoteTask.is_completed != local.is_completed) {
                    baseline.is_completed = remoteTask.is_completed;
                }
            }

            bool contentRemoteChanged = (remoteTask.content != baseline.content);
            bool descRemoteChanged = (remoteTask.description != baseline.description);
            bool priorityRemoteChanged = (remoteTask.priority != baseline.priority);
            bool dueRemoteChanged = (remoteTask.due_date != baseline.due_date);
            bool completedRemoteChanged = (remoteTask.is_completed != baseline.is_completed);

            // Merge Content
            if (contentLocalDirty && contentRemoteChanged) {
                if (remoteTask.local_updated_at > local.local_updated_at) {
                    local.content = remoteTask.content;
                }
            } else if (!contentLocalDirty && contentRemoteChanged) {
                local.content = remoteTask.content;
            }

            // Merge Description
            if (descLocalDirty && descRemoteChanged) {
                if (remoteTask.local_updated_at > local.local_updated_at) {
                    local.description = remoteTask.description;
                }
            } else if (!descLocalDirty && descRemoteChanged) {
                local.description = remoteTask.description;
            }

            // Merge Priority
            if (priorityLocalDirty && priorityRemoteChanged) {
                if (remoteTask.local_updated_at > local.local_updated_at) {
                    local.priority = remoteTask.priority;
                }
            } else if (!priorityLocalDirty && priorityRemoteChanged) {
                local.priority = remoteTask.priority;
            }

            // Merge Due Date
            if (dueLocalDirty && dueRemoteChanged) {
                if (remoteTask.local_updated_at > local.local_updated_at) {
                    local.due_date = remoteTask.due_date;
                }
            } else if (!dueLocalDirty && dueRemoteChanged) {
                local.due_date = remoteTask.due_date;
            }

            // Merge Completed
            if (completedLocalDirty && completedRemoteChanged) {
                if (remoteTask.local_updated_at > local.local_updated_at) {
                    local.is_completed = remoteTask.is_completed;
                }
            } else if (!completedLocalDirty && completedRemoteChanged) {
                local.is_completed = remoteTask.is_completed;
            }

            local.server_id = remoteTask.server_id.empty() ? remoteTask.id : remoteTask.server_id;
            local.project_id = remoteTask.project_id;
            local.sync_state = hasPendingCommands ? SyncState::LocallyUpdated : SyncState::Synced;

            bool remoteWonContent = (contentLocalDirty && contentRemoteChanged && remoteTask.local_updated_at > local.local_updated_at);
            bool remoteWonDesc = (descLocalDirty && descRemoteChanged && remoteTask.local_updated_at > local.local_updated_at);
            bool remoteWonPriority = (priorityLocalDirty && priorityRemoteChanged && remoteTask.local_updated_at > local.local_updated_at);
            bool remoteWonDue = (dueLocalDirty && dueRemoteChanged && remoteTask.local_updated_at > local.local_updated_at);

            // Align never-sent pending item_update command args with accepted field winners (status = 'pending' AND retry_count = 0)
            if (remoteWonContent) {
                sqlite3_stmt* alignStmt = NULL;
                const char* alignSql =
                    "UPDATE sync_commands SET args_json = json_set(args_json, '$.content', ?) "
                    "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                    "AND status = 'pending' AND retry_count = 0 AND type = 'item_update' "
                    "AND json_extract(args_json, '$.content') IS NOT NULL;";
                if (sqlite3_prepare_v2(m_db.handle(), alignSql, -1, &alignStmt, NULL) != SQLITE_OK) {
                    return false;
                }
                sqlite3_bind_text(alignStmt, 1, local.content.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(alignStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(alignStmt, 3, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                int rc = sqlite3_step(alignStmt);
                sqlite3_finalize(alignStmt);
                if (rc != SQLITE_DONE) {
                    return false;
                }
            }
            if (remoteWonDesc) {
                sqlite3_stmt* alignStmt = NULL;
                const char* alignSql =
                    "UPDATE sync_commands SET args_json = json_set(args_json, '$.description', ?) "
                    "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                    "AND status = 'pending' AND retry_count = 0 AND type = 'item_update' "
                    "AND json_extract(args_json, '$.description') IS NOT NULL;";
                if (sqlite3_prepare_v2(m_db.handle(), alignSql, -1, &alignStmt, NULL) != SQLITE_OK) {
                    return false;
                }
                sqlite3_bind_text(alignStmt, 1, local.description.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(alignStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(alignStmt, 3, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                int rc = sqlite3_step(alignStmt);
                sqlite3_finalize(alignStmt);
                if (rc != SQLITE_DONE) {
                    return false;
                }
            }
            if (remoteWonPriority) {
                sqlite3_stmt* alignStmt = NULL;
                const char* alignSql =
                    "UPDATE sync_commands SET args_json = json_set(args_json, '$.priority', ?) "
                    "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                    "AND status = 'pending' AND retry_count = 0 AND type = 'item_update' "
                    "AND json_extract(args_json, '$.priority') IS NOT NULL;";
                if (sqlite3_prepare_v2(m_db.handle(), alignSql, -1, &alignStmt, NULL) != SQLITE_OK) {
                    return false;
                }
                sqlite3_bind_int(alignStmt, 1, local.priority);
                sqlite3_bind_text(alignStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(alignStmt, 3, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                int rc = sqlite3_step(alignStmt);
                sqlite3_finalize(alignStmt);
                if (rc != SQLITE_DONE) {
                    return false;
                }
            }
            if (remoteWonDue) {
                sqlite3_stmt* alignStmt = NULL;
                const char* alignSql = NULL;
                if (!local.due_date.empty()) {
                    alignSql =
                        "UPDATE sync_commands SET args_json = json_set(args_json, '$.due_date', ?) "
                        "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                        "AND status = 'pending' AND retry_count = 0 AND type = 'item_update' "
                        "AND json_type(args_json, '$.due_date') IS NOT NULL;";
                } else {
                    alignSql =
                        "UPDATE sync_commands SET args_json = json_set(args_json, '$.due_date', null) "
                        "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                        "AND status = 'pending' AND retry_count = 0 AND type = 'item_update' "
                        "AND json_type(args_json, '$.due_date') IS NOT NULL;";
                }
                if (sqlite3_prepare_v2(m_db.handle(), alignSql, -1, &alignStmt, NULL) != SQLITE_OK) {
                    return false;
                }
                if (!local.due_date.empty()) {
                    sqlite3_bind_text(alignStmt, 1, local.due_date.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(alignStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(alignStmt, 3, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_text(alignStmt, 1, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(alignStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                }
                int rc = sqlite3_step(alignStmt);
                sqlite3_finalize(alignStmt);
                if (rc != SQLITE_DONE) {
                    return false;
                }
            }

            // For in-flight or retry commands whose payload cannot be modified, enqueue a fresh corrective command to enforce the remote winner
            if (remoteWonContent || remoteWonDesc || remoteWonPriority || remoteWonDue) {
                sqlite3_stmt* checkInFlightStmt = NULL;
                const char* checkInFlightSql =
                    "SELECT count(*) FROM sync_commands "
                    "WHERE (temp_id = ? OR json_extract(args_json, '$.id') = ?) "
                    "AND (status = 'in_flight' OR (status = 'pending' AND retry_count > 0)) "
                    "AND type = 'item_update';";
                bool needsCorrection = false;
                if (sqlite3_prepare_v2(m_db.handle(), checkInFlightSql, -1, &checkInFlightStmt, NULL) != SQLITE_OK) {
                    return false;
                }
                sqlite3_bind_text(checkInFlightStmt, 1, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(checkInFlightStmt, 2, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(checkInFlightStmt) == SQLITE_ROW) {
                    needsCorrection = (sqlite3_column_int(checkInFlightStmt, 0) > 0);
                }
                sqlite3_finalize(checkInFlightStmt);

                if (needsCorrection) {
                    std::string correctUuid = util::generateUuidV4();
                    int64_t nowMs = util::currentEpochMs();
                    std::ostringstream correctJson;
                    correctJson << "{\"id\":\"" << escapeJson(remoteTask.id) << "\"";
                    if (remoteWonContent) {
                        correctJson << ",\"content\":\"" << escapeJson(local.content) << "\"";
                    }
                    if (remoteWonDesc) {
                        correctJson << ",\"description\":\"" << escapeJson(local.description) << "\"";
                    }
                    if (remoteWonPriority) {
                        correctJson << ",\"priority\":" << local.priority;
                    }
                    if (remoteWonDue) {
                        if (!local.due_date.empty()) {
                            correctJson << ",\"due_date\":\"" << escapeJson(local.due_date) << "\"";
                        } else {
                            correctJson << ",\"due_date\":null";
                        }
                    }
                    correctJson << "}";

                    sqlite3_stmt* insertCorrectStmt = NULL;
                    const char* insertCorrectSql =
                        "INSERT INTO sync_commands (uuid, type, temp_id, args_json, status, created_at, retry_count) "
                        "VALUES (?, 'item_update', NULL, ?, 'pending', ?, 0);";
                    if (sqlite3_prepare_v2(m_db.handle(), insertCorrectSql, -1, &insertCorrectStmt, NULL) != SQLITE_OK) {
                        return false;
                    }
                    sqlite3_bind_text(insertCorrectStmt, 1, correctUuid.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(insertCorrectStmt, 2, correctJson.str().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(insertCorrectStmt, 3, nowMs);
                    int rc = sqlite3_step(insertCorrectStmt);
                    sqlite3_finalize(insertCorrectStmt);
                    if (rc != SQLITE_DONE) {
                        return false;
                    }
                }
            }
        } else {
            local = remoteTask;
            local.sync_state = SyncState::Synced;
        }
    } else {
        local = remoteTask;
        local.sync_state = SyncState::Synced;
    }

    // Update baseline with accepted remote changes (propagating all failures)
    sqlite3_stmt* updateBaseStmt = NULL;
    const char* updateBaseSql =
        "INSERT OR REPLACE INTO task_baselines (id, content, description, is_completed, priority, due_date, project_id, local_updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(m_db.handle(), updateBaseSql, -1, &updateBaseStmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(updateBaseStmt, 1, remoteTask.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateBaseStmt, 2, remoteTask.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updateBaseStmt, 3, remoteTask.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(updateBaseStmt, 4, remoteTask.is_completed ? 1 : 0);
    sqlite3_bind_int(updateBaseStmt, 5, remoteTask.priority);
    if (!remoteTask.due_date.empty()) {
        sqlite3_bind_text(updateBaseStmt, 6, remoteTask.due_date.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(updateBaseStmt, 6);
    }
    sqlite3_bind_text(updateBaseStmt, 7, remoteTask.project_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(updateBaseStmt, 8, remoteTask.local_updated_at);

    int rcBase = sqlite3_step(updateBaseStmt);
    sqlite3_finalize(updateBaseStmt);
    if (rcBase != SQLITE_DONE) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "INSERT INTO tasks (id, server_id, content, description, is_completed, priority, due_date, project_id, local_updated_at, sync_state) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "server_id = excluded.server_id, content = excluded.content, description = excluded.description, "
        "is_completed = excluded.is_completed, priority = excluded.priority, due_date = excluded.due_date, "
        "project_id = excluded.project_id, local_updated_at = excluded.local_updated_at, sync_state = excluded.sync_state;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, local.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, local.server_id.empty() ? local.id.c_str() : local.server_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, local.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, local.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, local.is_completed ? 1 : 0);
    sqlite3_bind_int(stmt, 6, local.priority);
    if (!local.due_date.empty()) {
        sqlite3_bind_text(stmt, 7, local.due_date.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 7);
    }
    sqlite3_bind_text(stmt, 8, local.project_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 9, local.local_updated_at);
    sqlite3_bind_text(stmt, 10, syncStateToString(local.sync_state).c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

bool TaskRepository::deleteRemoteTask(const std::string& taskId) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* baseStmt = NULL;
    const char* baseSql = "DELETE FROM task_baselines WHERE id = ?;";
    if (sqlite3_prepare_v2(m_db.handle(), baseSql, -1, &baseStmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(baseStmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
    int rcBase = sqlite3_step(baseStmt);
    sqlite3_finalize(baseStmt);
    if (rcBase != SQLITE_DONE) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = "DELETE FROM tasks WHERE id = ? OR server_id = ?;";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

} // namespace todoist
