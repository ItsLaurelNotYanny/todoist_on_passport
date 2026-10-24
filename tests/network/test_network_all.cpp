#include "todoist/network/curl_sync_transport.hpp"
#include "todoist/network/sync_notification_queue.hpp"
#include "todoist/network/sync_worker_thread.hpp"
#include "todoist/sync/sync_engine.hpp"
#include "todoist/db_manager.hpp"
#include "todoist/task_repository.hpp"
#include "todoist/sync_command_queue.hpp"
#include "todoist/temp_id_reconciler.hpp"
#include "todoist/sync_metadata_repo.hpp"
#include "todoist/uuid_util.hpp"

#include <iostream>
#include <sstream>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

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
// Mock SyncTransport for Worker Thread Tests
// ---------------------------------------------------------------------------
class MockSyncTransport : public todoist::ISyncTransport {
public:
    MockSyncTransport()
        : m_callCount(0)
        , m_nextStatusCode(200)
        , m_nextBody("{\"sync_token\":\"next_tok\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}")
        , m_nextError("")
    {}

    virtual HttpResponse executeSyncPost(const std::string& /*endpointUrl*/,
                                        const std::string& /*requestBody*/,
                                        const std::string& /*bearerToken*/) {
        m_callCount++;
        return HttpResponse(m_nextStatusCode, m_nextBody, m_nextError);
    }

    int m_callCount;
    int m_nextStatusCode;
    std::string m_nextBody;
    std::string m_nextError;
};

struct TestEnv {
    std::string dbPath;
    todoist::DatabaseManager db;
    todoist::TaskRepository taskRepo;
    todoist::SyncCommandQueue cmdQueue;
    todoist::TempIdReconciler reconciler;
    todoist::SyncMetadataRepository metaRepo;
    todoist::SyncEngine engine;

    explicit TestEnv(const std::string& name)
        : dbPath("/tmp/todoist_net_test_" + name + "_" + todoist::util::generateUuidV4() + ".db")
        , db()
        , taskRepo(db)
        , cmdQueue(db)
        , reconciler(db)
        , metaRepo(db, taskRepo, cmdQueue)
        , engine(db, taskRepo, cmdQueue, reconciler, metaRepo)
    {
        unlink(dbPath.c_str());
        unlink((dbPath + "-wal").c_str());
        unlink((dbPath + "-shm").c_str());
        bool opened = db.open(dbPath);
        assert(opened);
        bool init = db.initializeSchema();
        assert(init);
        bool eng_init = engine.initialize();
        assert(eng_init);
    }

    ~TestEnv() {
        unlink(dbPath.c_str());
        unlink((dbPath + "-wal").c_str());
        unlink((dbPath + "-shm").c_str());
    }
};

// ---------------------------------------------------------------------------
// Test 1: SyncNotificationQueue Value Decoupling & Thread-Safety
// ---------------------------------------------------------------------------
static void test_notification_queue() {
    todoist::SyncNotificationQueue queue;
    TEST_ASSERT(queue.empty(), "Queue must initially be empty");
    TEST_ASSERT(queue.size() == 0, "Queue size must be 0");

    // Push events
    todoist::SyncEventSnapshot s1(todoist::WORKER_STATE_SYNCING, 0, "Syncing", 1000, 0);
    todoist::SyncEventSnapshot s2(todoist::WORKER_STATE_SUCCESS, 200, "Success", 1005, 5);
    queue.push(s1);
    queue.push(s2);

    TEST_ASSERT(queue.size() == 2, "Queue size should be 2");
    TEST_ASSERT(!queue.empty(), "Queue should not be empty");

    // Drain notifications into value vector
    std::vector<todoist::SyncEventSnapshot> drained;
    queue.drainNotifications(drained);

    TEST_ASSERT(queue.empty(), "Queue must be empty after drain");
    TEST_ASSERT(drained.size() == 2, "Drained vector must contain 2 events");
    TEST_ASSERT(drained[0].state == todoist::WORKER_STATE_SYNCING, "Event 0 state mismatch");
    TEST_ASSERT(drained[1].state == todoist::WORKER_STATE_SUCCESS, "Event 1 state mismatch");
    TEST_ASSERT(drained[1].itemsProcessed == 5, "Event 1 items mismatch");

    // Ceasing to call drainNotifications does not leak or crash (UI destruction safety)
    queue.push(s1);
}

// ---------------------------------------------------------------------------
// Test 2: Saturating Full Jitter Backoff Formula
// ---------------------------------------------------------------------------
static void test_backoff_calculation() {
    TestEnv env("backoff");
    MockSyncTransport transport;
    todoist::SyncNotificationQueue queue;

    todoist::SyncWorkerThread worker(env.engine, transport, queue);

    for (int attempt = 0; attempt <= 15; ++attempt) {
        double backoff = worker.calculateBackoff(attempt);
        TEST_ASSERT(backoff >= 0.0, "Backoff cannot be negative");
        TEST_ASSERT(backoff <= 60.0, "Backoff must saturate at 60.0s ceiling");
    }
}

// ---------------------------------------------------------------------------
// Test 3: Worker Thread Lifecycle & Clean Join
// ---------------------------------------------------------------------------
static void test_worker_lifecycle() {
    TestEnv env("lifecycle");
    MockSyncTransport transport;
    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);

    TEST_ASSERT(worker.start(), "Worker start failed");
    TEST_ASSERT(worker.getState() == todoist::WORKER_STATE_IDLE, "Initial state should be IDLE");

    // Stop and join cleanly
    worker.stop();
    TEST_ASSERT(worker.getState() == todoist::WORKER_STATE_IDLE, "Final state should be IDLE");
}

// ---------------------------------------------------------------------------
// Test 4: Worker Immediate Sync Request & Success Flow
// ---------------------------------------------------------------------------
static void test_worker_sync_request_success() {
    TestEnv env("success");
    MockSyncTransport transport;
    transport.m_nextStatusCode = 200;
    transport.m_nextBody = "{\"sync_token\":\"tok_123\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}";

    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);
    TEST_ASSERT(worker.start(), "Worker start failed");

    worker.requestSync();

    // Wait briefly for worker to process cycle
    for (int i = 0; i < 50; ++i) {
        if (transport.m_callCount > 0 && worker.getState() == todoist::WORKER_STATE_IDLE) {
            break;
        }
        usleep(10000); // 10ms
    }

    TEST_ASSERT(transport.m_callCount >= 1, "Transport should have been called at least once");
    TEST_ASSERT(worker.getConsecutiveFailures() == 0, "Failures should be 0 on success");

    std::vector<todoist::SyncEventSnapshot> events;
    queue.drainNotifications(events);
    TEST_ASSERT(!events.empty(), "Events should have been recorded");

    bool saw_success = false;
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].state == todoist::WORKER_STATE_SUCCESS) {
            saw_success = true;
        }
    }
    TEST_ASSERT(saw_success, "Expected WORKER_STATE_SUCCESS snapshot");

    worker.stop();
}

// ---------------------------------------------------------------------------
// Test 5: Terminal Error (HTTP 401) Non-Spinning Halt
// ---------------------------------------------------------------------------
static void test_worker_terminal_error_halt() {
    TestEnv env("term_err");
    MockSyncTransport transport;
    transport.m_nextStatusCode = 401;
    transport.m_nextBody = "{\"error\":\"Unauthorized\",\"error_code\":401}";

    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);
    TEST_ASSERT(worker.start(), "Worker start failed");

    worker.requestSync();

    for (int i = 0; i < 50; ++i) {
        if (transport.m_callCount > 0 && worker.getState() == todoist::WORKER_STATE_FATAL_ERROR) {
            break;
        }
        usleep(10000);
    }

    TEST_ASSERT(worker.getState() == todoist::WORKER_STATE_FATAL_ERROR, "Worker must enter FATAL_ERROR state");
    int calls_after_fatal = transport.m_callCount;

    // Sleep for 50ms and assert NO spinning retries occurred
    usleep(50000);
    TEST_ASSERT(transport.m_callCount == calls_after_fatal, "Terminal error must halt retry loop without spinning");

    worker.stop();
}

// ---------------------------------------------------------------------------
// Test 6: Transient Error Backoff & Reachability Interruption
// ---------------------------------------------------------------------------
static void test_worker_transient_error_reachability_interruption() {
    TestEnv env("transient");
    MockSyncTransport transport;
    transport.m_nextStatusCode = 503; // Server unavailable (transient)
    transport.m_nextBody = "Service Unavailable";

    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);
    TEST_ASSERT(worker.start(), "Worker start failed");

    worker.requestSync();

    for (int i = 0; i < 50; ++i) {
        if (transport.m_callCount > 0 && worker.getState() == todoist::WORKER_STATE_BACKOFF_WAIT) {
            break;
        }
        usleep(10000);
    }

    TEST_ASSERT(worker.getState() == todoist::WORKER_STATE_BACKOFF_WAIT, "Worker should be in BACKOFF_WAIT");
    TEST_ASSERT(worker.getConsecutiveFailures() == 1, "Consecutive failures should be 1");

    int calls_before_reachability = transport.m_callCount;

    // Now mock network reachability recovery: should interrupt backoff immediately!
    transport.m_nextStatusCode = 200;
    transport.m_nextBody = "{\"sync_token\":\"tok_rec\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}";
    worker.notifyReachabilityChanged(true);

    for (int i = 0; i < 50; ++i) {
        if (transport.m_callCount > calls_before_reachability && worker.getState() == todoist::WORKER_STATE_IDLE) {
            break;
        }
        usleep(10000);
    }

    TEST_ASSERT(transport.m_callCount > calls_before_reachability, "Reachability signal must wake and retry");
    TEST_ASSERT(worker.getConsecutiveFailures() == 0, "Failures reset to 0 after recovery");

    worker.stop();
}

// ---------------------------------------------------------------------------
// Test 7: CurlSyncTransport Cancellation Hook
// ---------------------------------------------------------------------------
static void test_curl_transport_cancellation_flag() {
    todoist::CurlSyncTransport transport("vendor/sources/cacert.pem");

    todoist::CancellationToken cancelToken;
    cancelToken.cancel();
    transport.setCancellationToken(&cancelToken);

    // Any request performed while cancellation token is cancelled should abort immediately
    todoist::ISyncTransport::HttpResponse resp = transport.executeSyncPost(
        "https://127.0.0.1:54321/dummy", "", "");

    TEST_ASSERT(resp.statusCode == 0, "Aborted request should return status 0");
    TEST_ASSERT(resp.errorMessage.find("aborted") != std::string::npos ||
                resp.errorMessage.find("Callback") != std::string::npos ||
                resp.errorMessage.find("callback") != std::string::npos,
                "Error message should indicate aborted by callback");
}

// ---------------------------------------------------------------------------
// Test 8: Worker Offline During Backoff Wait (M3-R005)
// ---------------------------------------------------------------------------
static void test_worker_offline_during_backoff() {
    TestEnv env("worker_offline_backoff");
    MockSyncTransport transport;
    transport.m_nextStatusCode = 500;
    transport.m_nextBody = "Server Error";

    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);

    TEST_ASSERT(worker.start(), "Worker start failed");
    worker.requestSync();

    // Wait until worker enters backoff wait
    bool backoff = false;
    for (int i = 0; i < 50 && !backoff; ++i) {
        if (worker.getState() == todoist::WORKER_STATE_BACKOFF_WAIT) {
            backoff = true;
        }
        usleep(10000);
    }
    TEST_ASSERT(backoff, "Worker should be in BACKOFF_WAIT");

    int calls_before_offline = transport.m_callCount;
    // Notify offline while worker is waiting in backoff
    worker.notifyReachabilityChanged(false);

    // Sleep for longer than the initial backoff interval (1.0 sec)
    usleep(1500000); // 1.5 seconds

    // Transport call count must NOT have increased while offline!
    TEST_ASSERT(transport.m_callCount == calls_before_offline,
                "Worker must not execute transport requests while reachability is false");

    worker.stop();
}

// ---------------------------------------------------------------------------
// Test 9: Worker Offline Between Backlog Batches (M3-R005)
// ---------------------------------------------------------------------------
class BatchCountingTransport : public todoist::ISyncTransport {
public:
    BatchCountingTransport() : m_callCount(0), m_worker(NULL) {}
    int m_callCount;
    todoist::SyncWorkerThread* m_worker;

    virtual HttpResponse executeSyncPost(const std::string&, const std::string&, const std::string&) {
        m_callCount++;
        if (m_callCount == 1 && m_worker) {
            // Drop reachability right as first batch finishes
            m_worker->notifyReachabilityChanged(false);
        }
        // Return successful batch response acknowledging commands
        return HttpResponse(200, "{\"sync_token\":\"tok_batch\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{\"cmd1\":\"ok\"}}", "");
    }
};

static void test_worker_offline_between_batches() {
    TestEnv env("worker_offline_batches");
    env.engine.setMaxBatchSize(1);

    BatchCountingTransport transport;
    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);
    transport.m_worker = &worker;

    // Enqueue two separate task commands locally
    todoist::Task t1;
    t1.content = "Task 1";
    env.taskRepo.createTaskLocally(t1, "cmd-add-1");

    todoist::Task t2;
    t2.content = "Task 2";
    env.taskRepo.createTaskLocally(t2, "cmd-add-2");

    TEST_ASSERT(worker.start(), "Worker start failed");
    worker.requestSync();

    usleep(200000); // Allow first batch to complete

    // Because reachability went false during/after first batch, second batch must not execute yet
    TEST_ASSERT(transport.m_callCount == 1, "Only first batch should execute before reachability drops");

    // Recover reachability
    transport.m_worker = NULL;
    worker.notifyReachabilityChanged(true);

    for (int i = 0; i < 50; ++i) {
        if (transport.m_callCount >= 2) break;
        usleep(10000);
    }
    TEST_ASSERT(transport.m_callCount >= 2, "Second batch should execute once reachability recovers");

    worker.stop();
}

// ---------------------------------------------------------------------------
// Test 10: Synchronized CancellationToken with CurlSyncTransport (M3-R004)
// ---------------------------------------------------------------------------
static void test_synchronized_cancellation_token() {
    todoist::CancellationToken token;
    TEST_ASSERT(!token.isCancelled(), "Fresh token should not be cancelled");

    token.cancel();
    TEST_ASSERT(token.isCancelled(), "Token should be cancelled after cancel()");

    token.reset();
    TEST_ASSERT(!token.isCancelled(), "Token should not be cancelled after reset()");

    // Wire to CurlSyncTransport
    todoist::CurlSyncTransport transport("vendor/sources/cacert.pem");
    transport.setCancellationToken(&token);

    token.cancel();
    todoist::ISyncTransport::HttpResponse resp1 = transport.executeSyncPost(
        "https://127.0.0.1:54321/dummy", "", "");
    TEST_ASSERT(resp1.statusCode == 0, "Cancelled token must abort request with status 0");

    // Reset token and reuse transport
    token.reset();
    // Non-cancelled request to non-existent server will fail with connect error, not callback abort
    todoist::ISyncTransport::HttpResponse resp2 = transport.executeSyncPost(
        "https://127.0.0.1:54321/dummy", "", "");
    TEST_ASSERT(resp2.errorMessage.find("aborted by an application callback") == std::string::npos,
                "Reset token should allow transfer attempt without premature callback abort");
}

// ---------------------------------------------------------------------------
// Test 11: Real Curl/Worker Lost-Response Cursor Durability & UUID Reuse (M3-R013)
// ---------------------------------------------------------------------------
struct LostResponseServerArg {
    int listen_fd;
    SSL_CTX* ssl_ctx;
    std::string temp_id;
    std::string req1_body;
    std::string req2_body;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int phase; // 0 = waiting req 1, 1 = req 1 reset done, 2 = req 2 200 done
    volatile bool stop;
};

static bool read_https_request(SSL* ssl, std::string& headers, std::string& body) {
    char buf[1024];
    std::string raw;
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    while ((header_end = raw.find("\r\n\r\n")) == std::string::npos) {
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) return false;
        raw.append(buf, static_cast<size_t>(n));
    }

    headers = raw.substr(0, header_end);
    std::string rest = raw.substr(header_end + 4);

    size_t cl_pos = headers.find("Content-Length: ");
    if (cl_pos == std::string::npos) {
        cl_pos = headers.find("content-length: ");
    }
    if (cl_pos != std::string::npos) {
        size_t val_start = cl_pos + 16;
        size_t val_end = headers.find("\r\n", val_start);
        if (val_end != std::string::npos) {
            content_length = static_cast<size_t>(std::atol(headers.substr(val_start, val_end - val_start).c_str()));
        }
    }

    while (rest.size() < content_length) {
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) break;
        rest.append(buf, static_cast<size_t>(n));
    }

    body = rest;
    return true;
}

static void* lost_response_server_worker(void* ptr) {
    LostResponseServerArg* arg = static_cast<LostResponseServerArg*>(ptr);

    // Phase 1: Accept connection 1, TLS handshake, read request, and abruptly reset TCP connection
    int cfd1 = accept(arg->listen_fd, NULL, NULL);
    if (cfd1 >= 0) {
        SSL* ssl1 = SSL_new(arg->ssl_ctx);
        SSL_set_fd(ssl1, cfd1);
        if (SSL_accept(ssl1) > 0) {
            std::string headers, body;
            if (read_https_request(ssl1, headers, body)) {
                pthread_mutex_lock(&arg->mutex);
                arg->req1_body = body;
                pthread_mutex_unlock(&arg->mutex);
            }
        }
        SSL_free(ssl1);
        struct linger sl;
        sl.l_onoff = 1;
        sl.l_linger = 0;
        setsockopt(cfd1, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
        close(cfd1);

        pthread_mutex_lock(&arg->mutex);
        arg->phase = 1;
        pthread_cond_broadcast(&arg->cond);
        pthread_mutex_unlock(&arg->mutex);
    }

    // Phase 2: Accept retry connection 2, TLS handshake, read request, and send 200 OK acknowledgement
    int cfd2 = accept(arg->listen_fd, NULL, NULL);
    if (cfd2 >= 0) {
        SSL* ssl2 = SSL_new(arg->ssl_ctx);
        SSL_set_fd(ssl2, cfd2);
        if (SSL_accept(ssl2) > 0) {
            std::string headers, body;
            if (read_https_request(ssl2, headers, body)) {
                pthread_mutex_lock(&arg->mutex);
                arg->req2_body = body;
                pthread_mutex_unlock(&arg->mutex);
            }

            std::string jsonBody =
                "{\"sync_token\":\"tok_cursor_after_ack\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{\"" +
                arg->temp_id + "\":\"server_id_100\"},\"sync_status\":{\"cmd-lost-1\":\"ok\"}}";

            std::ostringstream respOss;
            respOss << "HTTP/1.1 200 OK\r\n"
                    << "Content-Type: application/json\r\n"
                    << "Content-Length: " << jsonBody.size() << "\r\n"
                    << "Connection: close\r\n"
                    << "\r\n"
                    << jsonBody;
            std::string resp = respOss.str();

            SSL_write(ssl2, resp.data(), static_cast<int>(resp.size()));
            SSL_shutdown(ssl2);
        }
        SSL_free(ssl2);
        close(cfd2);

        pthread_mutex_lock(&arg->mutex);
        arg->phase = 2;
        pthread_cond_broadcast(&arg->cond);
        pthread_mutex_unlock(&arg->mutex);
    }

    // Subsequent queue drain / poll requests: respond 200 OK with empty delta until listener is stopped
    while (!arg->stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(arg->listen_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        int s = select(arg->listen_fd + 1, &fds, NULL, NULL, &tv);
        if (s <= 0) {
            continue;
        }
        int cfd_sub = accept(arg->listen_fd, NULL, NULL);
        if (cfd_sub < 0) {
            break;
        }
        SSL* ssl_sub = SSL_new(arg->ssl_ctx);
        SSL_set_fd(ssl_sub, cfd_sub);
        if (SSL_accept(ssl_sub) > 0) {
            std::string h, b;
            read_https_request(ssl_sub, h, b);
            std::string pollBody =
                "{\"sync_token\":\"tok_cursor_after_ack\",\"full_sync\":false,\"items\":[],\"temp_id_mapping\":{},\"sync_status\":{}}";
            std::ostringstream pollOss;
            pollOss << "HTTP/1.1 200 OK\r\n"
                    << "Content-Type: application/json\r\n"
                    << "Content-Length: " << pollBody.size() << "\r\n"
                    << "Connection: close\r\n\r\n"
                    << pollBody;
            std::string pollResp = pollOss.str();
            SSL_write(ssl_sub, pollResp.data(), static_cast<int>(pollResp.size()));
            SSL_shutdown(ssl_sub);
        }
        SSL_free(ssl_sub);
        close(cfd_sub);
    }

    return NULL;
}

static void test_worker_lost_response_cursor_durability() {
    // Generate self-signed certificate with IP SAN 127.0.0.1 for loopback HTTPS
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    TEST_ASSERT(pkey != NULL, "EVP_EC_gen failed");
    X509* cert = X509_new();
    TEST_ASSERT(cert != NULL, "X509_new failed");
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 77777);
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
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &v3_ctx, NID_subject_alt_name, const_cast<char*>("IP:127.0.0.1"));
    TEST_ASSERT(ext != NULL, "X509V3_EXT_conf_nid failed");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);

    TEST_ASSERT(X509_sign(cert, pkey, EVP_sha256()) > 0, "X509_sign failed");

    std::string ca_path = "/tmp/test_lost_resp_ca_" + todoist::util::generateUuidV4() + ".pem";
    FILE* fp = fopen(ca_path.c_str(), "wb");
    TEST_ASSERT(fp != NULL, "fopen CA file failed");
    PEM_write_X509(fp, cert);
    fclose(fp);

    SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
    TEST_ASSERT(ssl_ctx != NULL, "SSL_CTX_new failed");
    TEST_ASSERT(SSL_CTX_use_certificate(ssl_ctx, cert) == 1, "use_certificate failed");
    TEST_ASSERT(SSL_CTX_use_PrivateKey(ssl_ctx, pkey) == 1, "use_PrivateKey failed");

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(listen_fd >= 0, "socket failed");
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    TEST_ASSERT(bind(listen_fd, (struct sockaddr*)&sin, sizeof(sin)) == 0, "bind failed");
    TEST_ASSERT(listen(listen_fd, 5) == 0, "listen failed");

    socklen_t slen = sizeof(sin);
    TEST_ASSERT(getsockname(listen_fd, (struct sockaddr*)&sin, &slen) == 0, "getsockname failed");
    int port = ntohs(sin.sin_port);
    std::ostringstream urlOss;
    urlOss << "https://127.0.0.1:" << port << "/sync";
    std::string endpointUrl = urlOss.str();

    TestEnv env("worker_lost_response");
    env.engine.setEndpointUrl(endpointUrl);
    env.metaRepo.setMetadata("sync_token", "tok_durable_cursor_001");
    TEST_ASSERT(env.metaRepo.getSyncToken() == "tok_durable_cursor_001", "Initial sync token");

    todoist::Task t;
    t.content = "Durable cursor task";
    env.taskRepo.createTaskLocally(t, "cmd-lost-1");

    std::vector<todoist::SyncCommand> initialCmds;
    TEST_ASSERT(env.cmdQueue.getPendingCommands(initialCmds) && initialCmds.size() == 1,
                "Expected 1 pending command");
    std::string expected_uuid = initialCmds[0].uuid;
    std::string expected_args = initialCmds[0].args_json;
    std::string expected_temp_id = initialCmds[0].temp_id;

    LostResponseServerArg sarg;
    sarg.listen_fd = listen_fd;
    sarg.ssl_ctx = ssl_ctx;
    sarg.temp_id = expected_temp_id;
    sarg.phase = 0;
    sarg.stop = false;
    pthread_mutex_init(&sarg.mutex, NULL);
    pthread_cond_init(&sarg.cond, NULL);

    pthread_t st;
    TEST_ASSERT(pthread_create(&st, NULL, lost_response_server_worker, &sarg) == 0, "pthread_create failed");

    todoist::CurlSyncTransport transport(ca_path);
    todoist::SyncNotificationQueue queue;
    todoist::SyncWorkerThread worker(env.engine, transport, queue);

    TEST_ASSERT(worker.start(), "Worker start failed");
    worker.requestSync();

    // Wait until server handles Phase 1 (req 1 dropped via TCP reset)
    pthread_mutex_lock(&sarg.mutex);
    while (sarg.phase < 1) {
        pthread_cond_wait(&sarg.cond, &sarg.mutex);
    }
    pthread_mutex_unlock(&sarg.mutex);

    // Wait until worker observes transport failure and enters BACKOFF_WAIT
    bool in_backoff = false;
    for (int i = 0; i < 50; ++i) {
        if (worker.getState() == todoist::WORKER_STATE_BACKOFF_WAIT) {
            in_backoff = true;
            break;
        }
        usleep(10000);
    }
    TEST_ASSERT(in_backoff, "Worker must enter BACKOFF_WAIT after lost transport response");

    // Verify durable SQLite state after lost response:
    // 1. Cursor MUST NOT advance
    TEST_ASSERT(env.metaRepo.getSyncToken() == "tok_durable_cursor_001",
                "Durable cursor must remain unchanged across lost transport response");
    // 2. Command queue MUST retain pending command with identical UUID and payload
    std::vector<todoist::SyncCommand> afterDropCmds;
    TEST_ASSERT(env.cmdQueue.getPendingCommands(afterDropCmds) && afterDropCmds.size() == 1,
                "Pending command must be retained after lost response");
    TEST_ASSERT(afterDropCmds[0].uuid == expected_uuid, "Command UUID must be preserved");
    TEST_ASSERT(afterDropCmds[0].args_json == expected_args, "Command payload must be preserved");

    // Wake up worker for immediate retry
    worker.notifyReachabilityChanged(true);

    // Wait until server handles Phase 2 (req 2 accepted and 200 OK returned)
    pthread_mutex_lock(&sarg.mutex);
    while (sarg.phase < 2) {
        pthread_cond_wait(&sarg.cond, &sarg.mutex);
    }
    pthread_mutex_unlock(&sarg.mutex);

    // Wait until worker finishes processing 200 OK and returns to IDLE
    bool in_idle = false;
    for (int i = 0; i < 200; ++i) {
        if (worker.getState() == todoist::WORKER_STATE_IDLE) {
            in_idle = true;
            break;
        }
        usleep(10000);
    }
    TEST_ASSERT(in_idle, "Worker must return to IDLE after successful retry acknowledgement");

    // Verify durable SQLite state after acknowledgement:
    // 1. Cursor MUST advance to acknowledged token
    TEST_ASSERT(env.metaRepo.getSyncToken() == "tok_cursor_after_ack",
                "Durable cursor must advance after successful acknowledgement");
    // 2. Command queue MUST be cleared
    std::vector<todoist::SyncCommand> finalCmds;
    TEST_ASSERT(env.cmdQueue.getPendingCommands(finalCmds) && finalCmds.empty(),
                "Acknowledged command must be removed from queue");

    // Validate request payload retention and exact reuse across retries
    pthread_mutex_lock(&sarg.mutex);
    std::string req1 = sarg.req1_body;
    std::string req2 = sarg.req2_body;
    pthread_mutex_unlock(&sarg.mutex);

    TEST_ASSERT(!req1.empty() && !req2.empty(), "Both requests must have non-empty bodies");
    TEST_ASSERT(req1.find(expected_uuid) != std::string::npos, "Request 1 must contain original command UUID");
    TEST_ASSERT(req2.find(expected_uuid) != std::string::npos, "Request 2 must reuse exact same command UUID");
    TEST_ASSERT(req1.find("tok_durable_cursor_001") != std::string::npos, "Request 1 must contain durable cursor");
    TEST_ASSERT(req2.find("tok_durable_cursor_001") != std::string::npos, "Request 2 must retain exact durable cursor");
    TEST_ASSERT(req1 == req2, "Retried request body must be byte-for-byte identical to dropped request");

    // Negative controls: verify detection of mutated UUID, payload, or cursor
    std::string mutated_uuid = expected_uuid + "_mutated";
    TEST_ASSERT(req2.find(mutated_uuid) == std::string::npos,
                "Negative control: Mutated UUID must not appear in retried request");
    std::string mutated_payload = "\"Durable cursor task altered\"";
    TEST_ASSERT(req2.find(mutated_payload) == std::string::npos,
                "Negative control: Altered payload must not appear in retried request");
    std::string mutated_cursor = "tok_corrupted_cursor_999";
    TEST_ASSERT(req2.find(mutated_cursor) == std::string::npos,
                "Negative control: Altered cursor must not appear in retried request");

    worker.stop();
    sarg.stop = true;
    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    pthread_join(st, NULL);
    SSL_CTX_free(ssl_ctx);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    unlink(ca_path.c_str());
    pthread_mutex_destroy(&sarg.mutex);
    pthread_cond_destroy(&sarg.cond);
}

// ---------------------------------------------------------------------------
// Main test entrypoint
// ---------------------------------------------------------------------------
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running todoist_network Unit Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    run_test(test_notification_queue, "test_notification_queue");
    run_test(test_backoff_calculation, "test_backoff_calculation");
    run_test(test_worker_lifecycle, "test_worker_lifecycle");
    run_test(test_worker_sync_request_success, "test_worker_sync_request_success");
    run_test(test_worker_terminal_error_halt, "test_worker_terminal_error_halt");
    run_test(test_worker_transient_error_reachability_interruption, "test_worker_transient_error_reachability_interruption");
    run_test(test_curl_transport_cancellation_flag, "test_curl_transport_cancellation_flag");
    run_test(test_worker_offline_during_backoff, "test_worker_offline_during_backoff");
    run_test(test_worker_offline_between_batches, "test_worker_offline_between_batches");
    run_test(test_synchronized_cancellation_token, "test_synchronized_cancellation_token");
    run_test(test_worker_lost_response_cursor_durability, "test_worker_lost_response_cursor_durability");

    std::cout << "========================================" << std::endl;
    std::cout << " All " << g_test_count << " todoist_network unit tests PASSED!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
