#include "todoist/sync_metadata_repo.hpp"
#include <iostream>

namespace todoist {

SyncMetadataRepository::SyncMetadataRepository(DatabaseManager& dbManager,
                                               TaskRepository& taskRepo,
                                               SyncCommandQueue& commandQueue)
    : m_db(dbManager)
    , m_taskRepo(taskRepo)
    , m_commandQueue(commandQueue)
{
}

bool SyncMetadataRepository::getMetadata(const std::string& key, std::string& valueOut) {
    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT value FROM sync_metadata WHERE key = ? LIMIT 1;";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    const unsigned char* val = sqlite3_column_text(stmt, 0);
    valueOut = val ? reinterpret_cast<const char*>(val) : "";
    sqlite3_finalize(stmt);
    return true;
}

bool SyncMetadataRepository::setMetadata(const std::string& key, const std::string& value) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "INSERT INTO sync_metadata (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

bool SyncMetadataRepository::deleteMetadata(const std::string& key) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = "DELETE FROM sync_metadata WHERE key = ?;";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;

    return tx.commit();
}

std::string SyncMetadataRepository::getSyncToken() {
    std::string token;
    if (getMetadata("sync_token", token) && !token.empty()) {
        return token;
    }
    return "*"; // Default initial cursor for Todoist sync API
}

bool SyncMetadataRepository::setSyncToken(const std::string& token) {
    return setMetadata("sync_token", token);
}

bool SyncMetadataRepository::applyDeltaAndSaveToken(const std::vector<Task>& upsertedTasks,
                                                    const std::vector<std::string>& deletedTaskIds,
                                                    const std::vector<std::string>& acknowledgedCommandUuids,
                                                    const std::string& newSyncToken) {
    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    // 1. Upsert remote tasks
    for (std::vector<Task>::const_iterator it = upsertedTasks.begin(); it != upsertedTasks.end(); ++it) {
        if (!m_taskRepo.upsertRemoteTask(*it)) {
            return false;
        }
    }

    // 2. Delete remote tasks
    for (std::vector<std::string>::const_iterator it = deletedTaskIds.begin(); it != deletedTaskIds.end(); ++it) {
        if (!m_taskRepo.deleteRemoteTask(*it)) {
            return false;
        }
    }

    // 3. Prune acknowledged commands
    if (!acknowledgedCommandUuids.empty()) {
        if (!m_commandQueue.markBatchAcknowledged(acknowledgedCommandUuids)) {
            return false;
        }
    }

    // 4. Update sync token
    if (!newSyncToken.empty()) {
        if (!setSyncToken(newSyncToken)) {
            return false;
        }
    }

    return tx.commit();
}

} // namespace todoist
