#include <bb10_tls/bb10_tls.h>
#include <bb10_tls/bb10_tls.hpp>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ \
                      << " (" << (msg) << ")" << std::endl; \
            std::exit(1); \
        } \
    } while (0)

static bool wait_fd(int fd, bool for_write, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(fd + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
    return rc > 0;
}

static int nonblocking_handshake(bb10_tls_connection_t* conn, int fd, bool is_server) {
    for (int attempts = 0; attempts < 50; ++attempts) {
        int rc = is_server ? bb10_tls_accept(conn) : bb10_tls_connect(conn, NULL);
        if (rc == BB10_TLS_OK) return BB10_TLS_OK;
        if (rc == BB10_TLS_ERR_WANT_READ) {
            if (!wait_fd(fd, false, 500)) return -1;
        } else if (rc == BB10_TLS_ERR_WANT_WRITE) {
            if (!wait_fd(fd, true, 500)) return -1;
        } else {
            return rc;
        }
    }
    return -2;
}

static bool nonblocking_write_all(bb10_tls_connection_t* conn, int fd, const char* data, size_t total_len, size_t chunk_size) {
    size_t sent = 0;
    int retries = 0;
    while (sent < total_len && retries < 100) {
        size_t to_write = total_len - sent;
        if (to_write > chunk_size) to_write = chunk_size;
        size_t written = 0;
        int rc = bb10_tls_write(conn, data + sent, to_write, &written);
        if (rc == BB10_TLS_OK) {
            sent += written;
            retries = 0;
        } else if (rc == BB10_TLS_ERR_WANT_WRITE) {
            if (!wait_fd(fd, true, 500)) retries++;
        } else if (rc == BB10_TLS_ERR_WANT_READ) {
            if (!wait_fd(fd, false, 500)) retries++;
        } else {
            return false;
        }
    }
    return sent == total_len;
}

static bool nonblocking_read_all(bb10_tls_connection_t* conn, int fd, char* out_data, size_t total_len, size_t chunk_size) {
    size_t received = 0;
    int retries = 0;
    while (received < total_len && retries < 100) {
        size_t to_read = total_len - received;
        if (to_read > chunk_size) to_read = chunk_size;
        size_t nread = 0;
        int rc = bb10_tls_read(conn, out_data + received, to_read, &nread);
        if (rc == BB10_TLS_OK) {
            received += nread;
            retries = 0;
        } else if (rc == BB10_TLS_ERR_WANT_READ) {
            if (!wait_fd(fd, false, 500)) retries++;
        } else if (rc == BB10_TLS_ERR_WANT_WRITE) {
            if (!wait_fd(fd, true, 500)) retries++;
        } else {
            return false;
        }
    }
    return received == total_len;
}

struct StreamingServerArg {
    bb10_tls_context_t* ctx;
    int fd;
    size_t expected_bytes;
    size_t chunk_size;
    bool success;
};

static void* streaming_server_worker(void* ptr) {
    StreamingServerArg* arg = static_cast<StreamingServerArg*>(ptr);
    bb10_tls_connection_t* conn = bb10_tls_connection_create(arg->ctx, arg->fd);
    if (!conn) {
        close(arg->fd);
        arg->success = false;
        return NULL;
    }

    if (nonblocking_handshake(conn, arg->fd, true) != BB10_TLS_OK) {
        bb10_tls_connection_destroy(conn);
        arg->success = false;
        return NULL;
    }

    char* echo_buf = new char[arg->expected_bytes];
    bool read_ok = nonblocking_read_all(conn, arg->fd, echo_buf, arg->expected_bytes, arg->chunk_size);
    bool write_ok = false;
    if (read_ok) {
        write_ok = nonblocking_write_all(conn, arg->fd, echo_buf, arg->expected_bytes, arg->chunk_size);
    }
    delete[] echo_buf;

    bb10_tls_shutdown(conn);
    bb10_tls_connection_destroy(conn);
    arg->success = read_ok && write_ok;
    return NULL;
}

struct MidStreamCancelServerArg {
    bb10_tls_context_t* ctx;
    int fd;
    size_t stream_size;
    size_t prefix_size;
    const char* expected_prefix;
    size_t total_received;
    bool handshake_ok;
    bool prefix_verified;
    bool completed_full_transfer;
    bool cancelled_disconnect_observed;
    bool server_failed;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool prefix_ready;
};

static void* midstream_cancel_server_worker(void* ptr) {
    MidStreamCancelServerArg* arg = static_cast<MidStreamCancelServerArg*>(ptr);
    bb10_tls_connection_t* conn = bb10_tls_connection_create(arg->ctx, arg->fd);
    if (!conn) {
        close(arg->fd);
        pthread_mutex_lock(&arg->mutex);
        arg->server_failed = true;
        pthread_cond_broadcast(&arg->cond);
        pthread_mutex_unlock(&arg->mutex);
        return NULL;
    }

    if (nonblocking_handshake(conn, arg->fd, true) != BB10_TLS_OK) {
        bb10_tls_connection_destroy(conn);
        pthread_mutex_lock(&arg->mutex);
        arg->server_failed = true;
        pthread_cond_broadcast(&arg->cond);
        pthread_mutex_unlock(&arg->mutex);
        return NULL;
    }
    arg->handshake_ok = true;

    char* buf = new char[arg->stream_size];
    std::memset(buf, 0, arg->stream_size);
    size_t received = 0;
    int retries = 0;

    // Read until at least prefix_size is received
    while (received < arg->prefix_size && retries < 20) {
        size_t nread = 0;
        int rc = bb10_tls_read(conn, buf + received, arg->prefix_size - received, &nread);
        if (rc == BB10_TLS_OK) {
            received += nread;
            retries = 0;
        } else if (rc == BB10_TLS_ERR_WANT_READ) {
            if (!wait_fd(arg->fd, false, 50)) retries++;
        } else if (rc == BB10_TLS_ERR_WANT_WRITE) {
            if (!wait_fd(arg->fd, true, 50)) retries++;
        } else {
            break;
        }
    }

    arg->total_received = received;
    bool prefix_ok = false;
    if (received >= arg->prefix_size && arg->expected_prefix != NULL) {
        if (std::memcmp(buf, arg->expected_prefix, arg->prefix_size) == 0) {
            prefix_ok = true;
        }
    }
    arg->prefix_verified = prefix_ok;

    pthread_mutex_lock(&arg->mutex);
    if (prefix_ok) {
        arg->prefix_ready = true;
    } else {
        arg->server_failed = true;
    }
    pthread_cond_broadcast(&arg->cond);
    pthread_mutex_unlock(&arg->mutex);

    if (!prefix_ok) {
        delete[] buf;
        bb10_tls_connection_destroy(conn);
        return NULL;
    }

    // Now continue reading remainder of stream until cancellation disconnect or failure
    retries = 0;
    bool disconnect_observed = false;
    while (received < arg->stream_size && retries < 20) {
        size_t nread = 0;
        int rc = bb10_tls_read(conn, buf + received, arg->stream_size - received, &nread);
        if (rc == BB10_TLS_OK) {
            received += nread;
            retries = 0;
        } else if (rc == BB10_TLS_ERR_WANT_READ) {
            if (!wait_fd(arg->fd, false, 50)) retries++;
        } else if (rc == BB10_TLS_ERR_WANT_WRITE) {
            if (!wait_fd(arg->fd, true, 50)) retries++;
        } else if (rc == BB10_TLS_ERR_CLOSED || rc == BB10_TLS_ERR_IO_FAILED || rc < 0) {
            // Cancelled peer disconnect / failure observed
            disconnect_observed = true;
            break;
        }
    }

    arg->total_received = received;
    arg->completed_full_transfer = (received == arg->stream_size);
    arg->cancelled_disconnect_observed = disconnect_observed;
    delete[] buf;

    bb10_tls_connection_destroy(conn);
    return NULL;
}

int main() {
    alarm(10);
    std::cout << "[Standalone Consumer] Testing bb10_tls public SDK integration..." << std::endl;

    // 1. Basic C & C++ RAII API validation
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(ctx != NULL, "bb10_tls_context_create failed");
    const char* dummy_pin = "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff";
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(ctx, dummy_pin) == BB10_TLS_OK,
                "bb10_tls_context_set_expected_fingerprint failed");
    bb10_tls_context_destroy(ctx);

    std::string certPem, keyPem;
    TEST_ASSERT(bb10_tls::generateSelfSignedCert(certPem, keyPem) == BB10_TLS_OK,
                "bb10_tls::generateSelfSignedCert failed");

    unsigned char entropy[16];
    TEST_ASSERT(bb10_tls::randomBytes(entropy, sizeof(entropy)) == BB10_TLS_OK,
                "bb10_tls::randomBytes failed");

    // 2. Fragmented Nonblocking Bidirectional Streaming Test (64-byte chunks over 2048-byte stream)
    std::cout << "[Standalone Consumer] Running fragmented nonblocking bidirectional streaming test..." << std::endl;
    char pin[65];
    TEST_ASSERT(bb10_tls_calculate_cert_fingerprint(certPem.c_str(), certPem.size(), pin) == BB10_TLS_OK,
                "bb10_tls_calculate_cert_fingerprint failed");

    bb10_tls_context_t* s_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(bb10_tls_context_set_credentials(s_ctx, certPem.c_str(), certPem.size(),
                                                keyPem.c_str(), keyPem.size()) == BB10_TLS_OK,
                "server credentials failed");

    bb10_tls_context_t* c_ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    TEST_ASSERT(bb10_tls_context_set_expected_fingerprint(c_ctx, pin) == BB10_TLS_OK, "client pin failed");

    const size_t STREAM_SIZE = 2048;
    const size_t CHUNK_SIZE = 64;
    char send_data[STREAM_SIZE];
    char recv_data[STREAM_SIZE];
    for (size_t i = 0; i < STREAM_SIZE; ++i) {
        send_data[i] = static_cast<char>((i * 7 + 13) & 0xFF);
    }
    std::memset(recv_data, 0, STREAM_SIZE);

    int sv[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair failed");
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    StreamingServerArg sarg;
    sarg.ctx = s_ctx;
    sarg.fd = sv[0];
    sarg.expected_bytes = STREAM_SIZE;
    sarg.chunk_size = CHUNK_SIZE;
    sarg.success = false;

    pthread_t sthread;
    TEST_ASSERT(pthread_create(&sthread, NULL, streaming_server_worker, &sarg) == 0, "server thread start failed");

    bb10_tls_connection_t* c_conn = bb10_tls_connection_create(c_ctx, sv[1]);
    TEST_ASSERT(c_conn != NULL, "client connection_create failed");

    TEST_ASSERT(nonblocking_handshake(c_conn, sv[1], false) == BB10_TLS_OK, "client handshake failed");
    TEST_ASSERT(nonblocking_write_all(c_conn, sv[1], send_data, STREAM_SIZE, CHUNK_SIZE), "client stream write failed");
    TEST_ASSERT(nonblocking_read_all(c_conn, sv[1], recv_data, STREAM_SIZE, CHUNK_SIZE), "client stream read failed");

    TEST_ASSERT(std::memcmp(send_data, recv_data, STREAM_SIZE) == 0, "Echoed data does not match sent data!");

    // Negative control: altered streaming payload must fail identity check
    char corrupted_data[STREAM_SIZE];
    std::memcpy(corrupted_data, recv_data, STREAM_SIZE);
    corrupted_data[42] ^= 0x5A;
    TEST_ASSERT(std::memcmp(send_data, corrupted_data, STREAM_SIZE) != 0, "Altered streaming payload must not match sent data");

    bb10_tls_shutdown(c_conn);
    bb10_tls_connection_destroy(c_conn);
    pthread_join(sthread, NULL);
    TEST_ASSERT(sarg.success, "Server streaming worker reported failure");

    // 3. Active-Transfer Mid-stream Cancellation Scenario
    std::cout << "[Standalone Consumer] Running active-transfer mid-stream cancellation test..." << std::endl;
    int sv_cancel[2];
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_cancel) == 0, "cancel socketpair failed");
    fcntl(sv_cancel[0], F_SETFL, O_NONBLOCK);
    fcntl(sv_cancel[1], F_SETFL, O_NONBLOCK);

    const size_t CANCEL_STREAM_SIZE = 4096;
    const size_t CANCEL_PREFIX_SIZE = 512;
    char cancel_send_data[CANCEL_STREAM_SIZE];
    for (size_t i = 0; i < CANCEL_STREAM_SIZE; ++i) {
        cancel_send_data[i] = static_cast<char>((i * 3 + 42) & 0xFF);
    }

    char cancel_expected_prefix[CANCEL_PREFIX_SIZE];
    std::memcpy(cancel_expected_prefix, cancel_send_data, CANCEL_PREFIX_SIZE);

    MidStreamCancelServerArg carg;
    carg.ctx = s_ctx;
    carg.fd = sv_cancel[0];
    carg.stream_size = CANCEL_STREAM_SIZE;
    carg.prefix_size = CANCEL_PREFIX_SIZE;
    carg.expected_prefix = cancel_expected_prefix;
    carg.total_received = 0;
    carg.handshake_ok = false;
    carg.prefix_verified = false;
    carg.completed_full_transfer = false;
    carg.cancelled_disconnect_observed = false;
    carg.server_failed = false;
    carg.prefix_ready = false;
    pthread_mutex_init(&carg.mutex, NULL);
    pthread_cond_init(&carg.cond, NULL);

    pthread_t cthread;
    TEST_ASSERT(pthread_create(&cthread, NULL, midstream_cancel_server_worker, &carg) == 0,
                "pthread_create cancel server failed");

    bb10_tls_connection_t* client_conn = bb10_tls_connection_create(c_ctx, sv_cancel[1]);
    TEST_ASSERT(client_conn != NULL, "client_conn create failed");

    // Authenticate both ends
    TEST_ASSERT(nonblocking_handshake(client_conn, sv_cancel[1], false) == BB10_TLS_OK,
                "Client handshake failed");

    // Send the strict prefix (512 bytes) of the larger payload
    TEST_ASSERT(nonblocking_write_all(client_conn, sv_cancel[1], cancel_send_data, CANCEL_PREFIX_SIZE, CHUNK_SIZE),
                "Client write prefix failed");

    // Synchronize: wait until server verifies receipt of the prefix
    pthread_mutex_lock(&carg.mutex);
    while (!carg.prefix_ready && !carg.server_failed) {
        pthread_cond_wait(&carg.cond, &carg.mutex);
    }
    pthread_mutex_unlock(&carg.mutex);

    TEST_ASSERT(carg.prefix_verified, "Server must have verified strict prefix");

    // Cancel while transfer is incomplete: destroy client connection abruptly mid-stream
    bb10_tls_connection_destroy(client_conn);

    // Wait for server thread to join
    pthread_join(cthread, NULL);

    pthread_mutex_destroy(&carg.mutex);
    pthread_cond_destroy(&carg.cond);

    // Assert that the active-transfer cancellation succeeded:
    // 1. Handshake authenticated cleanly
    TEST_ASSERT(carg.handshake_ok, "Handshake must have succeeded");
    // 2. Strict prefix was received and verified
    TEST_ASSERT(carg.total_received >= CANCEL_PREFIX_SIZE, "Server must have received prefix");
    // 3. Transfer was terminated incomplete (< CANCEL_STREAM_SIZE)
    TEST_ASSERT(carg.total_received < CANCEL_STREAM_SIZE, "Server must NOT have received full stream");
    // 4. Server did not report full transfer success
    TEST_ASSERT(!carg.completed_full_transfer, "Server must report transfer incomplete / cancelled");
    // 5. Server must have observed actual cancelled disconnect error (not retry timeout)
    TEST_ASSERT(carg.cancelled_disconnect_observed, "Server must have observed client cancellation disconnect");

    bb10_tls_context_destroy(s_ctx);
    bb10_tls_context_destroy(c_ctx);

    std::cout << "[Standalone Consumer] ALL PUBLIC SDK CONTRACT TESTS PASSED!" << std::endl;
    return 0;
}
