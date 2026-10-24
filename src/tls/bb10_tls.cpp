#include "bb10_tls/bb10_tls.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

enum bb10_tls_conn_state {
    BB10_TLS_STATE_INIT = 0,
    BB10_TLS_STATE_HANDSHAKE = 1,
    BB10_TLS_STATE_AUTHENTICATED = 2,
    BB10_TLS_STATE_FAILED = 3,
    BB10_TLS_STATE_CLOSED = 4
};

struct bb10_tls_context {
    bb10_tls_policy_t policy;
    SSL_CTX* ssl_ctx;
    std::string ca_path;
    std::string expected_fingerprint; // Normalized 64 lowercase hex characters
    bb10_tls_version_t min_version;
    bb10_tls_version_t max_version;
    bool has_credentials;
    std::string last_error;
};

struct bb10_tls_connection {
    bb10_tls_context_t* ctx;
    SSL* ssl;
    int fd; // Sentinel is -1; valid descriptors are >= 0
    bb10_tls_conn_state state;
    std::string last_error;
    char negotiated_version[32];
    char negotiated_cipher[64];
    char peer_fingerprint[65];
    bool peer_fingerprint_valid;
};

static int bb10_bio_write(BIO* b, const char* in, int inl) {
    if (!b || !in || inl <= 0) return 0;
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b)));
    if (fd < 0) return 0;
    BIO_clear_retry_flags(b);
#if defined(MSG_NOSIGNAL)
    ssize_t ret = send(fd, in, static_cast<size_t>(inl), MSG_NOSIGNAL);
#else
    ssize_t ret = write(fd, in, static_cast<size_t>(inl));
#endif
    if (ret <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            BIO_set_retry_write(b);
        }
    }
    return static_cast<int>(ret);
}

static int bb10_bio_read(BIO* b, char* out, int outl) {
    if (!b || !out || outl <= 0) return 0;
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b)));
    if (fd < 0) return 0;
    BIO_clear_retry_flags(b);
    ssize_t ret = read(fd, out, static_cast<size_t>(outl));
    if (ret <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            BIO_set_retry_read(b);
        }
    }
    return static_cast<int>(ret);
}

static int bb10_bio_puts(BIO* b, const char* str) {
    if (!str) return 0;
    return bb10_bio_write(b, str, static_cast<int>(strlen(str)));
}

static long bb10_bio_ctrl(BIO* b, int cmd, long num, void* ptr) {
    long ret = 1;
    switch (cmd) {
    case BIO_C_SET_FD: {
        int fd = *static_cast<int*>(ptr);
        BIO_set_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(fd)));
        BIO_set_init(b, 1);
        break;
    }
    case BIO_C_GET_FD: {
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b)));
        if (ptr) *static_cast<int*>(ptr) = fd;
        ret = fd;
        break;
    }
    case BIO_CTRL_GET_CLOSE:
        ret = BIO_get_shutdown(b);
        break;
    case BIO_CTRL_SET_CLOSE:
        BIO_set_shutdown(b, static_cast<int>(num));
        break;
    case BIO_CTRL_FLUSH:
    case BIO_CTRL_DUP:
        ret = 1;
        break;
    default:
        ret = 0;
        break;
    }
    return ret;
}

static pthread_once_t s_bio_method_once = PTHREAD_ONCE_INIT;
static BIO_METHOD* s_bio_method = NULL;

static void init_bb10_tls_bio_method_once() {
    BIO_METHOD* meth = BIO_meth_new(BIO_TYPE_SOCKET, "bb10_tls_nosigpipe_socket");
    if (meth) {
        BIO_meth_set_write(meth, bb10_bio_write);
        BIO_meth_set_read(meth, bb10_bio_read);
        BIO_meth_set_puts(meth, bb10_bio_puts);
        BIO_meth_set_ctrl(meth, bb10_bio_ctrl);
        s_bio_method = meth;
    }
}

static const BIO_METHOD* get_bb10_tls_bio_method() {
    pthread_once(&s_bio_method_once, init_bb10_tls_bio_method_once);
    return s_bio_method;
}

static bool normalize_hex_pin(const char* in, std::string& out_hex) {
    if (!in) {
        return false;
    }
    std::string clean;
    clean.reserve(64);
    for (const char* p = in; *p != '\0'; ++p) {
        char c = *p;
        if (c == ':' || c == ' ' || c == '-') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            clean += c;
        } else if (c >= 'a' && c <= 'f') {
            clean += c;
        } else if (c >= 'A' && c <= 'F') {
            clean += static_cast<char>(c - 'A' + 'a');
        } else {
            return false;
        }
    }
    if (clean.length() != 64) {
        return false;
    }
    out_hex = clean;
    return true;
}

bb10_tls_context_t* bb10_tls_context_create(bb10_tls_policy_t policy) {
    if (policy != BB10_TLS_POLICY_PUBLIC_PKI && policy != BB10_TLS_POLICY_P2P) {
        return NULL;
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    const SSL_METHOD* method = TLS_method();
    if (!method) {
        return NULL;
    }

    SSL_CTX* ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        return NULL;
    }

    // Default to TLS 1.2 minimum up to TLS 1.3 maximum
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);

    // Disable insecure legacy protocols and options
    long options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
    options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
#ifdef SSL_OP_NO_ANTI_REPLAY
    options |= SSL_OP_NO_ANTI_REPLAY;
#endif
    SSL_CTX_set_options(ssl_ctx, options);

    // Modern TLS 1.3 Ciphersuites and TLS 1.2 cipher list
    SSL_CTX_set_ciphersuites(ssl_ctx,
        "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256");
    SSL_CTX_set_cipher_list(ssl_ctx,
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");

    bb10_tls_context_t* ctx = new bb10_tls_context();
    ctx->policy = policy;
    ctx->ssl_ctx = ssl_ctx;
    ctx->min_version = BB10_TLS_VERSION_1_2;
    ctx->max_version = BB10_TLS_VERSION_1_3;
    ctx->has_credentials = false;

    if (policy == BB10_TLS_POLICY_PUBLIC_PKI) {
        // Enforce peer verification by default for public PKI
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
    } else {
        // P2P mode: verification is handled via raw SHA-256 fingerprint comparison
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    }

    return ctx;
}

void bb10_tls_context_destroy(bb10_tls_context_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
    }
    delete ctx;
}

int bb10_tls_context_set_ca_path(bb10_tls_context_t* ctx, const char* ca_bundle_path) {
    if (!ctx || !ctx->ssl_ctx || !ca_bundle_path) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    // Fail-closed verification: check if file is readable and load locations
    if (access(ca_bundle_path, R_OK) != 0) {
        ctx->last_error = "CA bundle file does not exist or is not readable";
        return BB10_TLS_ERR_CA_LOAD_FAILED;
    }

    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_bundle_path, NULL) <= 0) {
        ctx->last_error = "SSL_CTX_load_verify_locations failed";
        return BB10_TLS_ERR_CA_LOAD_FAILED;
    }

    ctx->ca_path = ca_bundle_path;
    return BB10_TLS_OK;
}

int bb10_tls_context_set_expected_fingerprint(bb10_tls_context_t* ctx, const char* sha256_hex) {
    if (!ctx || !sha256_hex) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    std::string normalized;
    if (!normalize_hex_pin(sha256_hex, normalized)) {
        ctx->last_error = "Invalid fingerprint pin format; must be 64 hexadecimal characters";
        return BB10_TLS_ERR_INVALID_PIN;
    }

    ctx->expected_fingerprint = normalized;
    return BB10_TLS_OK;
}

int bb10_tls_context_set_credentials(bb10_tls_context_t* ctx,
                                     const char* cert_pem,
                                     size_t cert_len,
                                     const char* key_pem,
                                     size_t key_len) {
    if (!ctx || !ctx->ssl_ctx || !cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    // Load Certificate
    BIO* cert_bio = BIO_new_mem_buf(cert_pem, static_cast<int>(cert_len));
    if (!cert_bio) {
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }
    X509* cert = PEM_read_bio_X509(cert_bio, NULL, NULL, NULL);
    BIO_free(cert_bio);
    if (!cert) {
        ctx->last_error = "Failed to parse certificate PEM";
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (SSL_CTX_use_certificate(ctx->ssl_ctx, cert) <= 0) {
        X509_free(cert);
        ctx->last_error = "SSL_CTX_use_certificate failed";
        return BB10_TLS_ERR_INTERNAL;
    }
    X509_free(cert);

    // Load Private Key
    BIO* key_bio = BIO_new_mem_buf(key_pem, static_cast<int>(key_len));
    if (!key_bio) {
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    BIO_free(key_bio);
    if (!pkey) {
        ctx->last_error = "Failed to parse private key PEM";
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (SSL_CTX_use_PrivateKey(ctx->ssl_ctx, pkey) <= 0) {
        EVP_PKEY_free(pkey);
        ctx->last_error = "SSL_CTX_use_PrivateKey failed";
        return BB10_TLS_ERR_INTERNAL;
    }
    EVP_PKEY_free(pkey);

    if (SSL_CTX_check_private_key(ctx->ssl_ctx) <= 0) {
        ctx->last_error = "Private key does not match certificate";
        return BB10_TLS_ERR_INTERNAL;
    }

    ctx->has_credentials = true;
    return BB10_TLS_OK;
}

int bb10_tls_context_set_min_tls_version(bb10_tls_context_t* ctx, bb10_tls_version_t version) {
    if (!ctx || !ctx->ssl_ctx) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    int proto_ver = 0;
    if (version == BB10_TLS_VERSION_1_2) {
        proto_ver = TLS1_2_VERSION;
    } else if (version == BB10_TLS_VERSION_1_3) {
        proto_ver = TLS1_3_VERSION;
    } else if (version == BB10_TLS_VERSION_DEFAULT) {
        proto_ver = TLS1_2_VERSION;
    } else {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (SSL_CTX_set_min_proto_version(ctx->ssl_ctx, proto_ver) <= 0) {
        return BB10_TLS_ERR_INTERNAL;
    }
    ctx->min_version = version;
    return BB10_TLS_OK;
}

int bb10_tls_context_set_max_tls_version(bb10_tls_context_t* ctx, bb10_tls_version_t version) {
    if (!ctx || !ctx->ssl_ctx) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    int proto_ver = 0;
    if (version == BB10_TLS_VERSION_1_2) {
        proto_ver = TLS1_2_VERSION;
    } else if (version == BB10_TLS_VERSION_1_3) {
        proto_ver = TLS1_3_VERSION;
    } else if (version == BB10_TLS_VERSION_DEFAULT) {
        proto_ver = TLS1_3_VERSION;
    } else {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (SSL_CTX_set_max_proto_version(ctx->ssl_ctx, proto_ver) <= 0) {
        return BB10_TLS_ERR_INTERNAL;
    }
    ctx->max_version = version;
    return BB10_TLS_OK;
}

bb10_tls_connection_t* bb10_tls_connection_create(bb10_tls_context_t* ctx, int socket_fd) {
    if (!ctx || !ctx->ssl_ctx || socket_fd < 0) {
        return NULL;
    }

#if defined(SO_NOSIGPIPE)
    int opt = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    SSL* ssl = SSL_new(ctx->ssl_ctx);
    if (!ssl) {
        return NULL;
    }

    const BIO_METHOD* bm = get_bb10_tls_bio_method();
    BIO* bio = bm ? BIO_new(bm) : NULL;
    if (!bio) {
        SSL_free(ssl);
        return NULL;
    }
    BIO_set_fd(bio, socket_fd, BIO_NOCLOSE);
    SSL_set_bio(ssl, bio, bio);

    bb10_tls_connection_t* conn = new bb10_tls_connection();
    conn->ctx = ctx;
    conn->ssl = ssl;
    conn->fd = socket_fd;
    conn->state = BB10_TLS_STATE_INIT;
    conn->negotiated_version[0] = '\0';
    conn->negotiated_cipher[0] = '\0';
    conn->peer_fingerprint[0] = '\0';
    conn->peer_fingerprint_valid = false;

    return conn;
}

void bb10_tls_connection_destroy(bb10_tls_connection_t* conn) {
    if (!conn) {
        return;
    }
    if (conn->ssl) {
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    // Sole socket ownership teardown: close native descriptor once and invalidate to -1
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    delete conn;
}

int bb10_tls_connect(bb10_tls_connection_t* conn, const char* hostname) {
    if (!conn || !conn->ssl || !conn->ctx) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (conn->state == BB10_TLS_STATE_FAILED) {
        return BB10_TLS_ERR_PIN_MISMATCH;
    }

    if (conn->ctx->policy == BB10_TLS_POLICY_PUBLIC_PKI) {
        if (!hostname || hostname[0] == '\0') {
            return BB10_TLS_ERR_INVALID_ARG;
        }

        // Distinguish DNS hostname from IPv4 / IPv6 literals (M3-R007)
        struct in_addr in4;
        struct in6_addr in6;
        bool is_ip = (inet_pton(AF_INET, hostname, &in4) == 1) ||
                     (inet_pton(AF_INET6, hostname, &in6) == 1);

        if (!is_ip) {
            // Set SNI extension (RFC 6066: literal IP addresses must not be sent in SNI)
            if (SSL_set_tlsext_host_name(conn->ssl, hostname) <= 0) {
                conn->last_error = "Failed to set TLS SNI extension";
                return BB10_TLS_ERR_INTERNAL;
            }
        }

        // Configure hostname verification with strict SAN precedence (RFC 6125)
        X509_VERIFY_PARAM* param = SSL_get0_param(conn->ssl);
        if (!param) {
            conn->last_error = "Failed to get SSL verify parameters";
            return BB10_TLS_ERR_INTERNAL;
        }

        if (is_ip) {
            if (X509_VERIFY_PARAM_set1_ip_asc(param, hostname) != 1) {
                conn->last_error = "Failed to set verification IP parameter";
                return BB10_TLS_ERR_INTERNAL;
            }
        } else {
            X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (X509_VERIFY_PARAM_set1_host(param, hostname, 0) != 1) {
                conn->last_error = "Failed to set verification hostname parameter";
                return BB10_TLS_ERR_INTERNAL;
            }
        }
        SSL_set_verify(conn->ssl, SSL_VERIFY_PEER, NULL);
    } else {
        // P2P Policy: verifying client requires expected fingerprint configured beforehand
        if (conn->ctx->expected_fingerprint.empty()) {
            conn->last_error = "Expected fingerprint not configured in P2P verifying client";
            conn->state = BB10_TLS_STATE_FAILED;
            return BB10_TLS_ERR_INVALID_PIN;
        }
        // In P2P mode, peer presents self-signed certificate, so skip PKI CA validation
        SSL_set_verify(conn->ssl, SSL_VERIFY_NONE, NULL);
    }

    conn->state = BB10_TLS_STATE_HANDSHAKE;
    int rc = SSL_connect(conn->ssl);
    if (rc <= 0) {
        int ssl_err = SSL_get_error(conn->ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ) {
            return BB10_TLS_ERR_WANT_READ;
        }
        if (ssl_err == SSL_ERROR_WANT_WRITE) {
            return BB10_TLS_ERR_WANT_WRITE;
        }
        conn->state = BB10_TLS_STATE_FAILED;
        if (ssl_err == SSL_ERROR_ZERO_RETURN) {
            return BB10_TLS_ERR_CLOSED;
        }

        long verify_err = SSL_get_verify_result(conn->ssl);
        if (verify_err != X509_V_OK) {
            conn->last_error = X509_verify_cert_error_string(verify_err);
            return BB10_TLS_ERR_CERT_VERIFY;
        }

        char err_buf[256];
        unsigned long e = ERR_get_error();
        if (e != 0) {
            ERR_error_string_n(e, err_buf, sizeof(err_buf));
            conn->last_error = err_buf;
        } else {
            conn->last_error = "TLS handshake negotiation failed";
        }
        return BB10_TLS_ERR_HANDSHAKE_FAILED;
    }

    // Handshake succeeded at TLS protocol level. Extract peer certificate.
    X509* peer_cert = SSL_get1_peer_certificate(conn->ssl);
    if (!peer_cert) {
        conn->state = BB10_TLS_STATE_FAILED;
        conn->last_error = "Peer did not present an X.509 certificate";
        return BB10_TLS_ERR_NO_PEER_CERT;
    }

    // Compute SHA-256 fingerprint of peer certificate DER encoding
    unsigned char* der = NULL;
    int der_len = i2d_X509(peer_cert, &der);
    if (der_len <= 0 || !der) {
        X509_free(peer_cert);
        conn->state = BB10_TLS_STATE_FAILED;
        conn->last_error = "Failed to encode peer certificate to DER";
        return BB10_TLS_ERR_INTERNAL;
    }

    unsigned char hash[32];
    int hash_rc = bb10_tls_sha256(der, static_cast<size_t>(der_len), hash);
    OPENSSL_free(der);
    if (hash_rc != BB10_TLS_OK) {
        X509_free(peer_cert);
        conn->state = BB10_TLS_STATE_FAILED;
        conn->last_error = "Failed to compute peer certificate SHA-256 fingerprint";
        return BB10_TLS_ERR_INTERNAL;
    }

    for (int i = 0; i < 32; ++i) {
        sprintf(conn->peer_fingerprint + (i * 2), "%02x", hash[i]);
    }
    conn->peer_fingerprint[64] = '\0';
    conn->peer_fingerprint_valid = true;

    if (conn->ctx->policy == BB10_TLS_POLICY_P2P) {
        // P2P Policy: Strict SHA-256 fingerprint pin match
        if (conn->ctx->expected_fingerprint != conn->peer_fingerprint) {
            X509_free(peer_cert);
            conn->state = BB10_TLS_STATE_FAILED;
            conn->last_error = "Peer certificate fingerprint mismatch; expected " +
                               conn->ctx->expected_fingerprint + ", got " + conn->peer_fingerprint;
            return BB10_TLS_ERR_PIN_MISMATCH;
        }
    } else {
        // Public PKI Policy: Check X.509 verification result
        long verify_err = SSL_get_verify_result(conn->ssl);
        if (verify_err != X509_V_OK) {
            X509_free(peer_cert);
            conn->state = BB10_TLS_STATE_FAILED;
            conn->last_error = X509_verify_cert_error_string(verify_err);
            return BB10_TLS_ERR_CERT_VERIFY;
        }
    }
    X509_free(peer_cert);

    conn->state = BB10_TLS_STATE_AUTHENTICATED;

    // Cache negotiated version and cipher
    const char* ver_str = SSL_get_version(conn->ssl);
    strncpy(conn->negotiated_version, ver_str ? ver_str : "Unknown", sizeof(conn->negotiated_version) - 1);
    conn->negotiated_version[sizeof(conn->negotiated_version) - 1] = '\0';

    const char* cipher_str = SSL_get_cipher_name(conn->ssl);
    strncpy(conn->negotiated_cipher, cipher_str ? cipher_str : "Unknown", sizeof(conn->negotiated_cipher) - 1);
    conn->negotiated_cipher[sizeof(conn->negotiated_cipher) - 1] = '\0';

    return BB10_TLS_OK;
}

int bb10_tls_accept(bb10_tls_connection_t* conn) {
    if (!conn || !conn->ssl || !conn->ctx) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (conn->state == BB10_TLS_STATE_FAILED) {
        return BB10_TLS_ERR_IO_FAILED;
    }

    if (conn->ctx->policy == BB10_TLS_POLICY_P2P) {
        // P2P Non-mTLS Server Contract:
        // Server presents its credentials; does NOT request or require client certificate or client pin.
        SSL_set_verify(conn->ssl, SSL_VERIFY_NONE, NULL);
    }

    conn->state = BB10_TLS_STATE_HANDSHAKE;
    int rc = SSL_accept(conn->ssl);
    if (rc <= 0) {
        int ssl_err = SSL_get_error(conn->ssl, rc);
        if (ssl_err == SSL_ERROR_WANT_READ) {
            return BB10_TLS_ERR_WANT_READ;
        }
        if (ssl_err == SSL_ERROR_WANT_WRITE) {
            return BB10_TLS_ERR_WANT_WRITE;
        }
        conn->state = BB10_TLS_STATE_FAILED;
        if (ssl_err == SSL_ERROR_ZERO_RETURN) {
            return BB10_TLS_ERR_CLOSED;
        }

        char err_buf[256];
        unsigned long e = ERR_get_error();
        if (e != 0) {
            ERR_error_string_n(e, err_buf, sizeof(err_buf));
            conn->last_error = err_buf;
        } else {
            conn->last_error = "TLS server accept handshake failed";
        }
        return BB10_TLS_ERR_HANDSHAKE_FAILED;
    }

    conn->state = BB10_TLS_STATE_AUTHENTICATED;

    const char* ver_str = SSL_get_version(conn->ssl);
    strncpy(conn->negotiated_version, ver_str ? ver_str : "Unknown", sizeof(conn->negotiated_version) - 1);
    conn->negotiated_version[sizeof(conn->negotiated_version) - 1] = '\0';

    const char* cipher_str = SSL_get_cipher_name(conn->ssl);
    strncpy(conn->negotiated_cipher, cipher_str ? cipher_str : "Unknown", sizeof(conn->negotiated_cipher) - 1);
    conn->negotiated_cipher[sizeof(conn->negotiated_cipher) - 1] = '\0';

    return BB10_TLS_OK;
}

int bb10_tls_read(bb10_tls_connection_t* conn, void* buf, size_t count, size_t* bytes_read) {
    if (!conn || !conn->ssl || !buf || !bytes_read) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    *bytes_read = 0;

    if (conn->state != BB10_TLS_STATE_AUTHENTICATED) {
        conn->last_error = (conn->state == BB10_TLS_STATE_FAILED) ?
            "Connection in terminal failed state" : "Connection not authenticated";
        return (conn->state == BB10_TLS_STATE_FAILED) ? BB10_TLS_ERR_PIN_MISMATCH : BB10_TLS_ERR_IO_FAILED;
    }

    int rc = SSL_read(conn->ssl, buf, static_cast<int>(count));
    if (rc > 0) {
        *bytes_read = static_cast<size_t>(rc);
        return BB10_TLS_OK;
    }

    int ssl_err = SSL_get_error(conn->ssl, rc);
    if (ssl_err == SSL_ERROR_WANT_READ) {
        return BB10_TLS_ERR_WANT_READ;
    }
    if (ssl_err == SSL_ERROR_WANT_WRITE) {
        return BB10_TLS_ERR_WANT_WRITE;
    }
    if (ssl_err == SSL_ERROR_ZERO_RETURN) {
        return BB10_TLS_ERR_CLOSED;
    }

    conn->last_error = "SSL_read encountered an underlying I/O error";
    return BB10_TLS_ERR_IO_FAILED;
}

int bb10_tls_write(bb10_tls_connection_t* conn, const void* buf, size_t count, size_t* bytes_written) {
    if (!conn || !conn->ssl || !buf || !bytes_written) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    *bytes_written = 0;

    if (conn->state != BB10_TLS_STATE_AUTHENTICATED) {
        conn->last_error = (conn->state == BB10_TLS_STATE_FAILED) ?
            "Connection in terminal failed state" : "Connection not authenticated";
        return (conn->state == BB10_TLS_STATE_FAILED) ? BB10_TLS_ERR_PIN_MISMATCH : BB10_TLS_ERR_IO_FAILED;
    }

    int rc = SSL_write(conn->ssl, buf, static_cast<int>(count));
    if (rc > 0) {
        *bytes_written = static_cast<size_t>(rc);
        return BB10_TLS_OK;
    }

    int ssl_err = SSL_get_error(conn->ssl, rc);
    if (ssl_err == SSL_ERROR_WANT_READ) {
        return BB10_TLS_ERR_WANT_READ;
    }
    if (ssl_err == SSL_ERROR_WANT_WRITE) {
        return BB10_TLS_ERR_WANT_WRITE;
    }
    if (ssl_err == SSL_ERROR_ZERO_RETURN) {
        return BB10_TLS_ERR_CLOSED;
    }

    conn->last_error = "SSL_write encountered an underlying I/O error";
    return BB10_TLS_ERR_IO_FAILED;
}

int bb10_tls_shutdown(bb10_tls_connection_t* conn) {
    if (!conn || !conn->ssl) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    if (conn->state == BB10_TLS_STATE_FAILED) {
        return BB10_TLS_ERR_IO_FAILED;
    }
    if (conn->state == BB10_TLS_STATE_CLOSED) {
        return BB10_TLS_ERR_CLOSED;
    }

    int rc = SSL_shutdown(conn->ssl);
    if (rc >= 0) {
        conn->state = BB10_TLS_STATE_CLOSED;
        return BB10_TLS_OK;
    }

    int ssl_err = SSL_get_error(conn->ssl, rc);
    if (ssl_err == SSL_ERROR_WANT_READ) {
        return BB10_TLS_ERR_WANT_READ;
    }
    if (ssl_err == SSL_ERROR_WANT_WRITE) {
        return BB10_TLS_ERR_WANT_WRITE;
    }

    conn->state = BB10_TLS_STATE_CLOSED;
    conn->last_error = "SSL_shutdown failed";
    return BB10_TLS_ERR_IO_FAILED;
}

int bb10_tls_connection_get_fd(const bb10_tls_connection_t* conn) {
    if (!conn) {
        return -1;
    }
    return conn->fd;
}

int bb10_tls_connection_get_negotiated_version(const bb10_tls_connection_t* conn, char* out_buf, size_t buf_len) {
    if (!conn || !out_buf || buf_len == 0) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    strncpy(out_buf, conn->negotiated_version, buf_len - 1);
    out_buf[buf_len - 1] = '\0';
    return BB10_TLS_OK;
}

int bb10_tls_connection_get_cipher(const bb10_tls_connection_t* conn, char* out_buf, size_t buf_len) {
    if (!conn || !out_buf || buf_len == 0) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    strncpy(out_buf, conn->negotiated_cipher, buf_len - 1);
    out_buf[buf_len - 1] = '\0';
    return BB10_TLS_OK;
}

int bb10_tls_connection_get_peer_fingerprint(const bb10_tls_connection_t* conn, char* out_hex_65) {
    if (!conn || !out_hex_65) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    if (!conn->peer_fingerprint_valid) {
        return BB10_TLS_ERR_NO_PEER_CERT;
    }
    memcpy(out_hex_65, conn->peer_fingerprint, 65);
    return BB10_TLS_OK;
}

const char* bb10_tls_get_last_error(const bb10_tls_connection_t* conn) {
    if (!conn) {
        return "Invalid connection handle";
    }
    return conn->last_error.c_str();
}
