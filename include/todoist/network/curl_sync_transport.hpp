#ifndef TODOIST_CURL_SYNC_TRANSPORT_HPP
#define TODOIST_CURL_SYNC_TRANSPORT_HPP

#include "todoist/sync/sync_transport_interface.hpp"
#include "todoist/network/cancellation_token.hpp"
#include <string>

namespace todoist {

class CurlSyncTransport : public ISyncTransport {
public:
    explicit CurlSyncTransport(const std::string& caBundlePath);
    virtual ~CurlSyncTransport();

    virtual HttpResponse executeSyncPost(const std::string& endpointUrl,
                                        const std::string& requestBody,
                                        const std::string& bearerToken);

    // Synchronized cancellation support via CancellationToken object
    void setCancellationToken(const CancellationToken* token);

    // Returns true if cancellation has been requested
    bool isCancelled() const;

    // Returns true if the last operation failed due to a fatal TLS/security/scheme error
    bool isLastFatalSecurityError() const;

    static const size_t MAX_RESPONSE_BYTES = 10 * 1024 * 1024; // 10 MiB ceiling
    static const long CONNECT_TIMEOUT_MS = 15000L;             // 15 seconds
    static const long TOTAL_TIMEOUT_MS = 30000L;               // 30 seconds

private:
    CurlSyncTransport(const CurlSyncTransport&);
    CurlSyncTransport& operator=(const CurlSyncTransport&);

    std::string m_caBundlePath;
    const CancellationToken* m_cancellationToken;
    bool m_lastFatalSecurityError;
};

} // namespace todoist

#endif // TODOIST_CURL_SYNC_TRANSPORT_HPP
