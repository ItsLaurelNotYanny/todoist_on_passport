#include "todoist/temp_id_reconciler.hpp"
#include "todoist/types.hpp"
#include <ctime>
#include <vector>

namespace todoist {

namespace {

struct ScopedStmt {
    sqlite3_stmt* stmt;
    ScopedStmt() : stmt(NULL) {}
    ~ScopedStmt() {
        if (stmt) {
            sqlite3_finalize(stmt);
            stmt = NULL;
        }
    }
    operator sqlite3_stmt*() const { return stmt; }
    sqlite3_stmt** operator&() { return &stmt; }
private:
    ScopedStmt(const ScopedStmt&);
    ScopedStmt& operator=(const ScopedStmt&);
};

} // anonymous namespace

TempIdReconciler::TempIdReconciler(DatabaseManager& dbManager)
    : m_db(dbManager)
{
}

bool TempIdReconciler::reconcile(const std::map<std::string, std::string>& tempIdMapping) {
    if (tempIdMapping.empty()) return true;

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    // 1. Prepared statement for updating tasks table primary key
    const char* updateTaskSql =
        "UPDATE tasks SET id = ?, server_id = ? WHERE id = ?;";
    ScopedStmt updateTaskStmt;
    if (sqlite3_prepare_v2(m_db.handle(), updateTaskSql, -1, &updateTaskStmt, NULL) != SQLITE_OK) {
        return false;
    }

    // 2. Prepared statement for updating task_baselines table primary key
    const char* updateBaseSql =
        "UPDATE task_baselines SET id = ? WHERE id = ?;";
    ScopedStmt updateBaseStmt;
    if (sqlite3_prepare_v2(m_db.handle(), updateBaseSql, -1, &updateBaseStmt, NULL) != SQLITE_OK) {
        return false;
    }

    // 3. Prepared statement for updating temp_id in sync_commands
    const char* updateCmdTempIdSql =
        "UPDATE sync_commands SET temp_id = ? WHERE temp_id = ?;";
    ScopedStmt updateCmdTempIdStmt;
    if (sqlite3_prepare_v2(m_db.handle(), updateCmdTempIdSql, -1, &updateCmdTempIdStmt, NULL) != SQLITE_OK) {
        return false;
    }

    // 4. Prepared statement to update structural id field in sync_commands (json_set)
    const char* updateCmdArgsSql =
        "UPDATE sync_commands SET args_json = json_set(args_json, '$.id', ?) "
        "WHERE json_extract(args_json, '$.id') = ?;";
    ScopedStmt updateCmdArgsStmt;
    if (sqlite3_prepare_v2(m_db.handle(), updateCmdArgsSql, -1, &updateCmdArgsStmt, NULL) != SQLITE_OK) {
        return false;
    }

    // 5. Prepared statement to check remaining commands for determining task sync_state
    const char* queryCmdsSql =
        "SELECT type FROM sync_commands WHERE temp_id = ? OR json_extract(args_json, '$.id') = ?;";
    ScopedStmt queryCmdsStmt;
    if (sqlite3_prepare_v2(m_db.handle(), queryCmdsSql, -1, &queryCmdsStmt, NULL) != SQLITE_OK) {
        return false;
    }

    // 6. Prepared statement to update task sync_state
    const char* updateStateSql =
        "UPDATE tasks SET sync_state = ? WHERE id = ?;";
    ScopedStmt updateStateStmt;
    if (sqlite3_prepare_v2(m_db.handle(), updateStateSql, -1, &updateStateStmt, NULL) != SQLITE_OK) {
        return false;
    }

    // 7. Prepared statement to record persistent temp_id mappings
    sqlite3_exec(m_db.handle(),
        "CREATE TABLE IF NOT EXISTS temp_id_mappings ("
        "  temp_id TEXT PRIMARY KEY,"
        "  server_id TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");", NULL, NULL, NULL);

    const char* insertMappingSql =
        "INSERT OR REPLACE INTO temp_id_mappings (temp_id, server_id, created_at) VALUES (?, ?, ?);";
    ScopedStmt insertMappingStmt;
    if (sqlite3_prepare_v2(m_db.handle(), insertMappingSql, -1, &insertMappingStmt, NULL) != SQLITE_OK) {
        return false;
    }

    for (std::map<std::string, std::string>::const_iterator mapIt = tempIdMapping.begin();
         mapIt != tempIdMapping.end();
         ++mapIt) {
        const std::string& tempId = mapIt->first;
        const std::string& serverId = mapIt->second;

        // 1. Remap in tasks
        sqlite3_reset(updateTaskStmt);
        sqlite3_bind_text(updateTaskStmt, 1, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateTaskStmt, 2, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateTaskStmt, 3, tempId.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(updateTaskStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }

        // 2. Remap in task_baselines
        sqlite3_reset(updateBaseStmt);
        sqlite3_bind_text(updateBaseStmt, 1, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateBaseStmt, 2, tempId.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(updateBaseStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }

        // 3. Remap temp_id column in sync_commands
        sqlite3_reset(updateCmdTempIdStmt);
        sqlite3_bind_text(updateCmdTempIdStmt, 1, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateCmdTempIdStmt, 2, tempId.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(updateCmdTempIdStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }

        // 4. Update structural id field in sync_commands
        sqlite3_reset(updateCmdArgsStmt);
        sqlite3_bind_text(updateCmdArgsStmt, 1, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateCmdArgsStmt, 2, tempId.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(updateCmdArgsStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }

        // 5. Query remaining commands for this task to compute proper final sync_state
        sqlite3_reset(queryCmdsStmt);
        sqlite3_bind_text(queryCmdsStmt, 1, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(queryCmdsStmt, 2, serverId.c_str(), -1, SQLITE_TRANSIENT);

        bool hasDelete = false;
        bool hasUpdateOrComplete = false;
        while ((rc = sqlite3_step(queryCmdsStmt)) == SQLITE_ROW) {
            const unsigned char* typeText = sqlite3_column_text(queryCmdsStmt, 0);
            if (typeText) {
                std::string cmdType = reinterpret_cast<const char*>(typeText);
                if (cmdType == "item_delete") {
                    hasDelete = true;
                } else if (cmdType == "item_update" || cmdType == "item_complete" || cmdType == "item_uncomplete") {
                    hasUpdateOrComplete = true;
                }
            }
        }
        if (rc != SQLITE_DONE) {
            return false;
        }

        SyncStateType newState = SyncState::Synced;
        if (hasDelete) {
            newState = SyncState::LocallyDeleted;
        } else if (hasUpdateOrComplete) {
            newState = SyncState::LocallyUpdated;
        }

        // 6. Update task sync_state
        sqlite3_reset(updateStateStmt);
        sqlite3_bind_text(updateStateStmt, 1, syncStateToString(newState).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStateStmt, 2, serverId.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(updateStateStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }

        // 7. Record mapping in temp_id_mappings table
        sqlite3_reset(insertMappingStmt);
        sqlite3_bind_text(insertMappingStmt, 1, tempId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertMappingStmt, 2, serverId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insertMappingStmt, 3, static_cast<sqlite3_int64>(time(NULL)));
        rc = sqlite3_step(insertMappingStmt);
        if (rc != SQLITE_DONE) {
            return false;
        }
    }

    return tx.commit();
}

} // namespace todoist
