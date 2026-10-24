#include "todoist/network/curl_sync_transport.hpp"
#include <curl/curl.h>
#include <cstring>

namespace todoist {

struct WriteContext {
    std::string* buffer;
    size_t max_bytes;
    bool overflow;
};

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_bytes = size * nmemb;
    WriteContext* ctx = static_cast<WriteContext*>(userp);
    if (!ctx || !ctx->buffer) {
        return 0;
    }
    if (ctx->buffer->size() + total_bytes > ctx->max_bytes) {
        ctx->overflow = true;
        return 0; // Returning 0 causes CURLE_WRITE_ERROR to abort transfer
    }
    ctx->buffer->append(static_cast<const char*>(contents), total_bytes);
    return total_bytes;
}

static int xferinfo_callback(void* clientp,
                             curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    CurlSyncTransport* transport = static_cast<CurlSyncTransport*>(clientp);
    if (transport && transport->isCancelled()) {
        return 1; // Non-zero aborts transfer with CURLE_ABORTED_BY_CALLBACK
    }
    return 0;
}

CurlSyncTransport::CurlSyncTransport(const std::string& caBundlePath)
    : m_caBundlePath(caBundlePath)
    , m_cancellationToken(NULL)
    , m_lastFatalSecurityError(false)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlSyncTransport::~CurlSyncTransport() {
}

void CurlSyncTransport::setCancellationToken(const CancellationToken* token) {
    m_cancellationToken = token;
}

bool CurlSyncTransport::isCancelled() const {
    if (m_cancellationToken && m_cancellationToken->isCancelled()) {
        return true;
    }
    return false;
}

bool CurlSyncTransport::isLastFatalSecurityError() const {
    return m_lastFatalSecurityError;
}

ISyncTransport::HttpResponse CurlSyncTransport::executeSyncPost(
    const std::string& endpointUrl,
    const std::string& requestBody,
    const std::string& bearerToken)
{
    m_lastFatalSecurityError = false;

    if (isCancelled()) {
        return HttpResponse(0, "", "Operation was aborted by an application callback");
    }

    if (endpointUrl.empty()) {
        m_lastFatalSecurityError = true;
        return HttpResponse(0, "", "Endpoint URL is empty");
    }

    if (endpointUrl.compare(0, 8, "https://") != 0) {
        m_lastFatalSecurityError = true;
        return HttpResponse(0, "", "Endpoint URL must use https:// scheme");
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return HttpResponse(0, "", "Failed to initialize curl handle");
    }

    std::string responseBody;
    WriteContext wctx;
    wctx.buffer = &responseBody;
    wctx.max_bytes = MAX_RESPONSE_BYTES;
    wctx.overflow = false;

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    if (!bearerToken.empty()) {
        std::string authHeader = "Authorization: Bearer " + bearerToken;
        headers = curl_slist_append(headers, authHeader.c_str());
    }
    // Suppress Expect: 100-continue header for synchronous RPC
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, endpointUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.data());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Timeout enforcement
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TOTAL_TIMEOUT_MS);

    // Credential and redirect safety: refuse plaintext and automated cross-origin redirection
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    // Strict protocol & TLS baseline
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    // Public PKI validation
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!m_caBundlePath.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, m_caBundlePath.c_str());
    }

    // Response buffer
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    // Cancellation hook
    if (m_cancellationToken) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (wctx.overflow) {
        return HttpResponse(0, "", "Response exceeded maximum allowed buffer size of 10 MiB");
    }

    if (res != CURLE_OK) {
        if (res == CURLE_SSL_CACERT ||
            res == CURLE_SSL_CACERT_BADFILE ||
            res == CURLE_PEER_FAILED_VERIFICATION ||
            res == CURLE_SSL_CERTPROBLEM ||
            res == CURLE_SSL_CIPHER ||
            res == CURLE_SSL_CONNECT_ERROR ||
            res == CURLE_SSL_ENGINE_NOTFOUND ||
            res == CURLE_SSL_ENGINE_SETFAILED ||
            res == CURLE_SSL_SHUTDOWN_FAILED ||
            res == CURLE_SSL_CRL_BADFILE ||
            res == CURLE_SSL_ISSUER_ERROR ||
            res == CURLE_SSL_PINNEDPUBKEYNOTMATCH ||
            res == CURLE_SSL_INVALIDCERTSTATUS ||
            res == CURLE_UNSUPPORTED_PROTOCOL) {
            m_lastFatalSecurityError = true;
        }
        return HttpResponse(0, "", curl_easy_strerror(res));
    }

    return HttpResponse(static_cast<int>(response_code), responseBody, "");
}

} // namespace todoist
