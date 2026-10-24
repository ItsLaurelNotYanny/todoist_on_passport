#ifndef TODOIST_SYNC_NOTIFICATION_QUEUE_HPP
#define TODOIST_SYNC_NOTIFICATION_QUEUE_HPP

#include <string>
#include <vector>
#include <pthread.h>

namespace todoist {

enum WorkerSyncState {
    WORKER_STATE_IDLE = 0,
    WORKER_STATE_SYNCING = 1,
    WORKER_STATE_SUCCESS = 2,
    WORKER_STATE_BACKOFF_WAIT = 3,
    WORKER_STATE_FATAL_ERROR = 4
};

struct SyncEventSnapshot {
    WorkerSyncState state;
    int errorCode;
    std::string errorMessage;
    unsigned long timestamp;
    int itemsProcessed;

    SyncEventSnapshot()
        : state(WORKER_STATE_IDLE)
        , errorCode(0)
        , errorMessage("")
        , timestamp(0)
        , itemsProcessed(0)
    {}

    SyncEventSnapshot(WorkerSyncState s, int err, const std::string& msg, unsigned long ts, int items)
        : state(s)
        , errorCode(err)
        , errorMessage(msg)
        , timestamp(ts)
        , itemsProcessed(items)
    {}
};

class SyncNotificationQueue {
public:
    SyncNotificationQueue() {
        pthread_mutex_init(&m_mutex, NULL);
    }

    ~SyncNotificationQueue() {
        pthread_mutex_destroy(&m_mutex);
    }

    void push(const SyncEventSnapshot& snapshot) {
        pthread_mutex_lock(&m_mutex);
        m_events.push_back(snapshot);
        pthread_mutex_unlock(&m_mutex);
    }

    void drainNotifications(std::vector<SyncEventSnapshot>& outEvents) {
        pthread_mutex_lock(&m_mutex);
        outEvents.swap(m_events);
        m_events.clear();
        pthread_mutex_unlock(&m_mutex);
    }

    size_t size() const {
        pthread_mutex_lock(&m_mutex);
        size_t count = m_events.size();
        pthread_mutex_unlock(&m_mutex);
        return count;
    }

    bool empty() const {
        return size() == 0;
    }

private:
    SyncNotificationQueue(const SyncNotificationQueue&);
    SyncNotificationQueue& operator=(const SyncNotificationQueue&);

    mutable pthread_mutex_t m_mutex;
    std::vector<SyncEventSnapshot> m_events;
};

} // namespace todoist

#endif // TODOIST_SYNC_NOTIFICATION_QUEUE_HPP
