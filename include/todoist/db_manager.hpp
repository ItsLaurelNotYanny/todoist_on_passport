#ifndef TODOIST_DB_MANAGER_HPP
#define TODOIST_DB_MANAGER_HPP

#include <string>
#include <pthread.h>
#include "sqlite3.h"

namespace todoist {

class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    // Open SQLite database file or ":memory:"
    bool open(const std::string& dbPath);
    void close();
    bool isOpen() const;

    sqlite3* handle() const { return m_db; }

    // Execute raw SQL statement without result
    bool execute(const std::string& sql, std::string* errorOut = NULL);

    // Transaction management
    bool beginImmediate(int timeoutMs = -1);
    bool commit();
    bool rollback();

    // Checks if another thread currently holds an active write transaction
    bool hasForeignTransaction() const;

    // Scoped non-blocking snapshot lock for coherent reads
    class ScopedSnapshotLock {
    public:
        explicit ScopedSnapshotLock(DatabaseManager& db);
        ~ScopedSnapshotLock();
        bool isLocked() const { return m_locked; }
    private:
        DatabaseManager& m_db;
        bool m_locked;
        ScopedSnapshotLock(const ScopedSnapshotLock&);
        ScopedSnapshotLock& operator=(const ScopedSnapshotLock&);
    };

    // RAII Transaction Guard
    class ScopedTransaction {
    public:
        explicit ScopedTransaction(DatabaseManager& db, int timeoutMs = -1);
        ~ScopedTransaction();

        bool commit();
        void rollback();
        bool isCommitted() const { return m_committed; }
        bool isActive() const { return m_active; }

    private:
        DatabaseManager& m_db;
        bool m_committed;
        bool m_active;

        // Disable copy (C++03)
        ScopedTransaction(const ScopedTransaction&);
        ScopedTransaction& operator=(const ScopedTransaction&);
    };

    // Run schema migrations to latest version
    bool initializeSchema();

    // Get current schema version
    int getSchemaVersion();

private:
    sqlite3* m_db;
    pthread_mutex_t m_mutex;
    pthread_cond_t m_txCond;
    pthread_t m_transactionOwner;
    bool m_hasTransactionOwner;
    int m_transactionDepth;

    bool beginImmediateLocked(int timeoutMs = -1);
    bool commitLocked();
    bool rollbackLocked();
    void closeLocked();

    // Disable copy (C++03)
    DatabaseManager(const DatabaseManager&);
    DatabaseManager& operator=(const DatabaseManager&);

    // RAII POSIX Mutex Lock Helper
    class MutexLocker {
    public:
        explicit MutexLocker(pthread_mutex_t* m, bool alreadyLocked = false)
            : m_mutex(m), m_locked(true) {
            if (!alreadyLocked) {
                pthread_mutex_lock(m_mutex);
            }
        }
        ~MutexLocker() {
            if (m_locked && m_mutex) {
                pthread_mutex_unlock(m_mutex);
            }
        }
    private:
        pthread_mutex_t* m_mutex;
        bool m_locked;
        MutexLocker(const MutexLocker&);
        MutexLocker& operator=(const MutexLocker&);
    };
};

} // namespace todoist

#endif // TODOIST_DB_MANAGER_HPP
