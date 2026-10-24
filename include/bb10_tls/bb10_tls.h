#ifndef BB10_TLS_H
#define BB10_TLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return and error codes */
#define BB10_TLS_OK                      0
#define BB10_TLS_ERR_WANT_READ          -1
#define BB10_TLS_ERR_WANT_WRITE         -2
#define BB10_TLS_ERR_CLOSED             -3
#define BB10_TLS_ERR_INVALID_ARG        -4
#define BB10_TLS_ERR_INVALID_PIN        -5
#define BB10_TLS_ERR_PIN_MISMATCH       -6
#define BB10_TLS_ERR_NO_PEER_CERT       -7
#define BB10_TLS_ERR_CERT_VERIFY        -8
#define BB10_TLS_ERR_CA_LOAD_FAILED     -9
#define BB10_TLS_ERR_HANDSHAKE_FAILED  -10
#define BB10_TLS_ERR_ENTROPY_FAILURE   -11
#define BB10_TLS_ERR_OUT_OF_MEMORY     -12
#define BB10_TLS_ERR_IO_FAILED         -13
#define BB10_TLS_ERR_INTERNAL          -14

/* Verification and usage policies */
typedef enum {
    BB10_TLS_POLICY_PUBLIC_PKI = 1,
    BB10_TLS_POLICY_P2P        = 2
} bb10_tls_policy_t;

/* TLS protocol versions */
typedef enum {
    BB10_TLS_VERSION_DEFAULT = 0,
    BB10_TLS_VERSION_1_2     = 0x0303,
    BB10_TLS_VERSION_1_3     = 0x0304
} bb10_tls_version_t;

/* Opaque context and connection handles */
typedef struct bb10_tls_context bb10_tls_context_t;
typedef struct bb10_tls_connection bb10_tls_connection_t;

/* Context management */
bb10_tls_context_t* bb10_tls_context_create(bb10_tls_policy_t policy);
void bb10_tls_context_destroy(bb10_tls_context_t* ctx);

int bb10_tls_context_set_ca_path(bb10_tls_context_t* ctx, const char* ca_bundle_path);
int bb10_tls_context_set_expected_fingerprint(bb10_tls_context_t* ctx, const char* sha256_hex);
int bb10_tls_context_set_credentials(bb10_tls_context_t* ctx,
                                     const char* cert_pem,
                                     size_t cert_len,
                                     const char* key_pem,
                                     size_t key_len);
int bb10_tls_context_set_min_tls_version(bb10_tls_context_t* ctx, bb10_tls_version_t version);
int bb10_tls_context_set_max_tls_version(bb10_tls_context_t* ctx, bb10_tls_version_t version);

/* Connection management */
bb10_tls_connection_t* bb10_tls_connection_create(bb10_tls_context_t* ctx, int socket_fd);
void bb10_tls_connection_destroy(bb10_tls_connection_t* conn);

int bb10_tls_connect(bb10_tls_connection_t* conn, const char* hostname);
int bb10_tls_accept(bb10_tls_connection_t* conn);

int bb10_tls_read(bb10_tls_connection_t* conn, void* buf, size_t count, size_t* bytes_read);
int bb10_tls_write(bb10_tls_connection_t* conn, const void* buf, size_t count, size_t* bytes_written);
int bb10_tls_shutdown(bb10_tls_connection_t* conn);

int bb10_tls_connection_get_fd(const bb10_tls_connection_t* conn);
int bb10_tls_connection_get_negotiated_version(const bb10_tls_connection_t* conn, char* out_buf, size_t buf_len);
int bb10_tls_connection_get_cipher(const bb10_tls_connection_t* conn, char* out_buf, size_t buf_len);
int bb10_tls_connection_get_peer_fingerprint(const bb10_tls_connection_t* conn, char* out_hex_65);
const char* bb10_tls_get_last_error(const bb10_tls_connection_t* conn);

/* Standalone crypto helpers */
int bb10_tls_random_bytes(unsigned char* buf, size_t len);
int bb10_tls_sha256(const unsigned char* data, size_t len, unsigned char* out_hash_32);
int bb10_tls_sha256_file(const char* filepath, char* out_hex_65);
int bb10_tls_calculate_cert_fingerprint(const char* cert_pem, size_t cert_len, char* out_hex_65);
int bb10_tls_generate_self_signed_cert(char** out_cert_pem,
                                       size_t* out_cert_len,
                                       char** out_key_pem,
                                       size_t* out_key_len);
void bb10_tls_free_string(char* str);
const char* bb10_tls_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* BB10_TLS_H */
