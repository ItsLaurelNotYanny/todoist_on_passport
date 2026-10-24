#include "bb10_tls/bb10_tls.h"
#include "bb10_tls/bb10_tls.hpp"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <iostream>
#include <sstream>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ \
                      << " in " << __FUNCTION__ << ": " << (msg) << std::endl; \
            std::exit(1); \
        } \
    } while (0)

static int g_test_count = 0;

static void run_test(void (*test_fn)(), const char* test_name) {
    std::cout << "[RUNNING] " << test_name << "..." << std::flush;
    test_fn();
    g_test_count++;
    std::cout << " PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// Test 1: Context Lifecycle & Invalid Policies
// ---------------------------------------------------------------------------
static void test_context_lifecycle() {
    bb10_tls_context_t* ctx_pki = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
    TEST_ASSERT(ctx_pki != NULL, "Public PKI context creation failed");
    bb10_tls_context_destroy(ctx_pki);

    bb10_tls_context_t* ctx_p2p = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(ctx_p2p != NULL, "P2P context creation failed");
    bb10_tls_context_destroy(ctx_p2p);

    bb10_tls_context_t* ctx_invalid = bb10_tls_context_create(static_cast<bb10_tls_policy_t>(999));
    TEST_ASSERT(ctx_invalid == NULL, "Invalid policy should return NULL");

    // NULL destroy must not crash
    bb10_tls_context_destroy(NULL);
}

// ---------------------------------------------------------------------------
// Test 2: Expected Fingerprint Pin Validation & Normalization
// ---------------------------------------------------------------------------
static void test_fingerprint_pin_validation() {
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(ctx != NULL, "Context create failed");

    // Valid standard 64 lowercase hex
    const char* valid_hex = "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef0";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, valid_hex) == BB10_TLS_OK,
                "Valid hex should succeed");

    // Valid with uppercase letters
    const char* valid_upper = "A1B2C3D4E5F60718293A4B5C6D7E8F90123456789ABCDEF0123456789ABCDEF0";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, valid_upper) == BB10_TLS_OK,
                "Valid uppercase hex should succeed");

    // Valid with colons (common fingerprint representation)
    const char* valid_colons = "A1:B2:C3:D4:E5:F6:07:18:29:3A:4B:5C:6D:7E:8F:90:"
                              "12:34:56:78:9A:BC:DE:F0:12:34:56:78:9A:BC:DE:F0";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, valid_colons) == BB10_TLS_OK,
                "Valid hex with colons should succeed");

    // Invalid: Too short (63 chars)
    const char* too_short = "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcde";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, too_short) == BB10_TLS_ERR_INVALID_PIN,
                "Too short pin must be rejected");

    // Invalid: Too long (65 chars)
    const char* too_long = "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef00";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, too_long) == BB10_TLS_ERR_INVALID_PIN,
                "Too long pin must be rejected");

    // Invalid: Illegal non-hex character 'g'
    const char* illegal_char = "g1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef0";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, illegal_char) == BB10_TLS_ERR_INVALID_PIN,
                "Non-hex character pin must be rejected");

    // Invalid: Empty string
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, "") == BB10_TLS_ERR_INVALID_PIN,
                "Empty string pin must be rejected");

    // Invalid: NULL pointer
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, NULL) == BB10_TLS_ERR_INVALID_ARG,
                "NULL pin must return INVALID_ARG");

    bb10_tls_context_destroy(ctx);
}

// ---------------------------------------------------------------------------
// Test 3: CSPRNG Random Bytes
// ---------------------------------------------------------------------------
static void test_random_bytes() {
    unsigned char buf1[32];
    unsigned char buf2[32];
    std::memset(buf1, 0, sizeof(buf1));
    std::memset(buf2, 0, sizeof(buf2));

    TEST_ASSERT(bb10_tls_random_bytes(buf1, sizeof(buf1)) == BB10_TLS_OK, "Random bytes 1 failed");
    TEST_ASSERT(bb10_tls_random_bytes(buf2, sizeof(buf2)) == BB10_TLS_OK, "Random bytes 2 failed");

    // Verify entropy (not all zeroes and not identical)
    bool non_zero1 = false;
    for (size_t i = 0; i < sizeof(buf1); ++i) {
        if (buf1[i] != 0) non_zero1 = true;
    }
    TEST_ASSERT(non_zero1, "Random bytes buffer 1 should not be all zeroes");
    TEST_ASSERT(std::memcmp(buf1, buf2, sizeof(buf1)) != 0, "Consecutive random byte buffers should differ");

    // Error cases
    TEST_ASSERT(bb10_tls_random_bytes(NULL, 16) == BB10_TLS_ERR_INVALID_ARG, "NULL buffer must fail");
    TEST_ASSERT(bb10_tls_random_bytes(buf1, 0) == BB10_TLS_ERR_INVALID_ARG, "Zero length must fail");
}

// ---------------------------------------------------------------------------
// Test 4: SHA-256 Hashing
// ---------------------------------------------------------------------------
static void test_sha256_hash() {
    // SHA-256 of empty string: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    const unsigned char empty_str[] = "";
    unsigned char hash[32];
    TEST_ASSERT(bb10_tls_sha256(empty_str, 0, hash) == BB10_TLS_OK, "SHA256 empty string failed");

    char hex[65];
    for (int i = 0; i < 32; ++i) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[64] = '\0';
    TEST_ASSERT(std::strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
                "Empty string SHA-256 mismatch");

    // Test file hashing
    const char* tmp_file = "/tmp/test_bb10_tls_sha.txt";
    FILE* fp = fopen(tmp_file, "wb");
    TEST_ASSERT(fp != NULL, "Failed to create temp file");
    const char* test_data = "The quick brown fox jumps over the lazy dog";
    fwrite(test_data, 1, strlen(test_data), fp);
    fclose(fp);

    char file_hex[65];
    TEST_ASSERT(bb10_tls_sha256_file(tmp_file, file_hex) == BB10_TLS_OK, "SHA256 file failed");
    // Known SHA-256 for "The quick brown fox jumps over the lazy dog":
    // d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592
    TEST_ASSERT(std::strcmp(file_hex, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592") == 0,
                "File SHA-256 mismatch");
    unlink(tmp_file);

    // Non-existent file
    TEST_ASSERT(bb10_tls_sha256_file("/tmp/non_existent_file_bb10_tls.bin", file_hex) == BB10_TLS_ERR_IO_FAILED,
                "Non-existent file should return IO_FAILED");
}

// ---------------------------------------------------------------------------
// Test 5: Self-Signed Certificate and Key Generation
// ---------------------------------------------------------------------------
static void test_self_signed_cert_generation() {
    char* cert_pem = NULL;
    size_t cert_len = 0;
    char* key_pem = NULL;
    size_t key_len = 0;

    int rc = bb10_tls_generate_self_signed_cert(&cert_pem, &cert_len, &key_pem, &key_len);
    TEST_ASSERT(rc == BB10_TLS_OK, "Self signed cert generation failed");
    TEST_ASSERT(cert_pem != NULL && cert_len > 0, "Certificate PEM invalid");
    TEST_ASSERT(key_pem != NULL && key_len > 0, "Key PEM invalid");

    // Verify PEM structure headers
    TEST_ASSERT(std::strstr(cert_pem, "-----BEGIN CERTIFICATE-----") != NULL, "Missing cert PEM header");
    TEST_ASSERT(std::strstr(key_pem, "-----BEGIN PRIVATE KEY-----") != NULL, "Missing key PEM header");

    // Verify it loads cleanly into context
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(ctx != NULL, "Context create failed");
    int set_rc = bb10_tls_context_set_credentials(ctx, cert_pem, cert_len, key_pem, key_len);
    TEST_ASSERT(set_rc == BB10_TLS_OK, "Loading generated credentials into context failed");

    bb10_tls_context_destroy(ctx);
    bb10_tls_free_string(cert_pem);
    bb10_tls_free_string(key_pem);
}

// ---------------------------------------------------------------------------
// Test 6: CA Bundle Load & Fail-Closed Behavior
// ---------------------------------------------------------------------------
static const char* find_cacert_path() {
    if (access("vendor/sources/cacert.pem", R_OK) == 0) {
        return "vendor/sources/cacert.pem";
    }
    if (access("../vendor/sources/cacert.pem", R_OK) == 0) {
        return "../vendor/sources/cacert.pem";
    }
    return NULL;
}

static void test_ca_bundle_loading() {
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
    TEST_ASSERT(ctx != NULL, "Context create failed");

    // Non-existent path fails closed
    int rc_bad = bb10_tls_context_set_ca_path(ctx, "/path/does/not/exist/cacert.pem");
    TEST_ASSERT(rc_bad == BB10_TLS_ERR_CA_LOAD_FAILED, "Non-existent CA bundle must return CA_LOAD_FAILED");

    // Real CA bundle from vendor/sources/cacert.pem
    const char* ca_path = find_cacert_path();
    TEST_ASSERT(ca_path != NULL, "cacert.pem must be present in vendor/sources/");
    int rc_good = bb10_tls_context_set_ca_path(ctx, ca_path);
    TEST_ASSERT(rc_good == BB10_TLS_OK, "Valid vendor CA bundle should load successfully");

    bb10_tls_context_destroy(ctx);
}

// ---------------------------------------------------------------------------
// Server thread argument and worker for loopback handshake
// ---------------------------------------------------------------------------
struct ServerThreadArg {
    bb10_tls_context_t* ctx;
    int fd;
    int result_rc;
    char negotiated_version[32];
    char negotiated_cipher[64];
    std::string received_msg;
};

static void* p2p_server_worker(void* arg) {
    ServerThreadArg* sarg = static_cast<ServerThreadArg*>(arg);
    bb10_tls_connection_t* conn = bb10_tls_connection_create(sarg->ctx, sarg->fd);
    if (!conn) {
        sarg->result_rc = BB10_TLS_ERR_INTERNAL;
        close(sarg->fd);
        return NULL;
    }

    sarg->result_rc = bb10_tls_accept(conn);
    if (sarg->result_rc == BB10_TLS_OK) {
        bb10_tls_connection_get_negotiated_version(conn, sarg->negotiated_version, sizeof(sarg->negotiated_version));
        bb10_tls_connection_get_cipher(conn, sarg->negotiated_cipher, sizeof(sarg->negotiated_cipher));

        // Read client message
        char buf[128];
        size_t nread = 0;
        int r_rc = bb10_tls_read(conn, buf, sizeof(buf) - 1, &nread);
        if (r_rc == BB10_TLS_OK && nread > 0) {
            buf[nread] = '\0';
            sarg->received_msg = buf;
            // Echo response
            const char* resp = "PONG_FROM_SERVER";
            size_t nwritten = 0;
            bb10_tls_write(conn, resp, strlen(resp), &nwritten);
        }
        bb10_tls_shutdown(conn);
    }

    bb10_tls_connection_destroy(conn);
    return NULL;
}

// ---------------------------------------------------------------------------
// Test 7: Loopback Socketpair TLS 1.3 Handshake & Bi-directional Transfer
// ---------------------------------------------------------------------------
static void test_loopback_p2p_tls13_handshake() {
    // 1. Generate Server credentials
    char* cert_pem = NULL;
    size_t cert_len = 0;
    char* key_pem = NULL;
    size_t key_len = 0;
    TEST_ASSERT(bb10_tls_generate_self_signed_cert(&cert_pem, &cert_len, &key_pem, &key_len) == BB10_TLS_OK,
                "Cert gen failed");

    // Calculate certificate fingerprint
    BIO* cbio = BIO_new_mem_buf(cert_pem, static_cast<int>(cert_len));
    X509* cert = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    BIO_free(cbio);
    TEST_ASSERT(cert != NULL, "PEM parse failed");

    unsigned char* der = NULL;
    int der_len = i2d_X509(cert, &der);
    TEST_ASSERT(der_len > 0, "DER encode failed");
    unsigned char hash[32];
    TEST_ASSERT(bb10_tls_sha256(der, static_cast<size_t>(der_len), hash) == BB10_TLS_OK, "SHA256 failed");
    OPENSSL_free(der);
    X509_free(cert);

    char server_fingerprint[65];
    for (int i = 0; i < 32; ++i) {
        sprintf(server_fingerprint + (i * 2), "%02x", hash[i]);
    }
    server_fingerprint[64] = '\0';

    // 2. Setup Server Context
    bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(s_ctx != NULL, "Server ctx create failed");
    TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_pem, cert_len, key_pem, key_len) == BB10_TLS_OK,
                "Server set credentials failed");

    // 3. Setup Client Context (Configured with expected server fingerprint)
    bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(c_ctx != NULL, "Client ctx create failed");
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(c_ctx, server_fingerprint) == BB10_TLS_OK,
                "Client set expected fingerprint failed");

    // 4. Create full-duplex socket pair
    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

    ServerThreadArg sarg;
    sarg.ctx = s_ctx;
    sarg.fd = sv[0];
    sarg.result_rc = -999;
    sarg.negotiated_version[0] = '\0';
    sarg.negotiated_cipher[0] = '\0';

    pthread_t sthread;
    TEST_ASSERT(pthread_create(&sthread, NULL, p2p_server_worker, &sarg) == 0, "pthread_create failed");

    // Client connection
    bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
    TEST_ASSERT(c_conn != NULL, "Client connection create failed");

    int c_rc = bb10_tls_connect(c_conn, NULL);
    TEST_ASSERT(c_rc == BB10_TLS_OK, "Client bb10_tls_connect failed");

    // Verify TLS 1.3 negotiated
    char c_ver[32];
    bb10_tls_connection_get_negotiated_version(c_conn, c_ver, sizeof(c_ver));
    TEST_ASSERT(std::strcmp(c_ver, "TLSv1.3") == 0, "Negotiated version must be TLSv1.3");

    // Verify peer fingerprint matches
    char c_peer_fp[65];
    TEST_ASSERT(bb10_tls_connection_get_peer_fingerprint(c_conn, c_peer_fp) == BB10_TLS_OK, "Get peer fingerprint failed");
    TEST_ASSERT(std::strcmp(c_peer_fp, server_fingerprint) == 0, "Peer fingerprint does not match expected");

    // Send data to server
    const char* ping_msg = "PING_FROM_CLIENT";
    size_t written = 0;
    TEST_ASSERT(bb10_tls_write(c_conn, ping_msg, strlen(ping_msg), &written) == BB10_TLS_OK, "Client write failed");
    TEST_ASSERT(written == strlen(ping_msg), "Write size mismatch");

    // Read response from server
    char resp_buf[128];
    size_t nread = 0;
    TEST_ASSERT(bb10_tls_read(c_conn, resp_buf, sizeof(resp_buf) - 1, &nread) == BB10_TLS_OK, "Client read failed");
    resp_buf[nread] = '\0';
    TEST_ASSERT(std::strcmp(resp_buf, "PONG_FROM_SERVER") == 0, "Server response content mismatch");

    bb10_tls_shutdown(c_conn);
    bb10_tls_connection_destroy(c_conn);

    pthread_join(sthread, NULL);
    TEST_ASSERT(sarg.result_rc == BB10_TLS_OK, "Server accept failed");
    TEST_ASSERT(sarg.received_msg == "PING_FROM_CLIENT", "Server received content mismatch");

    bb10_tls_context_destroy(s_ctx);
    bb10_tls_context_destroy(c_ctx);
    bb10_tls_free_string(cert_pem);
    bb10_tls_free_string(key_pem);
}

// ---------------------------------------------------------------------------
// Test 8: P2P Fingerprint Mismatch Rejection
// ---------------------------------------------------------------------------
static void test_p2p_fingerprint_mismatch_rejection() {
    char* cert_pem = NULL;
    size_t cert_len = 0;
    char* key_pem = NULL;
    size_t key_len = 0;
    TEST_ASSERT(bb10_tls_generate_self_signed_cert(&cert_pem, &cert_len, &key_pem, &key_len) == BB10_TLS_OK,
                "Cert gen failed");

    bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_pem, cert_len, key_pem, key_len) == BB10_TLS_OK,
                "Server set creds failed");

    // Client has a deliberate wrong fingerprint pin
    const char* wrong_pin = "0000000000000000000000000000000000000000000000000000000000000000";
    bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(c_ctx, wrong_pin) == BB10_TLS_OK,
                "Client set wrong pin failed");

    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

    ServerThreadArg sarg;
    sarg.ctx = s_ctx;
    sarg.fd = sv[0];
    sarg.result_rc = -999;

    pthread_t sthread;
    TEST_ASSERT(pthread_create(&sthread, NULL, p2p_server_worker, &sarg) == 0, "pthread_create failed");

    bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
    TEST_ASSERT(c_conn != NULL, "Client conn create failed");

    int c_rc = bb10_tls_connect(c_conn, NULL);
    TEST_ASSERT(c_rc == BB10_TLS_ERR_PIN_MISMATCH, "Client must reject with PIN_MISMATCH");

    bb10_tls_connection_destroy(c_conn);
    pthread_join(sthread, NULL);

    bb10_tls_context_destroy(s_ctx);
    bb10_tls_context_destroy(c_ctx);
    bb10_tls_free_string(cert_pem);
    bb10_tls_free_string(key_pem);
}

// ---------------------------------------------------------------------------
// Test 9: P2P Verifying Client Missing Pin Fails Closed Before Network I/O
// ---------------------------------------------------------------------------
static void test_p2p_missing_pin_fails_closed() {
    bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(c_ctx != NULL, "Client ctx create failed");

    // Do NOT set expected fingerprint on client
    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

    bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[0]);
    TEST_ASSERT(c_conn != NULL, "Client conn create failed");

    int c_rc = bb10_tls_connect(c_conn, NULL);
    TEST_ASSERT(c_rc == BB10_TLS_ERR_INVALID_PIN, "Client without expected pin must fail with INVALID_PIN");

    bb10_tls_connection_destroy(c_conn);
    close(sv[1]);
    bb10_tls_context_destroy(c_ctx);
}

// ---------------------------------------------------------------------------
// Test 10: Descriptor Sentinel & Sole Ownership Rules
// ---------------------------------------------------------------------------
static void test_descriptor_sentinel_and_ownership() {
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(ctx != NULL, "Context create failed");

    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

    int fd0 = sv[0];
    int fd1 = sv[1];

    bb10_tls_connection_t* conn = bb10_tls_connection_create(ctx, fd0);
    TEST_ASSERT(conn != NULL, "Connection create failed");
    TEST_ASSERT(bb10_tls_connection_get_fd(conn) == fd0, "get_fd mismatch");

    // Destroy connection: must close fd0
    bb10_tls_connection_destroy(conn);

    // Verify fd0 is now closed (write or close on it fails with EBADF)
    char dummy = 'x';
    ssize_t w = write(fd0, &dummy, 1);
    TEST_ASSERT(w < 0, "Destroyed socket fd should be closed");

    // Sentinel -1 check
    TEST_ASSERT(bb10_tls_connection_get_fd(NULL) == -1, "NULL connection get_fd must return -1");

    close(fd1);
    bb10_tls_context_destroy(ctx);
}

// ---------------------------------------------------------------------------
// Test 11: Public PKI Verification Fails Closed Against Self-Signed Server
// ---------------------------------------------------------------------------
static void test_public_pki_rejects_untrusted_server() {
    char* cert_pem = NULL;
    size_t cert_len = 0;
    char* key_pem = NULL;
    size_t key_len = 0;
    TEST_ASSERT(bb10_tls_generate_self_signed_cert(&cert_pem, &cert_len, &key_pem, &key_len) == BB10_TLS_OK,
                "Cert gen failed");

    bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_pem, cert_len, key_pem, key_len) == BB10_TLS_OK,
                "Server credentials failed");

    // Client uses Public PKI with official Mozilla CA bundle
    bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
    const char* ca_path = find_cacert_path();
    TEST_ASSERT(ca_path != NULL, "cacert.pem must be present in vendor/sources/");
    TEST_ASSERT(bb10_tls_context_set_ca_path(c_ctx, ca_path) == BB10_TLS_OK,
                "CA path load failed");

    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

    ServerThreadArg sarg;
    sarg.ctx = s_ctx;
    sarg.fd = sv[0];
    sarg.result_rc = -999;

    pthread_t sthread;
    TEST_ASSERT(pthread_create(&sthread, NULL, p2p_server_worker, &sarg) == 0, "pthread_create failed");

    bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
    TEST_ASSERT(c_conn != NULL, "Client conn create failed");

    // Client connects expecting untrusted server; CA chain check must fail
    int c_rc = bb10_tls_connect(c_conn, "example.com");
    TEST_ASSERT(c_rc == BB10_TLS_ERR_CERT_VERIFY || c_rc == BB10_TLS_ERR_HANDSHAKE_FAILED,
                "Public PKI client must reject untrusted self-signed certificate");

    bb10_tls_connection_destroy(c_conn);
    pthread_join(sthread, NULL);

    bb10_tls_context_destroy(s_ctx);
    bb10_tls_context_destroy(c_ctx);
    bb10_tls_free_string(cert_pem);
    bb10_tls_free_string(key_pem);
}

// ---------------------------------------------------------------------------
// Test 12: SAN Precedence (RFC 6125) Verification
// ---------------------------------------------------------------------------
static void test_san_precedence_rfc6125() {
    // Generate an EC key
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    TEST_ASSERT(pkey != NULL, "EVP_EC_gen failed");

    X509* cert = X509_new();
    TEST_ASSERT(cert != NULL, "X509_new failed");
    X509_set_version(cert, 2); // X509 v3
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 12345);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 315360000L);

    // Subject Common Name is "legacy-cn.example.com"
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"legacy-cn.example.com", -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    X509_set_pubkey(cert, pkey);

    // Add Subject Alternative Name (SAN): "DNS:san-precedence.example.com"
    X509V3_CTX v3_ctx;
    X509V3_set_ctx_nodb(&v3_ctx);
    X509V3_set_ctx(&v3_ctx, cert, cert, NULL, NULL, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3_ctx, NID_subject_alt_name,
                                             const_cast<char*>("DNS:san-precedence.example.com"));
    TEST_ASSERT(ext != NULL, "SAN extension creation failed");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);

    TEST_ASSERT(X509_sign(cert, pkey, EVP_sha256()) > 0, "X509_sign failed");

    // OpenSSL X509_check_host verification check
    // 1. Check against SAN name -> MUST MATCH (1)
    int san_match = X509_check_host(cert, "san-precedence.example.com", 0, 0, NULL);
    TEST_ASSERT(san_match == 1, "SAN hostname must match");

    // 2. Check against CN name when SAN is present -> MUST FAIL (0) per RFC 6125 (CN is ignored if SAN is present!)
    int cn_match = X509_check_host(cert, "legacy-cn.example.com", 0, 0, NULL);
    TEST_ASSERT(cn_match == 0, "RFC 6125 SAN Precedence: CN must be ignored when SAN is present");

    X509_free(cert);
    EVP_PKEY_free(pkey);
}

// ---------------------------------------------------------------------------
// Test 13: C++03 RAII Wrappers
// ---------------------------------------------------------------------------
static void test_cpp03_raii_wrappers() {
    bb10_tls::Context ctx(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(ctx.isValid(), "Context wrapper isValid failed");
    TEST_ASSERT(ctx.setExpectedFingerprint("a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef0") == BB10_TLS_OK,
                "Context setExpectedFingerprint failed");

    std::string certPem, keyPem;
    TEST_ASSERT(bb10_tls::generateSelfSignedCert(certPem, keyPem) == BB10_TLS_OK,
                "generateSelfSignedCert failed");
    TEST_ASSERT(!certPem.empty() && !keyPem.empty(), "Generated PEMs empty");

    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

    {
        bb10_tls::Connection conn(ctx, sv[0]);
        TEST_ASSERT(conn.isValid(), "Connection wrapper isValid failed");
        TEST_ASSERT(conn.getFd() == sv[0], "Connection getFd mismatch");
        // conn goes out of scope here and destructor closes sv[0]
    }

    // Verify sv[0] is closed by RAII destructor
    char c = 0;
    TEST_ASSERT(write(sv[0], &c, 1) < 0, "Connection RAII destructor must close fd");
    close(sv[1]);
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

// ---------------------------------------------------------------------------
// Test 14: Mock Entropy Failure Injection
// ---------------------------------------------------------------------------
static int mock_failing_rand_bytes(unsigned char* /*buf*/, int /*num*/) {
    return 0; // Fail RNG
}

static int mock_failing_rand_status(void) {
    return 0;
}

static void test_entropy_failure_injection() {
    RAND_METHOD failing_meth;
    std::memset(&failing_meth, 0, sizeof(failing_meth));
    failing_meth.bytes = mock_failing_rand_bytes;
    failing_meth.pseudorand = mock_failing_rand_bytes;
    failing_meth.status = mock_failing_rand_status;

    const RAND_METHOD* orig_meth = RAND_get_rand_method();
    RAND_set_rand_method(&failing_meth);

    unsigned char buf[32];
    int r_rc = bb10_tls_random_bytes(buf, sizeof(buf));
    TEST_ASSERT(r_rc == BB10_TLS_ERR_ENTROPY_FAILURE, "bb10_tls_random_bytes must fail with ENTROPY_FAILURE");

    char* cert = NULL;
    char* key = NULL;
    size_t clen = 0, klen = 0;
    int g_rc = bb10_tls_generate_self_signed_cert(&cert, &clen, &key, &klen);
    TEST_ASSERT(g_rc == BB10_TLS_ERR_ENTROPY_FAILURE, "bb10_tls_generate_self_signed_cert must fail with ENTROPY_FAILURE");

    // Restore original RAND method
    RAND_set_rand_method(orig_meth);

    r_rc = bb10_tls_random_bytes(buf, sizeof(buf));
    TEST_ASSERT(r_rc == BB10_TLS_OK, "bb10_tls_random_bytes must succeed after restoring RNG");
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void calc_cert_fingerprint(const char* cert_pem, size_t cert_len, char* out_hex_65) {
    BIO* cbio = BIO_new_mem_buf(cert_pem, static_cast<int>(cert_len));
    X509* cert = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    BIO_free(cbio);
    unsigned char* der = NULL;
    int der_len = i2d_X509(cert, &der);
    unsigned char hash[32];
    bb10_tls_sha256(der, static_cast<size_t>(der_len), hash);
    OPENSSL_free(der);
    X509_free(cert);
    for (int i = 0; i < 32; ++i) {
        sprintf(out_hex_65 + (i * 2), "%02x", hash[i]);
    }
    out_hex_65[64] = '\0';
}

// ---------------------------------------------------------------------------
// Test 15: Forced TLS Versions and Cross-Version Rejection
// ---------------------------------------------------------------------------
struct VersionServerArg {
    bb10_tls_context_t* ctx;
    int fd;
    int result_rc;
    std::string negotiated_ver;
};

static void* version_server_worker(void* ptr) {
    VersionServerArg* arg = static_cast<VersionServerArg*>(ptr);
    bb10_tls_connection_t* conn = bb10_tls_connection_create(arg->ctx, arg->fd);
    if (!conn) {
        arg->result_rc = -100;
        return NULL;
    }
    arg->result_rc = bb10_tls_accept(conn);
    if (arg->result_rc == BB10_TLS_OK) {
        char ver[32];
        bb10_tls_connection_get_negotiated_version(conn, ver, sizeof(ver));
        arg->negotiated_ver = ver;
    }
    bb10_tls_connection_destroy(conn);
    return NULL;
}

static void test_forced_tls_versions_and_rejection() {
    char *cert_pem = NULL, *key_pem = NULL;
    size_t cert_len = 0, key_len = 0;
    TEST_ASSERT(bb10_tls_generate_self_signed_cert(&cert_pem, &cert_len, &key_pem, &key_len) == 0,
                "Cert generation failed");

    char s_fp[65];
    calc_cert_fingerprint(cert_pem, cert_len, s_fp);

    // Scenario A: Both sides forced to TLS 1.3
    {
        int sv[2];
        TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

        bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
        TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_pem, cert_len, key_pem, key_len) == 0, "set creds");
        bb10_tls_context_set_min_tls_version(s_ctx, BB10_TLS_VERSION_1_3);
        bb10_tls_context_set_max_tls_version(s_ctx, BB10_TLS_VERSION_1_3);

        bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
        TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(c_ctx, s_fp) == 0, "set fp");
        bb10_tls_context_set_min_tls_version(c_ctx, BB10_TLS_VERSION_1_3);
        bb10_tls_context_set_max_tls_version(c_ctx, BB10_TLS_VERSION_1_3);

        VersionServerArg sarg;
        sarg.ctx = s_ctx;
        sarg.fd = sv[0];
        sarg.result_rc = -999;
        pthread_t st;
        pthread_create(&st, NULL, version_server_worker, &sarg);

        bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
        int c_rc = bb10_tls_connect(c_conn, "localhost");
        TEST_ASSERT(c_rc == BB10_TLS_OK, "TLS 1.3 client connect failed");
        char c_ver[32];
        bb10_tls_connection_get_negotiated_version(c_conn, c_ver, sizeof(c_ver));
        TEST_ASSERT(std::string(c_ver) == "TLSv1.3", "Negotiated version must be TLSv1.3");

        bb10_tls_connection_destroy(c_conn);
        pthread_join(st, NULL);
        TEST_ASSERT(sarg.result_rc == BB10_TLS_OK, "TLS 1.3 server accept failed");
        TEST_ASSERT(sarg.negotiated_ver == "TLSv1.3", "Server negotiated version must be TLSv1.3");

        bb10_tls_context_destroy(s_ctx);
        bb10_tls_context_destroy(c_ctx);
        close(sv[0]);
        close(sv[1]);
    }

    // Scenario B: Both sides forced to TLS 1.2
    {
        int sv[2];
        TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

        bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
        TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_pem, cert_len, key_pem, key_len) == 0, "set creds");
        bb10_tls_context_set_min_tls_version(s_ctx, BB10_TLS_VERSION_1_2);
        bb10_tls_context_set_max_tls_version(s_ctx, BB10_TLS_VERSION_1_2);

        bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
        TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(c_ctx, s_fp) == 0, "set fp");
        bb10_tls_context_set_min_tls_version(c_ctx, BB10_TLS_VERSION_1_2);
        bb10_tls_context_set_max_tls_version(c_ctx, BB10_TLS_VERSION_1_2);

        VersionServerArg sarg;
        sarg.ctx = s_ctx;
        sarg.fd = sv[0];
        sarg.result_rc = -999;
        pthread_t st;
        pthread_create(&st, NULL, version_server_worker, &sarg);

        bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
        int c_rc = bb10_tls_connect(c_conn, "localhost");
        TEST_ASSERT(c_rc == BB10_TLS_OK, "TLS 1.2 client connect failed");
        char c_ver[32];
        bb10_tls_connection_get_negotiated_version(c_conn, c_ver, sizeof(c_ver));
        TEST_ASSERT(std::string(c_ver) == "TLSv1.2", "Negotiated version must be TLSv1.2");

        bb10_tls_connection_destroy(c_conn);
        pthread_join(st, NULL);
        TEST_ASSERT(sarg.result_rc == BB10_TLS_OK, "TLS 1.2 server accept failed");
        TEST_ASSERT(sarg.negotiated_ver == "TLSv1.2", "Server negotiated version must be TLSv1.2");

        bb10_tls_context_destroy(s_ctx);
        bb10_tls_context_destroy(c_ctx);
        close(sv[0]);
        close(sv[1]);
    }

    // Scenario C: Cross-version mismatch (server TLS 1.3 only, client TLS 1.2 only)
    {
        int sv[2];
        TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");

        bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
        TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_pem, cert_len, key_pem, key_len) == 0, "set creds");
        bb10_tls_context_set_min_tls_version(s_ctx, BB10_TLS_VERSION_1_3);
        bb10_tls_context_set_max_tls_version(s_ctx, BB10_TLS_VERSION_1_3);

        bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
        TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(c_ctx, s_fp) == 0, "set fp");
        bb10_tls_context_set_min_tls_version(c_ctx, BB10_TLS_VERSION_1_2);
        bb10_tls_context_set_max_tls_version(c_ctx, BB10_TLS_VERSION_1_2);

        VersionServerArg sarg;
        sarg.ctx = s_ctx;
        sarg.fd = sv[0];
        sarg.result_rc = -999;
        pthread_t st;
        pthread_create(&st, NULL, version_server_worker, &sarg);

        bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
        int c_rc = bb10_tls_connect(c_conn, "localhost");
        TEST_ASSERT(c_rc == BB10_TLS_ERR_HANDSHAKE_FAILED, "Cross-version mismatch must fail handshake");

        bb10_tls_connection_destroy(c_conn);
        pthread_join(st, NULL);
        TEST_ASSERT(sarg.result_rc != BB10_TLS_OK, "Server must fail on incompatible client version");

        bb10_tls_context_destroy(s_ctx);
        bb10_tls_context_destroy(c_ctx);
        close(sv[0]);
        close(sv[1]);
    }

    bb10_tls_free_string(cert_pem);
    bb10_tls_free_string(key_pem);
}

// ---------------------------------------------------------------------------
// Test 16: Real IP SAN Matching and Mismatch Rejection
// ---------------------------------------------------------------------------
static void test_real_ip_san_matching() {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    TEST_ASSERT(pkey != NULL, "EVP_EC_gen failed");

    X509* cert = X509_new();
    TEST_ASSERT(cert != NULL, "X509_new failed");
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 99999);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 315360000L);

    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"127.0.0.1", -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    X509_set_pubkey(cert, pkey);

    X509V3_CTX v3_ctx;
    X509V3_set_ctx_nodb(&v3_ctx);
    X509V3_set_ctx(&v3_ctx, cert, cert, NULL, NULL, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3_ctx, NID_subject_alt_name,
                                              const_cast<char*>("IP:127.0.0.1,IP:192.168.1.50"));
    TEST_ASSERT(ext != NULL, "IP SAN extension creation failed");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);

    TEST_ASSERT(X509_sign(cert, pkey, EVP_sha256()) > 0, "X509_sign failed");

    // 1. Direct OpenSSL IP SAN checking via X509_check_ip_asc
    TEST_ASSERT(X509_check_ip_asc(cert, "127.0.0.1", 0) == 1, "IP 127.0.0.1 SAN must match");
    TEST_ASSERT(X509_check_ip_asc(cert, "192.168.1.50", 0) == 1, "IP 192.168.1.50 SAN must match");
    TEST_ASSERT(X509_check_ip_asc(cert, "10.0.0.1", 0) == 0, "Mismatched IP 10.0.0.1 must be rejected");

    // 2. Full bb10_tls adapter verification with Public PKI trust
    BIO* cert_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert_bio, cert);
    char* cert_buf = NULL;
    long cert_len = BIO_get_mem_data(cert_bio, &cert_buf);

    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(key_bio, pkey, NULL, NULL, 0, NULL, NULL);
    char* key_buf = NULL;
    long key_len = BIO_get_mem_data(key_bio, &key_buf);

    // Save cert as trusted CA file
    const char* tmp_ca_path = "/tmp/test_bb10_tls_ip_san_ca.pem";
    FILE* fp = fopen(tmp_ca_path, "wb");
    TEST_ASSERT(fp != NULL, "Failed to create temp CA file");
    fwrite(cert_buf, 1, static_cast<size_t>(cert_len), fp);
    fclose(fp);

    // Server context using these credentials
    bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, cert_buf, static_cast<size_t>(cert_len),
                                                key_buf, static_cast<size_t>(key_len)) == BB10_TLS_OK,
                "set_credentials failed");

    // Scenario A: Client with trusted CA connects to matching IP "127.0.0.1" -> MUST SUCCEED
    {
        int sv[2];
        TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");
        ServerThreadArg sarg;
        sarg.ctx = s_ctx;
        sarg.fd = sv[0];
        sarg.result_rc = -999;
        pthread_t st;
        TEST_ASSERT(pthread_create(&st, NULL, p2p_server_worker, &sarg) == 0, "pthread_create failed");

        bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
        TEST_ASSERT(bb10_tls_context_set_ca_path(c_ctx, tmp_ca_path) == BB10_TLS_OK, "set_ca_path failed");
        bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
        TEST_ASSERT(c_conn != NULL, "connection_create failed");

        int c_rc = bb10_tls_connect(c_conn, "127.0.0.1");
        TEST_ASSERT(c_rc == BB10_TLS_OK, "bb10_tls_connect to matching IP 127.0.0.1 must succeed");
        bb10_tls_connection_destroy(c_conn);
        pthread_join(st, NULL);
        bb10_tls_context_destroy(c_ctx);
    }

    // Scenario B: Client with trusted CA connects to mismatched IP "10.0.0.1" -> MUST FAIL WITH CERT_VERIFY
    {
        int sv[2];
        TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");
        ServerThreadArg sarg;
        sarg.ctx = s_ctx;
        sarg.fd = sv[0];
        sarg.result_rc = -999;
        pthread_t st;
        TEST_ASSERT(pthread_create(&st, NULL, p2p_server_worker, &sarg) == 0, "pthread_create failed");

        bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
        TEST_ASSERT(bb10_tls_context_set_ca_path(c_ctx, tmp_ca_path) == BB10_TLS_OK, "set_ca_path failed");
        bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
        TEST_ASSERT(c_conn != NULL, "connection_create failed");

        int c_rc = bb10_tls_connect(c_conn, "10.0.0.1");
        TEST_ASSERT(c_rc == BB10_TLS_ERR_CERT_VERIFY, "bb10_tls_connect to mismatched IP 10.0.0.1 must fail with CERT_VERIFY");
        bb10_tls_connection_destroy(c_conn);
        pthread_join(st, NULL);
        bb10_tls_context_destroy(c_ctx);
    }

    // Scenario C: Client without CA bundle connects to "127.0.0.1" -> MUST FAIL WITH CERT_VERIFY (untrusted CA)
    {
        int sv[2];
        TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");
        ServerThreadArg sarg;
        sarg.ctx = s_ctx;
        sarg.fd = sv[0];
        sarg.result_rc = -999;
        pthread_t st;
        TEST_ASSERT(pthread_create(&st, NULL, p2p_server_worker, &sarg) == 0, "pthread_create failed");

        bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
        bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
        TEST_ASSERT(c_conn != NULL, "connection_create failed");

        int c_rc = bb10_tls_connect(c_conn, "127.0.0.1");
        TEST_ASSERT(c_rc == BB10_TLS_ERR_CERT_VERIFY || c_rc == BB10_TLS_ERR_HANDSHAKE_FAILED,
                    "bb10_tls_connect with untrusted CA must fail with CERT_VERIFY");
        bb10_tls_connection_destroy(c_conn);
        pthread_join(st, NULL);
        bb10_tls_context_destroy(c_ctx);
    }

    unlink(tmp_ca_path);
    bb10_tls_context_destroy(s_ctx);
    BIO_free(cert_bio);
    BIO_free(key_bio);

    X509_free(cert);
    EVP_PKEY_free(pkey);
}

// ---------------------------------------------------------------------------
// Main test entrypoint
// ---------------------------------------------------------------------------
int main() {
    signal(SIGPIPE, SIG_IGN);

    std::cout << "========================================" << std::endl;
    std::cout << " Running bb10_tls Core Unit Test Suite  " << std::endl;
    std::cout << "========================================" << std::endl;

    run_test(test_context_lifecycle, "test_context_lifecycle");
    run_test(test_fingerprint_pin_validation, "test_fingerprint_pin_validation");
    run_test(test_random_bytes, "test_random_bytes");
    run_test(test_sha256_hash, "test_sha256_hash");
    run_test(test_self_signed_cert_generation, "test_self_signed_cert_generation");
    run_test(test_ca_bundle_loading, "test_ca_bundle_loading");
    run_test(test_loopback_p2p_tls13_handshake, "test_loopback_p2p_tls13_handshake");
    run_test(test_p2p_fingerprint_mismatch_rejection, "test_p2p_fingerprint_mismatch_rejection");
    run_test(test_p2p_missing_pin_fails_closed, "test_p2p_missing_pin_fails_closed");
    run_test(test_descriptor_sentinel_and_ownership, "test_descriptor_sentinel_and_ownership");
    run_test(test_public_pki_rejects_untrusted_server, "test_public_pki_rejects_untrusted_server");
    run_test(test_san_precedence_rfc6125, "test_san_precedence_rfc6125");
    run_test(test_cpp03_raii_wrappers, "test_cpp03_raii_wrappers");
    run_test(test_entropy_failure_injection, "test_entropy_failure_injection");
    run_test(test_forced_tls_versions_and_rejection, "test_forced_tls_versions_and_rejection");
    run_test(test_real_ip_san_matching, "test_real_ip_san_matching");

    std::cout << "========================================" << std::endl;
    std::cout << " All " << g_test_count << " bb10_tls unit tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
