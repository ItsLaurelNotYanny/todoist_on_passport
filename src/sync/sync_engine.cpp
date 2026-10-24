#include "todoist/sync/sync_engine.hpp"
#include "picojson.h"
#include <sqlite3.h>
#include <sstream>
#include <iostream>

namespace todoist {

namespace {

std::string getCommandTargetTaskId(const SyncCommand& cmd,
                                   const std::map<std::string, std::string>& validatedMapping) {
    if (!cmd.temp_id.empty()) {
        std::map<std::string, std::string>::const_iterator it = validatedMapping.find(cmd.temp_id);
        if (it != validatedMapping.end()) {
            return it->second;
        }
        return cmd.temp_id;
    }

    // Extract ID structurally using picojson to decode escapes and handle whitespace
    picojson::value v;
    std::string err = picojson::parse(v, cmd.args_json);
    if (err.empty() && v.is<picojson::object>()) {
        const picojson::object& obj = v.get<picojson::object>();
        picojson::object::const_iterator it = obj.find("id");
        if (it != obj.end() && it->second.is<std::string>()) {
            std::string rawId = it->second.get<std::string>();
            std::map<std::string, std::string>::const_iterator itMap = validatedMapping.find(rawId);
            if (itMap != validatedMapping.end()) {
                return itMap->second;
            }
            return rawId;
        }
    }
    return "";
}

struct CycleGuard {
    pthread_mutex_t* mutex;
    bool* cycleActive;
    SyncEngineStateType* state;
    SyncCycleStatusType finalStatus;

    CycleGuard(pthread_mutex_t* m, bool* ca, SyncEngineStateType* s)
        : mutex(m), cycleActive(ca), state(s), finalStatus(SyncCycleStatus::FatalError)
    {}

    ~CycleGuard() {
        pthread_mutex_lock(mutex);
        *cycleActive = false;
        if (*state == SyncEngineState::AuthError) {
            // Preserve explicit AuthError set during cycle (e.g. via setBearerToken(""))
        } else if (finalStatus == SyncCycleStatus::Success) {
            *state = SyncEngineState::Idle;
        } else if (finalStatus == SyncCycleStatus::AuthError) {
            *state = SyncEngineState::AuthError;
        } else if (finalStatus == SyncCycleStatus::RetryPending) {
            *state = SyncEngineState::RetryPending;
        } else {
            *state = SyncEngineState::Idle;
        }
        pthread_mutex_unlock(mutex);
    }
};

} // anonymous namespace

SyncEngine::SyncEngine(DatabaseManager& db,
                       TaskRepository& taskRepo,
                       SyncCommandQueue& cmdQueue,
                       TempIdReconciler& tempIdReconciler,
                       SyncMetadataRepository& metadataRepo,
                       const std::string& endpointUrl,
                       const std::string& bearerToken)
    : m_db(db)
    , m_taskRepo(taskRepo)
    , m_cmdQueue(cmdQueue)
    , m_tempIdReconciler(tempIdReconciler)
    , m_metadataRepo(metadataRepo)
    , m_endpointUrl(endpointUrl)
    , m_bearerToken(bearerToken)
    , m_maxBatchSize(50)
    , m_state(SyncEngineState::Idle)
    , m_cycleActive(false)
{
    pthread_mutex_init(&m_mutex, NULL);
}

SyncEngine::~SyncEngine() {
    pthread_mutex_destroy(&m_mutex);
}

bool SyncEngine::initialize() {
    pthread_mutex_lock(&m_mutex);
    // Startup recovery: reset orphaned in_flight commands to pending and increment retry_count
    // to preserve send-history ambiguity across restarts.
    const char* sql =
        "UPDATE sync_commands "
        "SET status = 'pending', retry_count = retry_count + 1 "
        "WHERE status = 'in_flight';";

    int rc = sqlite3_exec(m_db.handle(), sql, NULL, NULL, NULL);
    pthread_mutex_unlock(&m_mutex);
    return (rc == SQLITE_OK);
}

void SyncEngine::setBearerToken(const std::string& token) {
    pthread_mutex_lock(&m_mutex);
    if (!token.empty()) {
        if (token != m_bearerToken) {
            m_bearerToken = token;
            if (m_state == SyncEngineState::AuthError) {
                m_state = SyncEngineState::Idle;
            }
        }
    } else {
        m_bearerToken.clear();
        m_state = SyncEngineState::AuthError;
    }
    pthread_mutex_unlock(&m_mutex);
}

std::string SyncEngine::getBearerToken() const {
    pthread_mutex_lock(&m_mutex);
    std::string tok = m_bearerToken;
    pthread_mutex_unlock(&m_mutex);
    return tok;
}

void SyncEngine::setEndpointUrl(const std::string& url) {
    pthread_mutex_lock(&m_mutex);
    m_endpointUrl = url;
    pthread_mutex_unlock(&m_mutex);
}

std::string SyncEngine::getEndpointUrl() const {
    pthread_mutex_lock(&m_mutex);
    std::string url = m_endpointUrl;
    pthread_mutex_unlock(&m_mutex);
    return url;
}

void SyncEngine::setMaxBatchSize(size_t batchSize) {
    pthread_mutex_lock(&m_mutex);
    if (batchSize == 0) {
        m_maxBatchSize = 50;
    } else if (batchSize > 100) {
        m_maxBatchSize = 100;
    } else {
        m_maxBatchSize = batchSize;
    }
    pthread_mutex_unlock(&m_mutex);
}

size_t SyncEngine::getMaxBatchSize() const {
    pthread_mutex_lock(&m_mutex);
    size_t sz = m_maxBatchSize;
    pthread_mutex_unlock(&m_mutex);
    return sz;
}

SyncEngineStateType SyncEngine::getState() const {
    pthread_mutex_lock(&m_mutex);
    SyncEngineStateType st = m_state;
    pthread_mutex_unlock(&m_mutex);
    return st;
}

bool SyncEngine::resetInFlightBatchToPending(const std::vector<SyncCommand>& batch, bool incrementRetry) {
    if (batch.empty()) {
        return true;
    }

    DatabaseManager::ScopedTransaction tx(m_db);
    if (!tx.isActive()) {
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql = incrementRetry
        ? "UPDATE sync_commands SET status = 'pending', retry_count = retry_count + 1 WHERE uuid = ?;"
        : "UPDATE sync_commands SET status = 'pending' WHERE uuid = ?;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        sqlite3_bind_text(stmt, 1, batch[i].uuid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    return tx.commit();
}

bool SyncEngine::getPreRequestCleanTaskIds(std::set<std::string>& outTaskIds) {
    outTaskIds.clear();
    sqlite3_stmt* stmt = NULL;
    // Pre-request clean tasks: sync_state == 'synced' and zero pending/in-flight commands
    const char* sql =
        "SELECT t.id FROM tasks t "
        "WHERE t.sync_state = 'synced' "
        "AND NOT EXISTS ("
        "    SELECT 1 FROM sync_commands sc "
        "    WHERE sc.status IN ('pending', 'in_flight') "
        "    AND (sc.temp_id = t.id OR json_extract(sc.args_json, '$.id') = t.id)"
        ");";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    int stepRc = SQLITE_OK;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char* tid = sqlite3_column_text(stmt, 0);
        if (tid) {
            outTaskIds.insert(reinterpret_cast<const char*>(tid));
        }
    }
    sqlite3_finalize(stmt);
    if (stepRc != SQLITE_DONE) {
        return false;
    }
    return true;
}

bool SyncEngine::cascadeCreateFailure(const std::string& tempId, const std::string& errorMsg) {
    if (tempId.empty()) {
        return true;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT uuid FROM sync_commands "
        "WHERE status IN ('pending', 'in_flight') "
        "AND (temp_id = ? OR json_extract(args_json, '$.id') = ?);";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, tempId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, tempId.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<std::string> dependentUuids;
    int stepRc = SQLITE_OK;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char* u = sqlite3_column_text(stmt, 0);
        if (u) {
            dependentUuids.push_back(reinterpret_cast<const char*>(u));
        }
    }
    sqlite3_finalize(stmt);
    if (stepRc != SQLITE_DONE) {
        return false;
    }

    for (size_t i = 0; i < dependentUuids.size(); ++i) {
        if (!m_cmdQueue.markCommandFailedPermanent(
                dependentUuids[i], "Parent item_add failed permanently: " + errorMsg)) {
            return false;
        }
    }

    return true;
}

bool SyncEngine::advanceBaselinesForAcknowledgedCommands(
    const std::vector<SyncCommand>& inFlightCommands,
    const std::vector<std::string>& ackUuids,
    const std::map<std::string, std::string>& validatedMapping) {

    std::set<std::string> ackUuidSet(ackUuids.begin(), ackUuids.end());

    for (size_t i = 0; i < inFlightCommands.size(); ++i) {
        const SyncCommand& cmd = inFlightCommands[i];
        if (ackUuidSet.find(cmd.uuid) == ackUuidSet.end()) {
            continue;
        }

        std::string taskId = getCommandTargetTaskId(cmd, validatedMapping);
        if (taskId.empty()) {
            continue;
        }

        picojson::value v;
        std::string err = picojson::parse(v, cmd.args_json);
        if (!err.empty() || !v.is<picojson::object>()) {
            continue;
        }
        const picojson::object& args = v.get<picojson::object>();

        // Fetch current baseline or initialize from task
        Task baseline;
        bool hasBaseline = false;
        sqlite3_stmt* baseStmt = NULL;
        const char* baseSql = "SELECT content, description, priority, due_date, is_completed FROM task_baselines WHERE id = ?;";
        if (sqlite3_prepare_v2(m_db.handle(), baseSql, -1, &baseStmt, NULL) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(baseStmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
        int baseRc = sqlite3_step(baseStmt);
        if (baseRc == SQLITE_ROW) {
            hasBaseline = true;
            const unsigned char* c = sqlite3_column_text(baseStmt, 0);
            const unsigned char* d = sqlite3_column_text(baseStmt, 1);
            const unsigned char* due = sqlite3_column_text(baseStmt, 3);
            baseline.content = c ? reinterpret_cast<const char*>(c) : "";
            baseline.description = d ? reinterpret_cast<const char*>(d) : "";
            baseline.priority = sqlite3_column_int(baseStmt, 2);
            baseline.due_date = due ? reinterpret_cast<const char*>(due) : "";
            baseline.is_completed = (sqlite3_column_int(baseStmt, 4) != 0);
        } else if (baseRc != SQLITE_DONE) {
            sqlite3_finalize(baseStmt);
            return false;
        }
        sqlite3_finalize(baseStmt);

        if (!hasBaseline) {
            Task localTask;
            if (m_taskRepo.getTaskById(taskId, localTask)) {
                baseline = localTask;
            }
        }

        // Advance baseline fields modified by the acknowledged command
        if (cmd.type == "item_add" || cmd.type == "item_update") {
            picojson::object::const_iterator itC = args.find("content");
            if (itC != args.end() && itC->second.is<std::string>()) {
                baseline.content = itC->second.get<std::string>();
            }
            picojson::object::const_iterator itD = args.find("description");
            if (itD != args.end() && itD->second.is<std::string>()) {
                baseline.description = itD->second.get<std::string>();
            }
            picojson::object::const_iterator itP = args.find("priority");
            if (itP != args.end() && itP->second.is<double>()) {
                baseline.priority = static_cast<int>(itP->second.get<double>());
            }
            picojson::object::const_iterator itDue = args.find("due_date");
            if (itDue != args.end()) {
                if (itDue->second.is<std::string>()) {
                    baseline.due_date = itDue->second.get<std::string>();
                } else if (itDue->second.is<picojson::null>()) {
                    baseline.due_date = "";
                }
            }
        } else if (cmd.type == "item_close" || cmd.type == "item_complete") {
            baseline.is_completed = true;
        } else if (cmd.type == "item_uncomplete") {
            baseline.is_completed = false;
        }

        // Persist updated baseline in SQLite
        sqlite3_stmt* upsertStmt = NULL;
        const char* upsertSql =
            "INSERT INTO task_baselines (id, content, description, priority, due_date, is_completed) "
            "VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "content = excluded.content, description = excluded.description, "
            "priority = excluded.priority, due_date = excluded.due_date, "
            "is_completed = excluded.is_completed;";

        if (sqlite3_prepare_v2(m_db.handle(), upsertSql, -1, &upsertStmt, NULL) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(upsertStmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsertStmt, 2, baseline.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upsertStmt, 3, baseline.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(upsertStmt, 4, baseline.priority);
        sqlite3_bind_text(upsertStmt, 5, baseline.due_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(upsertStmt, 6, baseline.is_completed ? 1 : 0);

        int stepRc = sqlite3_step(upsertStmt);
        sqlite3_finalize(upsertStmt);
        if (stepRc != SQLITE_DONE) {
            return false;
        }
    }
    return true;
}

bool SyncEngine::finalizeAcknowledgedTaskStates(
    const std::vector<SyncCommand>& inFlightCommands,
    const std::vector<std::string>& ackUuids,
    const std::map<std::string, std::string>& validatedMapping) {

    std::set<std::string> ackUuidSet(ackUuids.begin(), ackUuids.end());
    std::set<std::string> touchedTaskIds;

    for (size_t i = 0; i < inFlightCommands.size(); ++i) {
        if (ackUuidSet.find(inFlightCommands[i].uuid) != ackUuidSet.end()) {
            std::string tid = getCommandTargetTaskId(inFlightCommands[i], validatedMapping);
            if (!tid.empty()) {
                touchedTaskIds.insert(tid);
            }
        }
    }

    for (std::set<std::string>::const_iterator it = touchedTaskIds.begin(); it != touchedTaskIds.end(); ++it) {
        const std::string& taskId = *it;
        Task currentTask;
        if (!m_taskRepo.getTaskById(taskId, currentTask)) {
            continue; // Task may have been already deleted
        }

        // Query remaining pending/in-flight commands referencing this task
        sqlite3_stmt* stmt = NULL;
        const char* sql =
            "SELECT count(*) FROM sync_commands "
            "WHERE status IN ('pending', 'in_flight') "
            "AND (temp_id = ? OR json_extract(args_json, '$.id') = ?);";

        if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(stmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);

        int remainingCommands = 0;
        int stepRc = sqlite3_step(stmt);
        if (stepRc == SQLITE_ROW) {
            remainingCommands = sqlite3_column_int(stmt, 0);
        } else if (stepRc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);

        // Tombstone Priority: If task is LocallyDeleted or has remaining commands
        if (currentTask.sync_state == SyncState::LocallyDeleted) {
            // Keep LocallyDeleted! Prune only if no remaining commands need this row
            if (remainingCommands == 0) {
                // Deletion resolved conclusively; check all DELETE steps (M2-R002)
                sqlite3_stmt* delTask = NULL;
                int rc = sqlite3_prepare_v2(m_db.handle(), "DELETE FROM tasks WHERE id = ?;", -1, &delTask, NULL);
                if (rc != SQLITE_OK) return false;
                sqlite3_bind_text(delTask, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(delTask);
                sqlite3_finalize(delTask);
                if (rc != SQLITE_DONE) return false;

                sqlite3_stmt* delBase = NULL;
                rc = sqlite3_prepare_v2(m_db.handle(), "DELETE FROM task_baselines WHERE id = ?;", -1, &delBase, NULL);
                if (rc != SQLITE_OK) return false;
                sqlite3_bind_text(delBase, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(delBase);
                sqlite3_finalize(delBase);
                if (rc != SQLITE_DONE) return false;
            }
            continue;
        }

        if (remainingCommands > 0) {
            // Newer local commands exist: keep dirty state (LocallyUpdated or LocallyCreated)
            continue;
        }

        // No remaining commands: mark Synced and update baseline
        sqlite3_stmt* updTask = NULL;
        const char* updSql = "UPDATE tasks SET sync_state = 'synced' WHERE id = ?;";
        if (sqlite3_prepare_v2(m_db.handle(), updSql, -1, &updTask, NULL) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(updTask, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(updTask) != SQLITE_DONE) {
            sqlite3_finalize(updTask);
            return false;
        }
        sqlite3_finalize(updTask);

        // Update baseline to match current task state
        sqlite3_stmt* baseStmt = NULL;
        const char* baseSql =
            "INSERT INTO task_baselines (id, content, description, priority, due_date, is_completed) "
            "VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "content = excluded.content, description = excluded.description, "
            "priority = excluded.priority, due_date = excluded.due_date, "
            "is_completed = excluded.is_completed;";

        if (sqlite3_prepare_v2(m_db.handle(), baseSql, -1, &baseStmt, NULL) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(baseStmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(baseStmt, 2, currentTask.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(baseStmt, 3, currentTask.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(baseStmt, 4, currentTask.priority);
        sqlite3_bind_text(baseStmt, 5, currentTask.due_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(baseStmt, 6, currentTask.is_completed ? 1 : 0);

        if (sqlite3_step(baseStmt) != SQLITE_DONE) {
            sqlite3_finalize(baseStmt);
            return false;
        }
        sqlite3_finalize(baseStmt);
    }

    return true;
}

bool SyncEngine::pruneAbsentSyncedTasksWithProvenance(
    const std::set<std::string>& preRequestCleanTaskIds,
    const std::set<std::string>& requestTouchedTaskIds,
    const std::vector<Task>& incomingTasks) {

    std::set<std::string> incomingServerIds;
    for (size_t i = 0; i < incomingTasks.size(); ++i) {
        incomingServerIds.insert(incomingTasks[i].id);
    }

    // Candidates = PreRequestCleanTasks \ RequestTouchedTasks
    for (std::set<std::string>::const_iterator it = preRequestCleanTaskIds.begin();
         it != preRequestCleanTaskIds.end(); ++it) {
        const std::string& taskId = *it;

        if (requestTouchedTaskIds.find(taskId) != requestTouchedTaskIds.end()) {
            // Touched by current request; exempt from absence-based pruning
            continue;
        }

        if (incomingServerIds.find(taskId) == incomingServerIds.end()) {
            // Absent from server snapshot! Verify again that task has no pending commands
            sqlite3_stmt* checkStmt = NULL;
            const char* checkSql =
                "SELECT count(*) FROM sync_commands "
                "WHERE status IN ('pending', 'in_flight') "
                "AND (temp_id = ? OR json_extract(args_json, '$.id') = ?);";

            if (sqlite3_prepare_v2(m_db.handle(), checkSql, -1, &checkStmt, NULL) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_text(checkStmt, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(checkStmt, 2, taskId.c_str(), -1, SQLITE_TRANSIENT);
            int count = 0;
            int stepRc = sqlite3_step(checkStmt);
            if (stepRc == SQLITE_ROW) {
                count = sqlite3_column_int(checkStmt, 0);
            } else if (stepRc != SQLITE_DONE) {
                sqlite3_finalize(checkStmt);
                return false;
            }
            sqlite3_finalize(checkStmt);

            if (count == 0) {
                // Prune absent clean task and baseline; strictly check step results (M2-R002)
                sqlite3_stmt* delT = NULL;
                int rc = sqlite3_prepare_v2(m_db.handle(), "DELETE FROM tasks WHERE id = ?;", -1, &delT, NULL);
                if (rc != SQLITE_OK) return false;
                sqlite3_bind_text(delT, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(delT);
                sqlite3_finalize(delT);
                if (rc != SQLITE_DONE) return false;

                sqlite3_stmt* delB = NULL;
                rc = sqlite3_prepare_v2(m_db.handle(), "DELETE FROM task_baselines WHERE id = ?;", -1, &delB, NULL);
                if (rc != SQLITE_OK) return false;
                sqlite3_bind_text(delB, 1, taskId.c_str(), -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(delB);
                sqlite3_finalize(delB);
                if (rc != SQLITE_DONE) return false;
            }
        }
    }

    return true;
}

bool SyncEngine::hasUnmappedProjects() {
    std::string inboxProjId;
    m_metadataRepo.getMetadata("inbox_project_id", inboxProjId);

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT DISTINCT project_id FROM tasks "
        "WHERE project_id IS NOT NULL AND project_id != '' AND project_id != 'inbox' "
        "AND sync_state != 'locally_deleted';";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    bool unmapped = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(stmt, 0);
        if (p) {
            std::string pId(reinterpret_cast<const char*>(p));
            if (!pId.empty() && pId != "inbox" && pId != inboxProjId) {
                std::string name;
                if (!m_metadataRepo.getMetadata("project_name:" + pId, name) || name.empty()) {
                    unmapped = true;
                    break;
                }
            }
        }
    }
    sqlite3_finalize(stmt);
    return unmapped;
}

SyncCycleResult SyncEngine::executeSyncCycle(ISyncTransport& transport) {
    pthread_mutex_lock(&m_mutex);
    if (m_cycleActive) {
        pthread_mutex_unlock(&m_mutex);
        SyncCycleResult busyRes;
        busyRes.status = SyncCycleStatus::Busy;
        busyRes.errorMessage = "Sync cycle already active";
        return busyRes;
    }

    if (m_state == SyncEngineState::AuthError) {
        pthread_mutex_unlock(&m_mutex);
        SyncCycleResult authRes;
        authRes.status = SyncCycleStatus::AuthError;
        authRes.errorMessage = "Authentication error: credentials suspended until updated";
        return authRes;
    }

    m_cycleActive = true;
    m_state = SyncEngineState::Syncing;
    std::string currentToken = m_metadataRepo.getSyncToken();
    if (currentToken != "*" && hasUnmappedProjects()) {
        currentToken = "*";
    }
    std::string bearer = m_bearerToken;
    std::string url = m_endpointUrl;
    size_t batchSize = m_maxBatchSize;
    pthread_mutex_unlock(&m_mutex);

    // RAII guard guarantees m_cycleActive reset and state transition on any return/exception (M2-R001)
    CycleGuard guard(&m_mutex, &m_cycleActive, &m_state);
    SyncCycleResult cycleResult;

    // 1. Record pre-request clean tasks for provenance-guarded full-sync pruning
    std::set<std::string> preRequestCleanTaskIds;
    if (!getPreRequestCleanTaskIds(preRequestCleanTaskIds)) {
        guard.finalStatus = SyncCycleStatus::FatalError;
        cycleResult.status = SyncCycleStatus::FatalError;
        cycleResult.errorMessage = "Failed to query pre-request clean task IDs";
        return cycleResult;
    }

    // 2. Claim pending batch atomically in SQLite before I/O
    FrozenRequest frozenRequest;
    if (!SyncPayloadBuilder::claimPendingBatch(m_db, batchSize, currentToken, frozenRequest)) {
        guard.finalStatus = SyncCycleStatus::FatalError;
        cycleResult.status = SyncCycleStatus::FatalError;
        cycleResult.errorMessage = "Failed to claim pending batch in SQLite";
        return cycleResult;
    }

    // 3. Execute synchronous transport call outside locks with exception safety (M2-R001)
    ISyncTransport::HttpResponse httpRes;
#if defined(__EXCEPTIONS) || defined(__cpp_exceptions)
    try {
        httpRes = transport.executeSyncPost(url, frozenRequest.serializedBody, bearer);
    } catch (const std::exception& e) {
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Database recovery failed after transport exception: " + std::string(e.what());
            return cycleResult;
        }
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = std::string("Transport exception: ") + e.what();
        return cycleResult;
    } catch (...) {
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Database recovery failed after unknown transport exception";
            return cycleResult;
        }
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = "Unknown transport exception";
        return cycleResult;
    }
#else
    httpRes = transport.executeSyncPost(url, frozenRequest.serializedBody, bearer);
#endif
    cycleResult.httpStatusCode = httpRes.statusCode;

    // 4. Handle HTTP Transport Errors
    if (httpRes.statusCode == 401) {
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, false)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Database recovery failed after HTTP 401 AuthError";
            return cycleResult;
        }

        pthread_mutex_lock(&m_mutex);
        bool tokenReplaced = (!m_bearerToken.empty() && m_bearerToken != bearer);
        pthread_mutex_unlock(&m_mutex);

        if (tokenReplaced) {
            guard.finalStatus = SyncCycleStatus::RetryPending;
            cycleResult.status = SyncCycleStatus::RetryPending;
            cycleResult.errorMessage = "Old token 401 superseded by replacement token";
            return cycleResult;
        }

        guard.finalStatus = SyncCycleStatus::AuthError;
        cycleResult.status = SyncCycleStatus::AuthError;
        cycleResult.errorMessage = "HTTP 401 Unauthorized";
        return cycleResult;
    }

    if (httpRes.statusCode == 429 || httpRes.statusCode >= 500 || httpRes.statusCode == 0 || httpRes.statusCode != 200) {
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Database recovery failed after HTTP transport error";
            return cycleResult;
        }
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = httpRes.errorMessage.empty()
            ? "HTTP transport error"
            : httpRes.errorMessage;
        return cycleResult;
    }

    // 5. Pre-validate and parse response in memory before acquiring DB locks
    ParsedDeltaResponse parsedResponse;
    if (!SyncResponseParser::validateAndParseResponse(httpRes.body, parsedResponse)) {
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Database recovery failed after response pre-validation error: " + parsedResponse.validationError;
            return cycleResult;
        }
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = "Response pre-validation failed: " + parsedResponse.validationError;
        return cycleResult;
    }

    // 6. Outcome-Gated Mapping Correlation
    std::map<std::string, std::string> validatedMapping;
    std::string mapError;
    if (!SyncResponseParser::correlateAndValidateMappings(
            frozenRequest.inFlightCommands, parsedResponse, validatedMapping, mapError)) {
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Database recovery failed after mapping error: " + mapError;
            return cycleResult;
        }
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = "Inconsistent mapping correlation: " + mapError;
        return cycleResult;
    }

    // 7. Single Checked Outer Transaction
    bool hasRetryableCommand = false;
    bool txCommitted = false;
    {
        DatabaseManager::ScopedTransaction tx(m_db);
        if (!tx.isActive()) {
            if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
                guard.finalStatus = SyncCycleStatus::FatalError;
                cycleResult.status = SyncCycleStatus::FatalError;
                cycleResult.errorMessage = "Database recovery failed after transaction acquisition error";
                return cycleResult;
            }
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Failed to acquire outer database transaction";
            return cycleResult;
        }

        bool allStepsOk = true;

        // 7.1 Outcome-Gated Temporary ID Reconciliation
        if (!validatedMapping.empty()) {
            if (!m_tempIdReconciler.reconcile(validatedMapping)) {
                allStepsOk = false;
            }
        }

        // 7.2 Command Status Triage
        std::vector<std::string> ackUuids;
        if (allStepsOk) {
            for (size_t i = 0; i < frozenRequest.inFlightCommands.size(); ++i) {
                const SyncCommand& cmd = frozenRequest.inFlightCommands[i];
                std::map<std::string, CommandOutcome>::const_iterator itOutcome =
                    parsedResponse.commandOutcomes.find(cmd.uuid);

                if (itOutcome != parsedResponse.commandOutcomes.end()) {
                    const CommandOutcome& outcome = itOutcome->second;
                    if (outcome.isAck) {
                        ackUuids.push_back(cmd.uuid);
                    } else if (outcome.isPermanent) {
                        if (!m_cmdQueue.markCommandFailedPermanent(cmd.uuid, outcome.error)) {
                            allStepsOk = false;
                            break;
                        }
                        if (cmd.type == "item_add") {
                            if (!cascadeCreateFailure(cmd.temp_id, outcome.error)) {
                                allStepsOk = false;
                                break;
                            }
                        }
                        cycleResult.commandsFailedCount++;
                    } else {
                        // Transient retryable failure (M2-R003)
                        hasRetryableCommand = true;
                        if (!m_cmdQueue.markCommandFailedTransient(cmd.uuid, outcome.error)) {
                            allStepsOk = false;
                            break;
                        }
                        cycleResult.commandsFailedCount++;
                    }
                } else {
                    // Missing status for in-flight command: treated as unresolved transient failure
                    hasRetryableCommand = true;
                    if (!m_cmdQueue.markCommandFailedTransient(cmd.uuid, "Missing status in server response")) {
                        allStepsOk = false;
                        break;
                    }
                    cycleResult.commandsFailedCount++;
                }
            }
        }

        // 7.3 Advance Baselines for Acknowledged Commands (M2-R008)
        if (allStepsOk && !ackUuids.empty()) {
            if (!advanceBaselinesForAcknowledgedCommands(frozenRequest.inFlightCommands, ackUuids, validatedMapping)) {
                allStepsOk = false;
            }
        }

        // 7.4 Upsert Incoming Items (3-way baseline merge)
        if (allStepsOk) {
            for (size_t i = 0; i < parsedResponse.upsertedTasks.size(); ++i) {
                if (!m_taskRepo.upsertRemoteTask(parsedResponse.upsertedTasks[i])) {
                    allStepsOk = false;
                    break;
                }
                cycleResult.tasksUpsertedCount++;
            }
        }

        // 7.5 Delete Remote Items
        if (allStepsOk) {
            for (size_t i = 0; i < parsedResponse.deletedTaskIds.size(); ++i) {
                if (!m_taskRepo.deleteRemoteTask(parsedResponse.deletedTaskIds[i])) {
                    allStepsOk = false;
                    break;
                }
                cycleResult.tasksDeletedCount++;
            }
        }

        // 7.6 Prune Acknowledged Commands
        if (allStepsOk && !ackUuids.empty()) {
            if (!m_cmdQueue.markBatchAcknowledged(ackUuids)) {
                allStepsOk = false;
            } else {
                cycleResult.commandsAcknowledgedCount = ackUuids.size();
            }
        }

        // 7.7 Tombstone-Aware Task State Finalization
        if (allStepsOk) {
            if (!finalizeAcknowledgedTaskStates(frozenRequest.inFlightCommands, ackUuids, validatedMapping)) {
                allStepsOk = false;
            }
        }

        // 7.8 Provenance-Guarded Full-Sync Absence Pruning
        if (allStepsOk && parsedResponse.isFullSync) {
            std::set<std::string> requestTouchedTaskIds;
            for (size_t i = 0; i < frozenRequest.inFlightCommands.size(); ++i) {
                std::string tid = getCommandTargetTaskId(frozenRequest.inFlightCommands[i], validatedMapping);
                if (!tid.empty()) {
                    requestTouchedTaskIds.insert(tid);
                }
            }
            for (std::map<std::string, std::string>::const_iterator it = validatedMapping.begin();
                 it != validatedMapping.end(); ++it) {
                requestTouchedTaskIds.insert(it->second);
            }

            if (!pruneAbsentSyncedTasksWithProvenance(
                    preRequestCleanTaskIds, requestTouchedTaskIds, parsedResponse.upsertedTasks)) {
                allStepsOk = false;
            } else {
                cycleResult.hasFullSyncCatchUpNeeded = true;
            }
        }

        // 7.9 Advance Sync Token
        if (allStepsOk) {
            if (!m_metadataRepo.setSyncToken(parsedResponse.syncToken)) {
                allStepsOk = false;
            }
        }

        // 7.10 Persist Discovered Project Names, Delete Removed Projects, and Update Inbox Project ID
        if (allStepsOk) {
            for (std::map<std::string, std::string>::const_iterator itP = parsedResponse.projectNames.begin();
                 itP != parsedResponse.projectNames.end(); ++itP) {
                if (!m_metadataRepo.setMetadata("project_name:" + itP->first, itP->second)) {
                    allStepsOk = false;
                    break;
                }
            }
        }
        if (allStepsOk && !parsedResponse.deletedProjectIds.empty()) {
            for (size_t i = 0; i < parsedResponse.deletedProjectIds.size(); ++i) {
                if (!m_metadataRepo.deleteMetadata("project_name:" + parsedResponse.deletedProjectIds[i])) {
                    allStepsOk = false;
                    break;
                }
            }
        }
        if (allStepsOk && parsedResponse.isFullSync && parsedResponse.hasProjectsField) {
            std::vector<std::string> staleProjectKeys;
            sqlite3_stmt* pKeyStmt = NULL;
            const char* pKeySql = "SELECT key FROM sync_metadata WHERE key LIKE 'project_name:%';";
            if (sqlite3_prepare_v2(m_db.handle(), pKeySql, -1, &pKeyStmt, NULL) != SQLITE_OK) {
                allStepsOk = false;
            } else {
                int pKeyRc = SQLITE_OK;
                while ((pKeyRc = sqlite3_step(pKeyStmt)) == SQLITE_ROW) {
                    const unsigned char* k = sqlite3_column_text(pKeyStmt, 0);
                    if (k) {
                        std::string kStr(reinterpret_cast<const char*>(k));
                        if (kStr.length() > 13) {
                            std::string pId = kStr.substr(13);
                            if (parsedResponse.projectNames.find(pId) == parsedResponse.projectNames.end()) {
                                staleProjectKeys.push_back(kStr);
                            }
                        }
                    }
                }
                sqlite3_finalize(pKeyStmt);
                if (pKeyRc != SQLITE_DONE) {
                    allStepsOk = false;
                }
            }
            if (allStepsOk) {
                for (size_t i = 0; i < staleProjectKeys.size(); ++i) {
                    if (!m_metadataRepo.deleteMetadata(staleProjectKeys[i])) {
                        allStepsOk = false;
                        break;
                    }
                }
            }
        }
        if (allStepsOk && !parsedResponse.inboxProjectId.empty()) {
            if (!m_metadataRepo.setMetadata("inbox_project_id", parsedResponse.inboxProjectId)) {
                allStepsOk = false;
            }
        }

        if (allStepsOk) {
            txCommitted = tx.commit();
        }
    } // tx is fully destroyed and rolled back here if txCommitted is false

    if (!txCommitted) {
        // Outer transaction failed; rollback in-flight batch in its own checked transaction (M2-R001, M2-R002)
        if (!resetInFlightBatchToPending(frozenRequest.inFlightCommands, true)) {
            guard.finalStatus = SyncCycleStatus::FatalError;
            cycleResult.status = SyncCycleStatus::FatalError;
            cycleResult.errorMessage = "Failed to commit transaction and recovery write failed";
            return cycleResult;
        }
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = "Outer transaction aborted; in-flight commands reset to pending";
        cycleResult.commandsAcknowledgedCount = 0;
        cycleResult.tasksUpsertedCount = 0;
        cycleResult.tasksDeletedCount = 0;
        return cycleResult;
    }

    if (hasRetryableCommand) {
        guard.finalStatus = SyncCycleStatus::RetryPending;
        cycleResult.status = SyncCycleStatus::RetryPending;
        cycleResult.errorMessage = "Batch completed with retryable command failure(s)";
    } else {
        guard.finalStatus = SyncCycleStatus::Success;
        cycleResult.status = SyncCycleStatus::Success;
        cycleResult.errorMessage = "";
    }
    return cycleResult;
}

SyncCycleResult SyncEngine::drainPendingQueue(ISyncTransport& transport, int maxBatches) {
    SyncCycleResult lastResult;
    int batchesRun = 0;

    while (batchesRun < maxBatches) {
        // Check if there are any eligible pending commands
        sqlite3_stmt* checkStmt = NULL;
        const char* checkSql = "SELECT count(*) FROM sync_commands WHERE status = 'pending';";
        int pendingCount = 0;
        if (sqlite3_prepare_v2(m_db.handle(), checkSql, -1, &checkStmt, NULL) != SQLITE_OK) {
            lastResult.status = SyncCycleStatus::FatalError;
            lastResult.errorMessage = "Failed to prepare count query for pending commands";
            return lastResult;
        }
        int countRc = sqlite3_step(checkStmt);
        if (countRc == SQLITE_ROW) {
            pendingCount = sqlite3_column_int(checkStmt, 0);
        } else {
            sqlite3_finalize(checkStmt);
            lastResult.status = SyncCycleStatus::FatalError;
            lastResult.errorMessage = "Failed to query pending command count";
            return lastResult;
        }
        sqlite3_finalize(checkStmt);

        if (pendingCount == 0 && batchesRun > 0) {
            // Queue completely drained
            break;
        }

        lastResult = executeSyncCycle(transport);
        batchesRun++;

        if (!lastResult.isSuccess()) {
            // Stop immediately on RetryPending, AuthError, or FatalError (M2-R003)
            break;
        }

        if (lastResult.commandsAcknowledgedCount == 0 && lastResult.commandsFailedCount == 0) {
            // No commands were claimed or processed in this cycle
            break;
        }
    }

    return lastResult;
}

} // namespace todoist
