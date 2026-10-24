#ifndef TODOIST_SYNC_WORKER_THREAD_HPP
#define TODOIST_SYNC_WORKER_THREAD_HPP

#include "todoist/sync/sync_engine.hpp"
#include "todoist/sync/sync_transport_interface.hpp"
#include "todoist/network/sync_notification_queue.hpp"
#include "todoist/network/curl_sync_transport.hpp"
#include "todoist/network/cancellation_token.hpp"
#include <pthread.h>
#include <string>

namespace todoist {

class SyncWorkerThread {
public:
    SyncWorkerThread(SyncEngine& syncEngine,
                     ISyncTransport& transport,
                     SyncNotificationQueue& notificationQueue,
                     CurlSyncTransport* curlTransport = NULL);
    ~SyncWorkerThread();

    // Starts the joinable worker thread
    bool start();

    // Requests graceful cancellation, aborts active transport transfer, and joins thread
    void stop();

    // Triggers an immediate sync cycle (interrupting idle or backoff wait)
    void requestSync();

    // Notifies network reachability change (interrupts backoff immediately if reachable=true)
    void notifyReachabilityChanged(bool reachable);

    WorkerSyncState getState() const;
    int getConsecutiveFailures() const;

    // Saturating full jitter backoff calculation: min(60.0, 1.0 * (1 << min(attempt, 6)))
    double calculateBackoff(int attempt) const;

private:
    SyncWorkerThread(const SyncWorkerThread&);
    SyncWorkerThread& operator=(const SyncWorkerThread&);

    static void* threadEntry(void* arg);
    void runWorkerLoop();

    SyncEngine& m_syncEngine;
    ISyncTransport& m_transport;
    SyncNotificationQueue& m_queue;
    CurlSyncTransport* m_curlTransport;
    CancellationToken m_cancelToken;

    pthread_t m_thread;
    bool m_started;
    volatile bool m_stopping;
    bool m_syncRequested;
    bool m_reachable;
    double m_backoffWaitSeconds;
    int m_consecutiveFailures;
    WorkerSyncState m_state;

    mutable pthread_mutex_t m_stateMutex;
    pthread_cond_t m_stateCond;
};

} // namespace todoist

#endif // TODOIST_SYNC_WORKER_THREAD_HPP
