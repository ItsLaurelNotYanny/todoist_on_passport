#ifndef TODOIST_CANCELLATION_TOKEN_HPP
#define TODOIST_CANCELLATION_TOKEN_HPP

#include <pthread.h>

namespace todoist {

/**
 * Thread-safe cancellation token for POSIX/C++03.
 * Protects both cancellation requests and queries under a private mutex.
 */
class CancellationToken {
public:
    CancellationToken()
        : m_cancelled(false)
    {
        pthread_mutex_init(&m_mutex, NULL);
    }

    ~CancellationToken() {
        pthread_mutex_destroy(&m_mutex);
    }

    void cancel() {
        pthread_mutex_lock(&m_mutex);
        m_cancelled = true;
        pthread_mutex_unlock(&m_mutex);
    }

    void reset() {
        pthread_mutex_lock(&m_mutex);
        m_cancelled = false;
        pthread_mutex_unlock(&m_mutex);
    }

    bool isCancelled() const {
        pthread_mutex_lock(&m_mutex);
        bool cancelled = m_cancelled;
        pthread_mutex_unlock(&m_mutex);
        return cancelled;
    }

private:
    CancellationToken(const CancellationToken&);
    CancellationToken& operator=(const CancellationToken&);

    mutable pthread_mutex_t m_mutex;
    bool m_cancelled;
};

} // namespace todoist

#endif // TODOIST_CANCELLATION_TOKEN_HPP
