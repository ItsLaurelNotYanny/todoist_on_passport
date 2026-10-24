#include "todoist/db_manager.hpp"
#include <sys/time.h>

namespace todoist {

DatabaseManager::DatabaseManager()
    : m_db(NULL)
    , m_hasTransactionOwner(false)
    , m_transactionDepth(0)
{
    pthread_mutex_init(&m_mutex, NULL);
    pthread_cond_init(&m_txCond, NULL);
}

DatabaseManager::~DatabaseManager() {
    close();
    pthread_cond_destroy(&m_txCond);
    pthread_mutex_destroy(&m_mutex);
}

bool DatabaseManager::open(const std::string& dbPath) {
    MutexLocker lock(&m_mutex);
    if (m_db) {
        closeLocked();
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(dbPath.c_str(), &m_db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (m_db) {
            sqlite3_close(m_db);
            m_db = NULL;
        }
        return false;
    }

    // Set pragmas
    char* err = NULL;
    sqlite3_exec(m_db, "PRAGMA foreign_keys = ON;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    if (dbPath != ":memory:") {
        sqlite3_exec(m_db, "PRAGMA journal_mode = WAL;", NULL, NULL, &err);
        if (err) { sqlite3_free(err); err = NULL; }
        sqlite3_exec(m_db, "PRAGMA synchronous = NORMAL;", NULL, NULL, &err);
        if (err) { sqlite3_free(err); err = NULL; }
    }

    // Set busy timeout to 5000 ms to avoid immediate lock errors
    sqlite3_busy_timeout(m_db, 5000);

    return true;
}

void DatabaseManager::close() {
    MutexLocker lock(&m_mutex);
    closeLocked();
}

void DatabaseManager::closeLocked() {
    if (m_db) {
        if (m_transactionDepth > 0 || sqlite3_get_autocommit(m_db) == 0) {
            rollbackLocked();
        }
        sqlite3_close(m_db);
        m_db = NULL;
    }
    m_transactionDepth = 0;
    m_hasTransactionOwner = false;
    pthread_cond_broadcast(&m_txCond);
}

bool DatabaseManager::isOpen() const {
    return m_db != NULL;
}

bool DatabaseManager::execute(const std::string& sql, std::string* errorOut) {
    MutexLocker lock(&m_mutex);
    if (!m_db) {
        if (errorOut) *errorOut = "Database not open";
        return false;
    }

    pthread_t self = pthread_self();
    if (m_transactionDepth > 0 && !(m_hasTransactionOwner && pthread_equal(m_transactionOwner, self))) {
        // Another thread owns the active transaction. Wait with bounded deadline.
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct timespec ts;
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = (tv.tv_usec + 100000) * 1000; // 100ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
        }

        while (m_transactionDepth > 0 && !(m_hasTransactionOwner && pthread_equal(m_transactionOwner, self))) {
            int waitRc = pthread_cond_timedwait(&m_txCond, &m_mutex, &ts);
            if (waitRc != 0) {
                if (errorOut) *errorOut = "Transaction owned by another thread";
                return false;
            }
        }
    }

    char* errMsg = NULL;
    int rc = sqlite3_exec(m_db, sql.c_str(), NULL, NULL, &errMsg);
    if (rc != SQLITE_OK) {
        if (errorOut && errMsg) {
            *errorOut = errMsg;
        }
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool DatabaseManager::beginImmediate(int timeoutMs) {
    if (timeoutMs == 0) {
        if (pthread_mutex_trylock(&m_mutex) != 0) {
            return false;
        }
    } else {
        pthread_mutex_lock(&m_mutex);
    }
    MutexLocker lock(&m_mutex, true);
    return beginImmediateLocked(timeoutMs);
}

bool DatabaseManager::beginImmediateLocked(int timeoutMs) {
    if (!m_db) return false;

    pthread_t self = pthread_self();
    if (m_transactionDepth > 0) {
        if (m_hasTransactionOwner && pthread_equal(m_transactionOwner, self)) {
            m_transactionDepth++;
            return true;
        }

        if (timeoutMs == 0) {
            return false;
        }

        // Another thread owns the active transaction. Wait with bounded deadline.
        int waitMs = (timeoutMs > 0 && timeoutMs < 100) ? timeoutMs : 100;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct timespec ts;
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = (tv.tv_usec + waitMs * 1000) * 1000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
        }

        while (m_transactionDepth > 0 && !(m_hasTransactionOwner && pthread_equal(m_transactionOwner, self))) {
            int waitRc = pthread_cond_timedwait(&m_txCond, &m_mutex, &ts);
            if (waitRc != 0) {
                return false;
            }
        }
    }

    if (m_transactionDepth > 0) {
        if (m_hasTransactionOwner && pthread_equal(m_transactionOwner, self)) {
            m_transactionDepth++;
            return true;
        }
        return false;
    }

    if (timeoutMs >= 0) {
        sqlite3_busy_timeout(m_db, timeoutMs);
    }
    char* errMsg = NULL;
    int rc = sqlite3_exec(m_db, "BEGIN IMMEDIATE;", NULL, NULL, &errMsg);
    if (timeoutMs >= 0) {
        sqlite3_busy_timeout(m_db, 5000);
    }
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    m_transactionDepth = 1;
    m_transactionOwner = self;
    m_hasTransactionOwner = true;
    return true;
}

bool DatabaseManager::commit() {
    MutexLocker lock(&m_mutex);
    return commitLocked();
}

bool DatabaseManager::commitLocked() {
    if (!m_db || m_transactionDepth <= 0) return false;
    if (m_hasTransactionOwner && !pthread_equal(m_transactionOwner, pthread_self())) {
        return false;
    }

    if (m_transactionDepth > 1) {
        m_transactionDepth--;
        return true; // Nested transaction boundary satisfied
    }

    char* errMsg = NULL;
    int rc = sqlite3_exec(m_db, "COMMIT;", NULL, NULL, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        if (sqlite3_get_autocommit(m_db) == 0) {
            m_transactionDepth = 1; // Keep recoverable for rollback
        } else {
            m_transactionDepth = 0;
            m_hasTransactionOwner = false;
            pthread_cond_broadcast(&m_txCond);
        }
        return false;
    }
    m_transactionDepth = 0;
    m_hasTransactionOwner = false;
    pthread_cond_broadcast(&m_txCond);
    return true;
}

bool DatabaseManager::rollback() {
    MutexLocker lock(&m_mutex);
    return rollbackLocked();
}

bool DatabaseManager::rollbackLocked() {
    if (!m_db) return false;
    if (m_hasTransactionOwner && !pthread_equal(m_transactionOwner, pthread_self())) {
        return false;
    }
    if (m_transactionDepth <= 0 && sqlite3_get_autocommit(m_db) != 0) {
        return false;
    }

    m_transactionDepth = 0;
    m_hasTransactionOwner = false;
    pthread_cond_broadcast(&m_txCond);

    char* errMsg = NULL;
    int rc = sqlite3_exec(m_db, "ROLLBACK;", NULL, NULL, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return (rc == SQLITE_OK);
}

bool DatabaseManager::hasForeignTransaction() const {
    MutexLocker lock(const_cast<pthread_mutex_t*>(&m_mutex));
    return (m_transactionDepth > 0 && m_hasTransactionOwner && !pthread_equal(m_transactionOwner, pthread_self()));
}

DatabaseManager::ScopedSnapshotLock::ScopedSnapshotLock(DatabaseManager& db)
    : m_db(db)
    , m_locked(false)
{
    if (!m_db.isOpen()) {
        return;
    }
    if (pthread_mutex_trylock(&m_db.m_mutex) == 0) {
        if (m_db.m_hasTransactionOwner && !pthread_equal(m_db.m_transactionOwner, pthread_self())) {
            pthread_mutex_unlock(&m_db.m_mutex);
        } else {
            m_locked = true;
        }
    }
}

DatabaseManager::ScopedSnapshotLock::~ScopedSnapshotLock() {
    if (m_locked) {
        pthread_mutex_unlock(&m_db.m_mutex);
    }
}

DatabaseManager::ScopedTransaction::ScopedTransaction(DatabaseManager& db, int timeoutMs)
    : m_db(db)
    , m_committed(false)
    , m_active(false)
{
    m_active = m_db.beginImmediate(timeoutMs);
}

DatabaseManager::ScopedTransaction::~ScopedTransaction() {
    if (m_active && !m_committed) {
        m_db.rollback();
    }
}

bool DatabaseManager::ScopedTransaction::commit() {
    if (!m_active || m_committed) return false;
    m_committed = m_db.commit();
    return m_committed;
}

void DatabaseManager::ScopedTransaction::rollback() {
    if (m_active && !m_committed) {
        m_db.rollback();
        m_active = false;
    }
}

int DatabaseManager::getSchemaVersion() {
    MutexLocker lock(&m_mutex);
    if (!m_db) return -1;

    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT version FROM schema_version LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0; // Table probably doesn't exist yet
    }

    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

bool DatabaseManager::initializeSchema() {
    MutexLocker lock(&m_mutex);
    if (!m_db) return false;

    const char* createSchemaSql =
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "  version INTEGER PRIMARY KEY"
        ");"
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id TEXT PRIMARY KEY,"
        "  server_id TEXT,"
        "  content TEXT NOT NULL,"
        "  description TEXT DEFAULT '',"
        "  is_completed INTEGER NOT NULL DEFAULT 0,"
        "  priority INTEGER NOT NULL DEFAULT 1,"
        "  due_date TEXT DEFAULT NULL,"
        "  project_id TEXT NOT NULL DEFAULT 'inbox',"
        "  local_updated_at INTEGER NOT NULL,"
        "  sync_state TEXT NOT NULL DEFAULT 'synced'"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_tasks_project ON tasks(project_id);"
        "CREATE INDEX IF NOT EXISTS idx_tasks_sync_state ON tasks(sync_state);"
        "CREATE INDEX IF NOT EXISTS idx_tasks_completed ON tasks(is_completed);"
        ""
        "CREATE TABLE IF NOT EXISTS sync_commands ("
        "  uuid TEXT PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  temp_id TEXT DEFAULT NULL,"
        "  args_json TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  created_at INTEGER NOT NULL,"
        "  retry_count INTEGER NOT NULL DEFAULT 0,"
        "  last_error TEXT DEFAULT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_sync_commands_status ON sync_commands(status, created_at);"
        ""
        "CREATE TABLE IF NOT EXISTS dead_letter_commands ("
        "  uuid TEXT PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  args_json TEXT NOT NULL,"
        "  failed_at INTEGER NOT NULL,"
        "  error_reason TEXT NOT NULL"
        ");"
        ""
        "CREATE TABLE IF NOT EXISTS sync_metadata ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT"
        ");"
        ""
        "CREATE TABLE IF NOT EXISTS task_baselines ("
        "  id TEXT PRIMARY KEY,"
        "  content TEXT NOT NULL,"
        "  description TEXT DEFAULT '',"
        "  is_completed INTEGER NOT NULL DEFAULT 0,"
        "  priority INTEGER NOT NULL DEFAULT 1,"
        "  due_date TEXT DEFAULT NULL,"
        "  project_id TEXT NOT NULL DEFAULT 'inbox',"
        "  local_updated_at INTEGER NOT NULL DEFAULT 0"
        ");"
        ""
        "CREATE TABLE IF NOT EXISTS temp_id_mappings ("
        "  temp_id TEXT PRIMARY KEY,"
        "  server_id TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");";

    char* errMsg = NULL;
    int rc = sqlite3_exec(m_db, createSchemaSql, NULL, NULL, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }

    // Set initial schema version = 1 if not present
    sqlite3_stmt* checkStmt = NULL;
    sqlite3_prepare_v2(m_db, "SELECT count(*) FROM schema_version;", -1, &checkStmt, NULL);
    int count = 0;
    if (sqlite3_step(checkStmt) == SQLITE_ROW) {
        count = sqlite3_column_int(checkStmt, 0);
    }
    sqlite3_finalize(checkStmt);

    if (count == 0) {
        sqlite3_exec(m_db, "INSERT INTO schema_version (version) VALUES (1);", NULL, NULL, NULL);
    }

    return true;
}

} // namespace todoist
