#include "todoist/sync_command_queue.hpp"
#include "todoist/uuid_util.hpp"
#include <iostream>

namespace todoist {

SyncCommandQueue::SyncCommandQueue(DatabaseManager& dbManager)
    : m_db(dbManager)
{
}

bool SyncCommandQueue::getPendingCommands(std::vector<SyncCommand>& commandsOut, int limit) {
    commandsOut.clear();
    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT c.uuid, c.type, c.temp_id, c.args_json, c.status, c.created_at, c.retry_count, c.last_error "
        "FROM sync_commands c "
        "WHERE c.status = 'pending' "
        "  AND NOT EXISTS ( "
        "    SELECT 1 FROM sync_commands o "
        "    WHERE o.status = 'in_flight' "
        "      AND ( "
        "        (c.temp_id IS NOT NULL AND c.temp_id != '' AND (o.temp_id = c.temp_id OR json_extract(o.args_json, '$.id') = c.temp_id)) "
        "        OR "
        "        (json_extract(c.args_json, '$.id') IS NOT NULL AND json_extract(c.args_json, '$.id') != '' AND (o.temp_id = json_extract(c.args_json, '$.id') OR json_extract(o.args_json, '$.id') = json_extract(c.args_json, '$.id'))) "
        "      ) "
        "  ) "
        "ORDER BY c.created_at ASC, c.rowid ASC LIMIT ?;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SyncCommand cmd;
        cmd.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        cmd.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const unsigned char* tempId = sqlite3_column_text(stmt, 2);
        cmd.temp_id = tempId ? reinterpret_cast<const char*>(tempId) : "";
        cmd.args_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        cmd.status = stringToCommandStatus(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        cmd.created_at = sqlite3_column_int64(stmt, 5);
        cmd.retry_count = sqlite3_column_int(stmt, 6);
        const unsigned char* lastErr = sqlite3_column_text(stmt, 7);
        cmd.last_error = lastErr ? reinterpret_cast<const char*>(lastErr) : "";

        commandsOut.push_back(cmd);
    }

    sqlite3_finalize(stmt);
    return true;
}

int SyncCommandQueue::getPendingCount() {
    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT count(*) FROM sync_commands WHERE status = 'pending';";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int SyncCommandQueue::getInFlightCount() {
    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT count(*) FROM sync_commands WHERE status = 'in_flight';";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int SyncCommandQueue::getDeadLetterCount() {
    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT count(*) FROM dead_letter_commands;";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool SyncCommandQueue::markCommandsInFlight(const std::vector<std::string>& commandUuids) {
    if (commandUuids.empty()) return true;

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = "UPDATE sync_commands SET status = 'in_flight' WHERE uuid = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    for (std::vector<std::string>::const_iterator it = commandUuids.begin(); it != commandUuids.end(); ++it) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, it->c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    return tx.commit();
}

bool SyncCommandQueue::markCommandAcknowledged(const std::string& commandUuid) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = "DELETE FROM sync_commands WHERE uuid = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

bool SyncCommandQueue::markBatchAcknowledged(const std::vector<std::string>& commandUuids) {
    if (commandUuids.empty()) return true;

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = "DELETE FROM sync_commands WHERE uuid = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    for (std::vector<std::string>::const_iterator it = commandUuids.begin(); it != commandUuids.end(); ++it) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, it->c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return false;
        }
    }

    sqlite3_finalize(stmt);
    return tx.commit();
}

bool SyncCommandQueue::markCommandFailedTransient(const std::string& commandUuid, const std::string& errorMsg) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "UPDATE sync_commands SET status = 'pending', retry_count = retry_count + 1, last_error = ? "
        "WHERE uuid = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, errorMsg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, commandUuid.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

bool SyncCommandQueue::markCommandFailedPermanent(const std::string& commandUuid, const std::string& errorReason) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    // 1. Fetch existing command
    sqlite3_stmt* fetchStmt = NULL;
    const char* fetchSql = "SELECT type, args_json FROM sync_commands WHERE uuid = ?;";
    if (sqlite3_prepare_v2(m_db.handle(), fetchSql, -1, &fetchStmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(fetchStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(fetchStmt) != SQLITE_ROW) {
        sqlite3_finalize(fetchStmt);
        return false;
    }

    std::string type = reinterpret_cast<const char*>(sqlite3_column_text(fetchStmt, 0));
    std::string argsJson = reinterpret_cast<const char*>(sqlite3_column_text(fetchStmt, 1));
    sqlite3_finalize(fetchStmt);

    // 2. Insert into dead_letter_commands
    sqlite3_stmt* insertStmt = NULL;
    const char* insertSql =
        "INSERT INTO dead_letter_commands (uuid, type, args_json, failed_at, error_reason) "
        "VALUES (?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(m_db.handle(), insertSql, -1, &insertStmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(insertStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 3, argsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insertStmt, 4, util::currentEpochMs());
    sqlite3_bind_text(insertStmt, 5, errorReason.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(insertStmt);
    sqlite3_finalize(insertStmt);
    if (rc != SQLITE_DONE) return false;

    // 3. Delete from sync_commands
    sqlite3_stmt* delStmt = NULL;
    const char* delSql = "DELETE FROM sync_commands WHERE uuid = ?;";
    if (sqlite3_prepare_v2(m_db.handle(), delSql, -1, &delStmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(delStmt, 1, commandUuid.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(delStmt);
    sqlite3_finalize(delStmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

} // namespace todoist
