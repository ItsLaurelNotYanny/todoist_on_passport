#ifndef BB10_TLS_HPP
#define BB10_TLS_HPP

#include "bb10_tls.h"
#include <string>
#include <vector>

namespace bb10_tls {

class Context {
public:
    explicit Context(bb10_tls_policy_t policy)
        : m_ctx(bb10_tls_context_create(policy))
    {
    }

    ~Context() {
        if (m_ctx) {
            bb10_tls_context_destroy(m_ctx);
            m_ctx = NULL;
        }
    }

    bool isValid() const {
        return m_ctx != NULL;
    }

    bb10_tls_context_t* get() const {
        return m_ctx;
    }

    int setCaPath(const std::string& path) {
        if (!m_ctx) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_context_set_ca_path(m_ctx, path.c_str());
    }

    int setExpectedFingerprint(const std::string& fingerprintHex) {
        if (!m_ctx) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_context_set_expected_fingerprint(m_ctx, fingerprintHex.c_str());
    }

    int setCredentials(const std::string& certPem, const std::string& keyPem) {
        if (!m_ctx) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_context_set_credentials(
            m_ctx, certPem.data(), certPem.size(), keyPem.data(), keyPem.size());
    }

    int setMinTlsVersion(bb10_tls_version_t version) {
        if (!m_ctx) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_context_set_min_tls_version(m_ctx, version);
    }

    int setMaxTlsVersion(bb10_tls_version_t version) {
        if (!m_ctx) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_context_set_max_tls_version(m_ctx, version);
    }

private:
    Context(const Context&);
    Context& operator=(const Context&);

    bb10_tls_context_t* m_ctx;
};

class Connection {
public:
    Connection(Context& ctx, int socketFd)
        : m_conn(bb10_tls_connection_create(ctx.get(), socketFd))
    {
    }

    ~Connection() {
        if (m_conn) {
            bb10_tls_connection_destroy(m_conn);
            m_conn = NULL;
        }
    }

    bool isValid() const {
        return m_conn != NULL;
    }

    bb10_tls_connection_t* get() const {
        return m_conn;
    }

    int connect(const std::string& hostname) {
        if (!m_conn) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_connect(m_conn, hostname.c_str());
    }

    int accept() {
        if (!m_conn) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_accept(m_conn);
    }

    int read(void* buf, size_t count, size_t& bytesRead) {
        if (!m_conn) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_read(m_conn, buf, count, &bytesRead);
    }

    int write(const void* buf, size_t count, size_t& bytesWritten) {
        if (!m_conn) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_write(m_conn, buf, count, &bytesWritten);
    }

    int shutdown() {
        if (!m_conn) return BB10_TLS_ERR_INVALID_ARG;
        return bb10_tls_shutdown(m_conn);
    }

    int getFd() const {
        if (!m_conn) return -1;
        return bb10_tls_connection_get_fd(m_conn);
    }

    std::string getNegotiatedVersion() const {
        if (!m_conn) return "";
        char buf[32];
        if (bb10_tls_connection_get_negotiated_version(m_conn, buf, sizeof(buf)) == BB10_TLS_OK) {
            return std::string(buf);
        }
        return "";
    }

    std::string getCipher() const {
        if (!m_conn) return "";
        char buf[64];
        if (bb10_tls_connection_get_cipher(m_conn, buf, sizeof(buf)) == BB10_TLS_OK) {
            return std::string(buf);
        }
        return "";
    }

    std::string getPeerFingerprint() const {
        if (!m_conn) return "";
        char buf[65];
        if (bb10_tls_connection_get_peer_fingerprint(m_conn, buf) == BB10_TLS_OK) {
            return std::string(buf);
        }
        return "";
    }

    std::string getLastError() const {
        if (!m_conn) return "Invalid connection handle";
        const char* err = bb10_tls_get_last_error(m_conn);
        return err ? std::string(err) : std::string();
    }

private:
    Connection(const Connection&);
    Connection& operator=(const Connection&);

    bb10_tls_connection_t* m_conn;
};

inline int randomBytes(unsigned char* buf, size_t len) {
    return bb10_tls_random_bytes(buf, len);
}

inline int sha256(const unsigned char* data, size_t len, unsigned char* outHash32) {
    return bb10_tls_sha256(data, len, outHash32);
}

inline int sha256File(const std::string& filepath, std::string& outHex) {
    char hex[65];
    int rc = bb10_tls_sha256_file(filepath.c_str(), hex);
    if (rc == BB10_TLS_OK) {
        outHex = std::string(hex);
    }
    return rc;
}

inline int calculateCertFingerprint(const std::string& certPem, std::string& outFingerprint) {
    char buf[65];
    int rc = bb10_tls_calculate_cert_fingerprint(certPem.c_str(), certPem.size(), buf);
    if (rc == BB10_TLS_OK) {
        outFingerprint = buf;
    }
    return rc;
}

inline int generateSelfSignedCert(std::string& outCertPem, std::string& outKeyPem) {
    char* cert = NULL;
    size_t certLen = 0;
    char* key = NULL;
    size_t keyLen = 0;
    int rc = bb10_tls_generate_self_signed_cert(&cert, &certLen, &key, &keyLen);
    if (rc == BB10_TLS_OK) {
        if (cert) {
            outCertPem.assign(cert, certLen);
            bb10_tls_free_string(cert);
        }
        if (key) {
            outKeyPem.assign(key, keyLen);
            bb10_tls_free_string(key);
        }
    }
    return rc;
}

} // namespace bb10_tls

#endif /* BB10_TLS_HPP */
