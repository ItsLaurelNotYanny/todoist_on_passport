#include "todoist/network/sync_worker_thread.hpp"

#include <ctime>
#include <cstdlib>
#include <sys/time.h>

namespace todoist {

SyncWorkerThread::SyncWorkerThread(SyncEngine& syncEngine,
                                   ISyncTransport& transport,
                                   SyncNotificationQueue& notificationQueue,
                                   CurlSyncTransport* curlTransport)
    : m_syncEngine(syncEngine)
    , m_transport(transport)
    , m_queue(notificationQueue)
    , m_curlTransport(curlTransport)
    , m_cancelToken()
    , m_started(false)
    , m_stopping(false)
    , m_syncRequested(false)
    , m_reachable(true)
    , m_backoffWaitSeconds(0.0)
    , m_consecutiveFailures(0)
    , m_state(WORKER_STATE_IDLE)
{
    pthread_mutex_init(&m_stateMutex, NULL);
    pthread_cond_init(&m_stateCond, NULL);

    if (m_curlTransport) {
        m_curlTransport->setCancellationToken(&m_cancelToken);
    }
}

SyncWorkerThread::~SyncWorkerThread() {
    stop();
    if (m_curlTransport) {
        m_curlTransport->setCancellationToken(NULL);
    }
    pthread_mutex_destroy(&m_stateMutex);
    pthread_cond_destroy(&m_stateCond);
}

bool SyncWorkerThread::start() {
    pthread_mutex_lock(&m_stateMutex);
    if (m_started) {
        pthread_mutex_unlock(&m_stateMutex);
        return true;
    }
    m_cancelToken.reset();
    m_stopping = false;
    m_syncRequested = false;
    m_backoffWaitSeconds = 0.0;
    m_consecutiveFailures = 0;
    m_state = WORKER_STATE_IDLE;

    if (m_curlTransport) {
        m_curlTransport->setCancellationToken(&m_cancelToken);
    }

    int rc = pthread_create(&m_thread, NULL, threadEntry, this);
    if (rc == 0) {
        m_started = true;
    }
    pthread_mutex_unlock(&m_stateMutex);
    return rc == 0;
}

void SyncWorkerThread::stop() {
    pthread_mutex_lock(&m_stateMutex);
    if (!m_started || m_stopping) {
        pthread_mutex_unlock(&m_stateMutex);
        return;
    }
    m_stopping = true;
    m_cancelToken.cancel();
    pthread_cond_broadcast(&m_stateCond);
    pthread_mutex_unlock(&m_stateMutex);

    pthread_join(m_thread, NULL);

    pthread_mutex_lock(&m_stateMutex);
    m_started = false;
    m_state = WORKER_STATE_IDLE;
    if (m_curlTransport) {
        m_curlTransport->setCancellationToken(NULL);
    }
    pthread_mutex_unlock(&m_stateMutex);
}

void SyncWorkerThread::requestSync() {
    pthread_mutex_lock(&m_stateMutex);
    m_syncRequested = true;
    m_backoffWaitSeconds = 0.0;
    if (m_state == WORKER_STATE_FATAL_ERROR) {
        // Explicit user action clears latched fatal failure
        m_state = WORKER_STATE_IDLE;
    }
    pthread_cond_broadcast(&m_stateCond);
    pthread_mutex_unlock(&m_stateMutex);
}

void SyncWorkerThread::notifyReachabilityChanged(bool reachable) {
    pthread_mutex_lock(&m_stateMutex);
    m_reachable = reachable;
    if (reachable && m_state != WORKER_STATE_FATAL_ERROR) {
        if (m_syncEngine.getState() != SyncEngineState::AuthError) {
            // Network recovered: interrupt backoff wait immediately to retry
            m_backoffWaitSeconds = 0.0;
            m_syncRequested = true;
        }
    }
    // Always broadcast to wake waiting worker or interrupt backoff timed wait
    pthread_cond_broadcast(&m_stateCond);
    pthread_mutex_unlock(&m_stateMutex);
}

WorkerSyncState SyncWorkerThread::getState() const {
    pthread_mutex_lock(&m_stateMutex);
    WorkerSyncState s = m_state;
    pthread_mutex_unlock(&m_stateMutex);
    return s;
}

int SyncWorkerThread::getConsecutiveFailures() const {
    pthread_mutex_lock(&m_stateMutex);
    int f = m_consecutiveFailures;
    pthread_mutex_unlock(&m_stateMutex);
    return f;
}

double SyncWorkerThread::calculateBackoff(int attempt) const {
    const double BASE_BACKOFF = 1.0;
    const double MAX_BACKOFF = 60.0;
    const int MAX_EXPONENT = 6;

    int exp = (attempt > MAX_EXPONENT) ? MAX_EXPONENT : attempt;
    if (exp < 0) {
        exp = 0;
    }
    double max_window = BASE_BACKOFF * static_cast<double>(1 << exp);
    if (max_window > MAX_BACKOFF) {
        max_window = MAX_BACKOFF;
    }

    // Full jitter: uniform random in [0.0, max_window]
    double jitter = static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX);
    return jitter * max_window;
}

void* SyncWorkerThread::threadEntry(void* arg) {
    SyncWorkerThread* self = static_cast<SyncWorkerThread*>(arg);
    self->runWorkerLoop();
    return NULL;
}

void SyncWorkerThread::runWorkerLoop() {
    while (true) {
        pthread_mutex_lock(&m_stateMutex);
        if (m_stopping) {
            pthread_mutex_unlock(&m_stateMutex);
            break;
        }

        // 1. Wait until a sync is requested (while reachable) or backoff timer expires or stopping
        while (!m_stopping && (!m_reachable || (!m_syncRequested && m_backoffWaitSeconds <= 0.0))) {
            pthread_cond_wait(&m_stateCond, &m_stateMutex);
        }

        if (m_stopping) {
            pthread_mutex_unlock(&m_stateMutex);
            break;
        }

        // 2. If we are in backoff wait, perform timed wait
        if (m_backoffWaitSeconds > 0.0) {
            struct timeval now;
            gettimeofday(&now, NULL);

            struct timespec ts;
            long sec = static_cast<long>(m_backoffWaitSeconds);
            long nsec = static_cast<long>((m_backoffWaitSeconds - sec) * 1e9);
            ts.tv_sec = now.tv_sec + sec;
            ts.tv_nsec = (now.tv_usec * 1000L) + nsec;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }

            m_state = WORKER_STATE_BACKOFF_WAIT;
            pthread_mutex_unlock(&m_stateMutex);

            m_queue.push(SyncEventSnapshot(WORKER_STATE_BACKOFF_WAIT, 0,
                                           "Waiting for exponential backoff",
                                           static_cast<unsigned long>(time(NULL)), 0));

            pthread_mutex_lock(&m_stateMutex);
            int wait_rc = 0;
            while (!m_stopping && m_reachable && !m_syncRequested && wait_rc == 0) {
                wait_rc = pthread_cond_timedwait(&m_stateCond, &m_stateMutex, &ts);
            }
            m_backoffWaitSeconds = 0.0;

            if (m_stopping) {
                pthread_mutex_unlock(&m_stateMutex);
                break;
            }

            if (!m_reachable) {
                m_state = WORKER_STATE_IDLE;
                pthread_mutex_unlock(&m_stateMutex);
                continue;
            }
        }

        m_syncRequested = false;
        m_state = WORKER_STATE_SYNCING;
        pthread_mutex_unlock(&m_stateMutex);

        // Broadcast syncing event
        m_queue.push(SyncEventSnapshot(WORKER_STATE_SYNCING, 0, "Syncing",
                                       static_cast<unsigned long>(time(NULL)), 0));

        // 3. Execute sync cycles on ISyncTransport until eligible backlog is drained
        SyncCycleResult res;
        int totalItems = 0;
        std::string cycleBearer = m_syncEngine.getBearerToken();
        while (true) {
            res = m_syncEngine.executeSyncCycle(m_transport);
            if (!res.isSuccess()) {
                break;
            }
            totalItems += static_cast<int>(res.commandsAcknowledgedCount + res.tasksUpsertedCount + res.tasksDeletedCount);

            // If no commands were acknowledged or failed in this cycle, backlog is drained
            if (res.commandsAcknowledgedCount == 0 && res.commandsFailedCount == 0) {
                break;
            }

            pthread_mutex_lock(&m_stateMutex);
            if (m_stopping || !m_reachable) {
                pthread_mutex_unlock(&m_stateMutex);
                break;
            }
            pthread_mutex_unlock(&m_stateMutex);
        }

        pthread_mutex_lock(&m_stateMutex);
        if (m_stopping) {
            pthread_mutex_unlock(&m_stateMutex);
            break;
        }

        bool isFatal = false;
        std::string currentBearer = m_syncEngine.getBearerToken();
        bool tokenReplacedInFlight = (!currentBearer.empty() && currentBearer != cycleBearer);

        if (m_curlTransport && m_curlTransport->isLastFatalSecurityError()) {
            isFatal = true;
        } else if (res.status == SyncCycleStatus::AuthError ||
                   res.httpStatusCode == 400 || res.httpStatusCode == 401 || res.httpStatusCode == 403 ||
                   res.status == SyncCycleStatus::FatalError) {
            if (tokenReplacedInFlight && (res.status == SyncCycleStatus::AuthError || res.httpStatusCode == 401 || res.httpStatusCode == 403)) {
                isFatal = false;
                m_syncRequested = true;
            } else {
                isFatal = true;
            }
        } else if (res.errorMessage.find("SSL") != std::string::npos ||
                   res.errorMessage.find("certificate") != std::string::npos ||
                   res.errorMessage.find("CA cert") != std::string::npos ||
                   res.errorMessage.find("cert") != std::string::npos ||
                   res.errorMessage.find("https://") != std::string::npos) {
            isFatal = true;
        }

        if (tokenReplacedInFlight && !isFatal) {
            m_state = WORKER_STATE_IDLE;
            m_syncRequested = true;
            m_backoffWaitSeconds = 0.0;
            pthread_mutex_unlock(&m_stateMutex);
            continue;
        }

        if (res.status == SyncCycleStatus::Success) {
            m_consecutiveFailures = 0;
            m_state = WORKER_STATE_SUCCESS;
            pthread_mutex_unlock(&m_stateMutex);

            m_queue.push(SyncEventSnapshot(WORKER_STATE_SUCCESS, 200, "Sync successful",
                                           static_cast<unsigned long>(time(NULL)), totalItems));

            pthread_mutex_lock(&m_stateMutex);
            m_state = WORKER_STATE_IDLE;
            pthread_mutex_unlock(&m_stateMutex);

            m_queue.push(SyncEventSnapshot(WORKER_STATE_IDLE, 0, "Idle",
                                           static_cast<unsigned long>(time(NULL)), 0));
        } else if (isFatal) {
            // Terminal authentication / client configuration / fatal security errors:
            // Latch fatal state, clear pending wakeups, halt automatic retry loop without spinning!
            m_consecutiveFailures = 0;
            m_backoffWaitSeconds = 0.0;
            m_syncRequested = false;
            m_state = WORKER_STATE_FATAL_ERROR;
            pthread_mutex_unlock(&m_stateMutex);

            m_queue.push(SyncEventSnapshot(WORKER_STATE_FATAL_ERROR, res.httpStatusCode,
                                           res.errorMessage.empty() ? "Fatal error" : res.errorMessage,
                                           static_cast<unsigned long>(time(NULL)), 0));
        } else {
            // Transient error (RetryPending, 5xx, or network disconnection):
            // Continue retrying with saturating full jitter exponential backoff.
            m_consecutiveFailures++;
            m_backoffWaitSeconds = calculateBackoff(m_consecutiveFailures);
            m_state = WORKER_STATE_BACKOFF_WAIT;
            pthread_mutex_unlock(&m_stateMutex);
        }
    }
}

} // namespace todoist
