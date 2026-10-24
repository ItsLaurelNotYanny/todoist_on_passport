#include "todoist/ui/todoist_app_controller.hpp"
#include "todoist/uuid_util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdio.h>
#include <cstdlib>
#include <ctime>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

namespace todoist {

namespace {

static bool sha256Bytes(const unsigned char* data1, size_t len1,
                        const unsigned char* data2, size_t len2,
                        unsigned char* out32) {
    EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();
    if (!mdCtx) return false;
    bool ok = (EVP_DigestInit_ex(mdCtx, EVP_sha256(), NULL) == 1);
    if (ok && data1 && len1 > 0) {
        ok = (EVP_DigestUpdate(mdCtx, data1, len1) == 1);
    }
    if (ok && data2 && len2 > 0) {
        ok = (EVP_DigestUpdate(mdCtx, data2, len2) == 1);
    }
    if (ok) {
        ok = (EVP_DigestFinal_ex(mdCtx, out32, NULL) == 1);
    }
    EVP_MD_CTX_free(mdCtx);
    return ok;
}

bool getMasterKey(unsigned char* outKey32) {
    if (!outKey32) return false;
    std::string path;
    const char* envPath = getenv("TODOIST_KEYSTORE_PATH");
    if (envPath && *envPath) {
        path = envPath;
    } else {
        const char* home = getenv("HOME");
        if (home && *home) {
            path = std::string(home) + "/.todoist_keystore.key";
        } else {
            return false; // Fail closed if private storage unavailable; no global /tmp fallback!
        }
    }

    unsigned char secret[32];
    bool haveSecret = false;
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 &&
            S_ISREG(st.st_mode) &&
            st.st_uid == geteuid() &&
            (st.st_mode & 0077) == 0 &&
            st.st_size == 32) {
            ssize_t rd = read(fd, secret, sizeof(secret));
            close(fd);
            if (rd == static_cast<ssize_t>(sizeof(secret))) {
                haveSecret = true;
            }
        } else {
            close(fd);
            return false; // Reject insecure or invalid existing key file!
        }
    }

    if (!haveSecret) {
        if (RAND_bytes(secret, sizeof(secret)) != 1) {
            return false;
        }
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            if (errno == EEXIST) {
                fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
                if (fd >= 0) {
                    struct stat st;
                    if (fstat(fd, &st) == 0 &&
                        S_ISREG(st.st_mode) &&
                        st.st_uid == geteuid() &&
                        (st.st_mode & 0077) == 0 &&
                        st.st_size == 32) {
                        ssize_t rd = read(fd, secret, sizeof(secret));
                        close(fd);
                        if (rd == static_cast<ssize_t>(sizeof(secret))) {
                            haveSecret = true;
                        }
                    } else {
                        close(fd);
                        return false;
                    }
                }
            }
            if (!haveSecret) {
                return false;
            }
        } else {
            struct stat st;
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) != 0) {
                close(fd);
                unlink(path.c_str());
                return false;
            }
            ssize_t wr = write(fd, secret, sizeof(secret));
            close(fd);
            if (wr != static_cast<ssize_t>(sizeof(secret))) {
                unlink(path.c_str());
                return false;
            }
            haveSecret = true;
        }
    }

    const char* salt = "com.todoist.passport.keystore.v1";
    return sha256Bytes(secret, sizeof(secret), reinterpret_cast<const unsigned char*>(salt), std::strlen(salt), outKey32);
}

static bool aesEncrypt(const unsigned char* key, const unsigned char* iv,
                       const unsigned char* plain, int plainLen,
                       std::vector<unsigned char>& cipherOut) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    cipherOut.resize(plainLen + 32);
    int outLen1 = 0, outLen2 = 0;
    if (EVP_EncryptUpdate(ctx, &cipherOut[0], &outLen1, plain, plainLen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    if (EVP_EncryptFinal_ex(ctx, &cipherOut[0] + outLen1, &outLen2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    EVP_CIPHER_CTX_free(ctx);
    cipherOut.resize(outLen1 + outLen2);
    return true;
}

static bool aesDecrypt(const unsigned char* key, const unsigned char* iv,
                       const unsigned char* cipher, int cipherLen,
                       std::vector<unsigned char>& plainOut) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    plainOut.resize(cipherLen + 32);
    int outLen1 = 0, outLen2 = 0;
    if (EVP_DecryptUpdate(ctx, &plainOut[0], &outLen1, cipher, cipherLen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    if (EVP_DecryptFinal_ex(ctx, &plainOut[0] + outLen1, &outLen2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    EVP_CIPHER_CTX_free(ctx);
    plainOut.resize(outLen1 + outLen2);
    return true;
}

std::string encryptToken(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    unsigned char masterKey[32];
    if (!getMasterKey(masterKey)) return "";

    unsigned char innerIv[16];
    if (RAND_bytes(innerIv, sizeof(innerIv)) != 1) {
        return "";
    }

    std::vector<unsigned char> innerCipher;
    if (!aesEncrypt(masterKey, innerIv,
                    reinterpret_cast<const unsigned char*>(plaintext.data()),
                    static_cast<int>(plaintext.size()),
                    innerCipher)) {
        return "";
    }

    std::vector<unsigned char> outerPlain(sizeof(innerIv) + innerCipher.size());
    std::memcpy(&outerPlain[0], innerIv, sizeof(innerIv));
    std::memcpy(&outerPlain[sizeof(innerIv)], &innerCipher[0], innerCipher.size());

    unsigned char packageKey[32];
    const char* salt = "com.todoist.passport.keystore.v1";
    if (!sha256Bytes(reinterpret_cast<const unsigned char*>(salt), std::strlen(salt), NULL, 0, packageKey)) {
        return "";
    }

    unsigned char outerIv[16];
    if (RAND_bytes(outerIv, sizeof(outerIv)) != 1) {
        return "";
    }

    std::vector<unsigned char> outerCipher;
    if (!aesEncrypt(packageKey, outerIv, &outerPlain[0], static_cast<int>(outerPlain.size()), outerCipher)) {
        return "";
    }

    std::string res = "enc:v1:";
    for (size_t i = 0; i < sizeof(outerIv); ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", outerIv[i]);
        res += buf;
    }
    res += ":";
    for (size_t i = 0; i < outerCipher.size(); ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", outerCipher[i]);
        res += buf;
    }
    return res;
}

std::string decryptToken(const std::string& stored) {
    if (stored.empty()) return "";
    if (stored.find("enc:v1:") != 0) {
        return stored;
    }
    std::string body = stored.substr(7);
    size_t colon = body.find(':');
    if (colon == std::string::npos || colon != 32) {
        return "";
    }
    std::string hexIv = body.substr(0, 32);
    std::string hexCipher = body.substr(33);
    if (hexCipher.empty() || hexCipher.size() % 2 != 0) {
        return "";
    }

    unsigned char outerIv[16];
    for (size_t i = 0; i < 16; ++i) {
        unsigned int byteVal = 0;
        if (sscanf(hexIv.substr(i * 2, 2).c_str(), "%02x", &byteVal) != 1) return "";
        outerIv[i] = static_cast<unsigned char>(byteVal);
    }

    size_t cipherLen = hexCipher.size() / 2;
    std::vector<unsigned char> outerCipher(cipherLen);
    for (size_t i = 0; i < cipherLen; ++i) {
        unsigned int byteVal = 0;
        if (sscanf(hexCipher.substr(i * 2, 2).c_str(), "%02x", &byteVal) != 1) return "";
        outerCipher[i] = static_cast<unsigned char>(byteVal);
    }

    unsigned char packageKey[32];
    const char* salt = "com.todoist.passport.keystore.v1";
    if (!sha256Bytes(reinterpret_cast<const unsigned char*>(salt), std::strlen(salt), NULL, 0, packageKey)) {
        return "";
    }

    std::vector<unsigned char> outerPlain;
    if (!aesDecrypt(packageKey, outerIv, &outerCipher[0], static_cast<int>(outerCipher.size()), outerPlain)) {
        return "";
    }

    if (outerPlain.size() <= 16) {
        return "";
    }

    unsigned char innerIv[16];
    std::memcpy(innerIv, &outerPlain[0], 16);
    int innerCipherLen = static_cast<int>(outerPlain.size()) - 16;

    unsigned char masterKey[32];
    if (!getMasterKey(masterKey)) {
        return "";
    }

    std::vector<unsigned char> plainOut;
    if (!aesDecrypt(masterKey, innerIv, &outerPlain[16], innerCipherLen, plainOut)) {
        return "";
    }

    return std::string(reinterpret_cast<const char*>(&plainOut[0]), plainOut.size());
}

std::string trimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() &&
           (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

bool hasControlOrSpace(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x20 || c >= 0x7f) {
            return true;
        }
    }
    return false;
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

int clampPriority(int priority) {
    if (priority < 1) return 1;
    if (priority > 4) return 4;
    return priority;
}

std::string syncStateToBadge(SyncStateType state) {
    switch (state) {
        case SyncState::LocallyCreated: return "Local New";
        case SyncState::LocallyUpdated: return "Local Edit";
        case SyncState::LocallyDeleted: return "Deleting";
        case SyncState::Synced:
        default:
            return "Synced";
    }
}

struct TaskDeterministicComparator {
    bool operator()(const Task& a, const Task& b) const {
        if (a.is_completed != b.is_completed) {
            return !a.is_completed; // Incomplete tasks first
        }
        if (a.priority != b.priority) {
            return a.priority > b.priority; // Priority 4 (P1) down to 1 (P4)
        }
        bool aHasDue = !a.due_date.empty();
        bool bHasDue = !b.due_date.empty();
        if (aHasDue != bHasDue) {
            return aHasDue; // Tasks with due dates precede undated tasks
        }
        if (aHasDue && a.due_date != b.due_date) {
            return a.due_date < b.due_date; // Earlier due dates first
        }
        if (a.local_updated_at != b.local_updated_at) {
            return a.local_updated_at > b.local_updated_at; // Most recent updates first
        }
        return a.id < b.id;
    }
};

std::string extractDatePrefix(const std::string& isoDue) {
    if (isoDue.size() >= 10) {
        return isoDue.substr(0, 10);
    }
    return isoDue;
}

} // anonymous namespace

TodoistAppController::TodoistAppController(DatabaseManager& db,
                                           ISyncTransport& transport,
                                           CurlSyncTransport* curlTransport,
                                           const std::string& endpointUrl)
    : m_db(db)
    , m_taskRepo(db)
    , m_cmdQueue(db)
    , m_tempIdReconciler(db)
    , m_metaRepo(db, m_taskRepo, m_cmdQueue)
    , m_syncEngine(db, m_taskRepo, m_cmdQueue, m_tempIdReconciler, m_metaRepo, endpointUrl, "")
    , m_transport(transport)
    , m_curlTransport(curlTransport)
    , m_notificationQueue()
    , m_worker(m_syncEngine, m_transport, m_notificationQueue, m_curlTransport)
    , m_initialized(false)
    , m_workerStarted(false)
    , m_apiToken("")
    , m_requiresAuth(false)
    , m_isSyncing(false)
    , m_filterMode(FilterMode::Inbox)
    , m_activeProjectId("inbox")
    , m_inboxProjectId("")
    , m_searchQuery("")
    , m_todayDateIso(currentLocalDateIso())
    , m_selectedIndex(-1)
    , m_selectedTaskId("")
    , m_createSheetOpen(false)
    , m_detailSheetOpen(false)
    , m_settingsSheetOpen(false)
    , m_deferredDebounceWhileDetailOpen(false)
    , m_debounceWindowMs(DEFAULT_DEBOUNCE_MS)
    , m_debouncePending(false)
    , m_debounceDeadlineMs(0)
    , m_lifecycleState(AppLifecycleState::Foreground)
    , m_networkType(NetworkConnectionType::WiFi)
    , m_wifiPollIntervalMs(DEFAULT_WIFI_POLL_MS)
    , m_cellularPollIntervalMs(DEFAULT_CELLULAR_POLL_MS)
    , m_lastBackgroundPollMs(0)
    , m_lastSyncedAtMs(0)
    , m_totalSyncTriggers(0)
    , m_totalIncompleteCount(0)
    , m_todayIncompleteCount(0)
    , m_completedCount(0)
    , m_pendingCommandCount(0)
    , m_deadLetterCount(0)
    , m_statusText("API token required")
    , m_lastErrorText("")
{
}

TodoistAppController::~TodoistAppController() {
    shutdown();
}

bool TodoistAppController::isValidIsoDate(const std::string& dueDate) {
    if (dueDate.empty()) {
        return true;
    }
    if (dueDate.size() < 10) {
        return false;
    }
    if (dueDate[4] != '-' || dueDate[7] != '-') {
        return false;
    }
    for (size_t i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (dueDate[i] < '0' || dueDate[i] > '9') {
            return false;
        }
    }
    int year = std::atoi(dueDate.substr(0, 4).c_str());
    int month = std::atoi(dueDate.substr(5, 2).c_str());
    int day = std::atoi(dueDate.substr(8, 2).c_str());
    if (year < 1970 || year > 2100) return false;
    if (month < 1 || month > 12) return false;

    int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool isLeap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    if (isLeap) {
        daysInMonth[1] = 29;
    }
    if (day < 1 || day > daysInMonth[month - 1]) {
        return false;
    }

    if (dueDate.size() == 10) {
        return true;
    }

    // Datetime validation: YYYY-MM-DD[T| ]HH:MM[:SS][Z|[+-]HH:MM]
    if (dueDate[10] != 'T' && dueDate[10] != ' ') {
        return false;
    }
    if (dueDate.size() < 16) {
        return false;
    }
    if (dueDate[11] < '0' || dueDate[11] > '9' || dueDate[12] < '0' || dueDate[12] > '9') {
        return false;
    }
    int hour = std::atoi(dueDate.substr(11, 2).c_str());
    if (hour < 0 || hour > 23) {
        return false;
    }
    if (dueDate[13] != ':') {
        return false;
    }
    if (dueDate[14] < '0' || dueDate[14] > '9' || dueDate[15] < '0' || dueDate[15] > '9') {
        return false;
    }
    int min = std::atoi(dueDate.substr(14, 2).c_str());
    if (min < 0 || min > 59) {
        return false;
    }

    size_t pos = 16;
    if (pos < dueDate.size() && dueDate[pos] == ':') {
        if (pos + 2 >= dueDate.size()) {
            return false;
        }
        if (dueDate[pos + 1] < '0' || dueDate[pos + 1] > '9' ||
            dueDate[pos + 2] < '0' || dueDate[pos + 2] > '9') {
            return false;
        }
        int sec = std::atoi(dueDate.substr(pos + 1, 2).c_str());
        if (sec < 0 || sec > 59) {
            return false;
        }
        pos += 3;
    }

    if (pos == dueDate.size()) {
        return true;
    }

    if (dueDate[pos] == 'Z') {
        return (pos + 1 == dueDate.size());
    }

    if (dueDate[pos] == '+' || dueDate[pos] == '-') {
        std::string tz = dueDate.substr(pos + 1);
        if (tz.size() == 5 && tz[2] == ':') {
            if (tz[0] >= '0' && tz[0] <= '9' && tz[1] >= '0' && tz[1] <= '9' &&
                tz[3] >= '0' && tz[3] <= '9' && tz[4] >= '0' && tz[4] <= '9') {
                int tzH = std::atoi(tz.substr(0, 2).c_str());
                int tzM = std::atoi(tz.substr(3, 2).c_str());
                return (tzH >= 0 && tzH <= 23 && tzM >= 0 && tzM <= 59);
            }
        } else if (tz.size() == 4) {
            if (tz[0] >= '0' && tz[0] <= '9' && tz[1] >= '0' && tz[1] <= '9' &&
                tz[2] >= '0' && tz[2] <= '9' && tz[3] >= '0' && tz[3] <= '9') {
                int tzH = std::atoi(tz.substr(0, 2).c_str());
                int tzM = std::atoi(tz.substr(2, 2).c_str());
                return (tzH >= 0 && tzH <= 23 && tzM >= 0 && tzM <= 59);
            }
        }
        return false;
    }

    return false;
}

std::string TodoistAppController::currentLocalDateIso() {
    time_t now = time(NULL);
    struct tm tmBuf;
#if defined(_POSIX_C_SOURCE) || defined(__QNXNTO__) || defined(__linux__)
    localtime_r(&now, &tmBuf);
#else
    struct tm* p = localtime(&now);
    if (p) tmBuf = *p;
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tmBuf.tm_year + 1900,
             tmBuf.tm_mon + 1,
             tmBuf.tm_mday);
    return std::string(buf);
}

bool TodoistAppController::initialize(uint64_t nowMs, bool startWorker) {
    if (!m_db.isOpen()) {
        m_lastErrorText = "Database is not open";
        return false;
    }
    if (!m_db.initializeSchema()) {
        m_lastErrorText = "Failed to initialize database schema";
        return false;
    }
    if (!m_syncEngine.initialize()) {
        m_lastErrorText = "Failed to initialize sync engine recovery";
        return false;
    }

    std::string storedToken;
    if (m_metaRepo.getMetadata("api_token", storedToken)) {
        std::string decrypted = decryptToken(trimAscii(storedToken));
        if (!decrypted.empty() && !hasControlOrSpace(decrypted)) {
            m_apiToken = decrypted;
            m_syncEngine.setBearerToken(m_apiToken);
            m_requiresAuth = false;
            m_statusText = "Ready";
        } else {
            m_apiToken.clear();
            m_requiresAuth = true;
            m_statusText = "API token required";
        }
    } else {
        m_apiToken.clear();
        m_requiresAuth = true;
        m_statusText = "API token required";
    }

    std::string metaInbox;
    if (m_metaRepo.getMetadata("inbox_project_id", metaInbox) && !metaInbox.empty()) {
        m_inboxProjectId = metaInbox;
    }

    reloadTasks();

    if (startWorker) {
        if (!m_worker.start()) {
            m_lastErrorText = "Failed to start background sync worker";
            return false;
        }
        m_workerStarted = true;
    }

    m_initialized = true;
    m_lastBackgroundPollMs = nowMs;

    // Cold-start automatic sync when a valid token is present and online
    if (hasApiToken() && m_networkType != NetworkConnectionType::Offline) {
        triggerSyncNow(nowMs);
    }
    return true;
}

void TodoistAppController::shutdown() {
    m_debouncePending = false;
    m_worker.stop();
    m_workerStarted = false;
    m_isSyncing = false;
}

bool TodoistAppController::setApiToken(const std::string& token, uint64_t nowMs) {
    std::string trimmed = trimAscii(token);
    if (trimmed.empty() || hasControlOrSpace(trimmed)) {
        m_lastErrorText = "Invalid API token: must be non-empty without whitespace or control characters";
        return false;
    }

    std::string encrypted = encryptToken(trimmed);
    if (encrypted.empty()) {
        m_lastErrorText = "Failed to encrypt API token";
        return false;
    }

    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive() || !m_metaRepo.setMetadata("api_token", encrypted) || !tx.commit()) {
        m_lastErrorText = "Failed to persist API token";
        return false;
    }

    m_apiToken = trimmed;
    m_syncEngine.setBearerToken(m_apiToken);
    m_requiresAuth = false;
    m_lastErrorText.clear();
    m_settingsSheetOpen = false;
    m_statusText = "API token saved";

    rebuildVisibleTasksAndCover();

    if (m_networkType != NetworkConnectionType::Offline) {
        triggerSyncNow(nowMs);
    }
    return true;
}

bool TodoistAppController::clearApiToken() {
    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive() || !m_metaRepo.setMetadata("api_token", "") || !tx.commit()) {
        return false;
    }

    m_apiToken.clear();
    m_syncEngine.setBearerToken("");
    m_debouncePending = false;
    m_isSyncing = false;
    m_requiresAuth = true;
    m_statusText = "API token required";
    rebuildVisibleTasksAndCover();
    return true;
}

bool TodoistAppController::hasApiToken() const {
    return !m_apiToken.empty();
}

std::string TodoistAppController::getMaskedApiToken() const {
    if (m_apiToken.empty()) {
        return "";
    }
    if (m_apiToken.size() <= 4) {
        return "••••";
    }
    return std::string("••••••••") + m_apiToken.substr(m_apiToken.size() - 4);
}

bool TodoistAppController::createTask(const std::string& content,
                                      const std::string& description,
                                      int priority,
                                      const std::string& dueDate,
                                      const std::string& projectId,
                                      uint64_t nowMs) {
    std::string cleanContent = trimAscii(content);
    if (cleanContent.empty()) {
        m_lastErrorText = "Task title cannot be empty";
        return false;
    }
    std::string cleanDue = trimAscii(dueDate);
    if (!isValidIsoDate(cleanDue)) {
        m_lastErrorText = "Invalid due date format (expected YYYY-MM-DD)";
        return false;
    }
    std::string cleanProject = trimAscii(projectId);
    if (cleanProject.empty()) {
        if (m_filterMode == FilterMode::Project && !m_activeProjectId.empty()) {
            cleanProject = m_activeProjectId;
        } else {
            cleanProject = "inbox";
        }
    }

    Task task;
    task.content = cleanContent;
    task.description = description;
    task.priority = clampPriority(priority);
    task.due_date = cleanDue;
    task.project_id = cleanProject;
    task.is_completed = false;

    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive() || !m_taskRepo.createTaskLocally(task) || !tx.commit()) {
        m_lastErrorText = "Storage temporarily busy; please retry";
        return false;
    }

    m_lastErrorText.clear();
    m_createSheetOpen = false;
    m_selectedTaskId = task.id;
    reloadTasks();
    scheduleDebouncedSync(nowMs);
    return true;
}

bool TodoistAppController::updateTask(const std::string& taskId,
                                      const std::string& content,
                                      const std::string& description,
                                      int priority,
                                      const std::string& dueDate,
                                      uint64_t nowMs) {
    if (taskId.empty()) {
        m_lastErrorText = "Task ID is empty";
        return false;
    }
    std::string cleanContent = trimAscii(content);
    if (cleanContent.empty()) {
        m_lastErrorText = "Task title cannot be empty";
        return false;
    }
    std::string cleanDue = trimAscii(dueDate);
    if (!isValidIsoDate(cleanDue)) {
        m_lastErrorText = "Invalid due date format (expected YYYY-MM-DD)";
        return false;
    }

    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive() || !m_taskRepo.updateTaskLocally(taskId, cleanContent, description, clampPriority(priority), cleanDue) || !tx.commit()) {
        m_lastErrorText = "Storage temporarily busy; please retry";
        return false;
    }

    m_lastErrorText.clear();
    m_detailSheetOpen = false;
    m_deferredDebounceWhileDetailOpen = false;
    m_selectedTaskId = taskId;
    reloadTasks();
    scheduleDebouncedSync(nowMs);
    return true;
}

bool TodoistAppController::toggleTaskCompleted(const std::string& taskId, uint64_t nowMs) {
    if (taskId.empty()) {
        return false;
    }
    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive()) {
        m_lastErrorText = "Storage temporarily busy; please retry";
        return false;
    }
    Task existing;
    if (!m_taskRepo.getTaskById(taskId, existing)) {
        m_lastErrorText = "Task not found";
        return false;
    }

    bool targetCompleted = !existing.is_completed;
    if (!m_taskRepo.setTaskCompletedLocally(taskId, targetCompleted) || !tx.commit()) {
        m_lastErrorText = "Storage temporarily busy; please retry";
        return false;
    }

    m_lastErrorText.clear();
    if (m_detailSheetOpen && m_detailTask.id == taskId) {
        m_detailTask.isCompleted = targetCompleted;
    }
    reloadTasks();
    scheduleDebouncedSync(nowMs);
    return true;
}

bool TodoistAppController::toggleSelectedTaskCompleted(uint64_t nowMs) {
    const TaskViewItem* sel = getSelectedTask();
    if (!sel) {
        return false;
    }
    return toggleTaskCompleted(sel->id, nowMs);
}

bool TodoistAppController::deleteTask(const std::string& taskId, uint64_t nowMs) {
    if (taskId.empty()) {
        return false;
    }
    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive() || !m_taskRepo.deleteTaskLocally(taskId) || !tx.commit()) {
        m_lastErrorText = "Storage temporarily busy; please retry";
        return false;
    }

    m_lastErrorText.clear();
    if (m_detailSheetOpen && m_detailTask.id == taskId) {
        m_detailSheetOpen = false;
        m_deferredDebounceWhileDetailOpen = false;
    }
    reloadTasks();
    scheduleDebouncedSync(nowMs);
    return true;
}

bool TodoistAppController::deleteSelectedTask(uint64_t nowMs) {
    const TaskViewItem* sel = getSelectedTask();
    if (!sel) {
        return false;
    }
    return deleteTask(sel->id, nowMs);
}

void TodoistAppController::setFilter(FilterModeType mode, const std::string& projectId) {
    m_filterMode = mode;
    if (mode == FilterMode::Project) {
        m_activeProjectId = projectId.empty() ? "inbox" : projectId;
    } else if (mode == FilterMode::Inbox) {
        m_activeProjectId = "inbox";
    } else if (!projectId.empty()) {
        m_activeProjectId = projectId;
    }
    rebuildVisibleTasksAndCover();
}

void TodoistAppController::setFilterByName(const std::string& modeName, const std::string& projectId) {
    setFilter(stringToFilterMode(modeName), projectId);
}

void TodoistAppController::setSearchQuery(const std::string& query) {
    m_searchQuery = trimAscii(query);
    rebuildVisibleTasksAndCover();
}

void TodoistAppController::setTodayDateIso(const std::string& isoDate) {
    if (isValidIsoDate(isoDate) && !isoDate.empty()) {
        m_todayDateIso = extractDatePrefix(isoDate);
        rebuildVisibleTasksAndCover();
    }
}

bool TodoistAppController::isInboxProject(const std::string& projectId) const {
    if (projectId.empty() || projectId == "inbox") {
        return true;
    }
    if (!m_inboxProjectId.empty() && projectId == m_inboxProjectId) {
        return true;
    }
    return false;
}

bool TodoistAppController::setInboxProjectId(const std::string& id) {
    std::string trimmed = trimAscii(id);
    if (trimmed.empty()) {
        m_lastErrorText = "Invalid Inbox project ID";
        return false;
    }
    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive()) {
        m_lastErrorText = "Database busy; please try again";
        return false;
    }
    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "INSERT INTO sync_metadata (key, value) VALUES ('inbox_project_id', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        m_lastErrorText = "Failed to save Inbox project ID";
        return false;
    }
    sqlite3_bind_text(stmt, 1, trimmed.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        m_lastErrorText = "Failed to save Inbox project ID";
        return false;
    }
    if (!tx.commit()) {
        m_lastErrorText = "Failed to commit Inbox project ID";
        return false;
    }
    m_lastErrorText.clear();
    m_inboxProjectId = trimmed;
    rebuildVisibleTasksAndCover();
    return true;
}

std::string TodoistAppController::getProjectDisplayName(const std::string& projectId) {
    if (projectId.empty() || projectId == "inbox" || isInboxProject(projectId)) {
        return "inbox";
    }
    std::string metaKey = "project_name:" + projectId;
    std::string name;
    if (m_metaRepo.getMetadata(metaKey, name) && !name.empty()) {
        return name;
    }
    return projectId;
}

std::string TodoistAppController::resolveProjectId(const std::string& nameOrId) {
    if (nameOrId.empty() || nameOrId == "inbox" || isInboxProject(nameOrId)) {
        return "inbox";
    }
    // Check if nameOrId is already a known project ID
    for (size_t i = 0; i < m_availableProjects.size(); ++i) {
        if (m_availableProjects[i] == nameOrId) {
            return nameOrId;
        }
    }
    // Check if nameOrId matches the display name of any available project
    for (size_t i = 0; i < m_availableProjects.size(); ++i) {
        if (getProjectDisplayName(m_availableProjects[i]) == nameOrId) {
            return m_availableProjects[i];
        }
    }
    // Fallback: check SQLite metadata directly for project_name:* = nameOrId
    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT key FROM sync_metadata WHERE key LIKE 'project_name:%' AND value = ? LIMIT 1;";
    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameOrId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* k = sqlite3_column_text(stmt, 0);
            if (k) {
                std::string kStr(reinterpret_cast<const char*>(k));
                if (kStr.length() > 13) {
                    std::string pId = kStr.substr(13);
                    sqlite3_finalize(stmt);
                    return pId;
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    return nameOrId;
}

bool TodoistAppController::setProjectName(const std::string& projectId, const std::string& name) {
    std::string trimmedId = trimAscii(projectId);
    std::string trimmedName = trimAscii(name);
    if (trimmedId.empty() || trimmedName.empty()) {
        return false;
    }
    std::string metaKey = "project_name:" + trimmedId;
    return m_metaRepo.setMetadata(metaKey, trimmedName);
}

bool TodoistAppController::queryAllNonDeletedTasks(std::vector<Task>& outTasks) {
    outTasks.clear();
    if (!m_db.isOpen()) {
        return false;
    }
    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "SELECT id, server_id, content, description, is_completed, priority, due_date, project_id, local_updated_at, sync_state "
        "FROM tasks WHERE sync_state != 'locally_deleted';";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Task t;
        const unsigned char* idText = sqlite3_column_text(stmt, 0);
        const unsigned char* serverIdText = sqlite3_column_text(stmt, 1);
        const unsigned char* contentText = sqlite3_column_text(stmt, 2);
        const unsigned char* descText = sqlite3_column_text(stmt, 3);
        const unsigned char* dueText = sqlite3_column_text(stmt, 6);
        const unsigned char* projText = sqlite3_column_text(stmt, 7);
        const unsigned char* stateText = sqlite3_column_text(stmt, 9);

        t.id = idText ? reinterpret_cast<const char*>(idText) : "";
        t.server_id = serverIdText ? reinterpret_cast<const char*>(serverIdText) : "";
        t.content = contentText ? reinterpret_cast<const char*>(contentText) : "";
        t.description = descText ? reinterpret_cast<const char*>(descText) : "";
        t.is_completed = (sqlite3_column_int(stmt, 4) != 0);
        t.priority = sqlite3_column_int(stmt, 5);
        t.due_date = dueText ? reinterpret_cast<const char*>(dueText) : "";
        t.project_id = projText ? reinterpret_cast<const char*>(projText) : "inbox";
        t.local_updated_at = sqlite3_column_int64(stmt, 8);
        t.sync_state = stateText ? stringToSyncState(reinterpret_cast<const char*>(stateText)) : SyncState::Synced;
        outTasks.push_back(t);
    }
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

TaskViewItem TodoistAppController::toViewItem(const Task& task, bool selected, const std::string& projectDisplayName) {
    TaskViewItem item;
    item.id = task.id;
    item.serverId = task.server_id;
    item.content = task.content;
    item.description = task.description;
    item.isCompleted = task.is_completed;
    item.priority = task.priority;
    item.dueDate = task.due_date;
    item.projectId = task.project_id.empty() ? "inbox" : task.project_id;
    item.projectDisplayName = projectDisplayName.empty() ? item.projectId : projectDisplayName;
    item.localUpdatedAt = task.local_updated_at;
    item.syncState = task.sync_state;
    item.syncBadgeText = syncStateToBadge(task.sync_state);
    item.isSelected = selected;
    return item;
}

bool TodoistAppController::reloadTasks() {
    DatabaseManager::ScopedSnapshotLock snapLock(m_db);
    if (!snapLock.isLocked()) {
        return true;
    }

    std::vector<Task> fetched;
    if (!queryAllNonDeletedTasks(fetched)) {
        return false;
    }

    // 1. Reconcile m_selectedTaskId and m_detailTask.id if a temp_id was mapped via temp_id_mappings
    std::string stagedSelectedId = m_selectedTaskId;
    TaskViewItem stagedDetailTask = m_detailTask;

    if (!stagedSelectedId.empty()) {
        sqlite3_stmt* mapStmt = NULL;
        if (sqlite3_prepare_v2(m_db.handle(), "SELECT server_id FROM temp_id_mappings WHERE temp_id = ?;", -1, &mapStmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(mapStmt, 1, stagedSelectedId.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(mapStmt) == SQLITE_ROW) {
                const unsigned char* sId = sqlite3_column_text(mapStmt, 0);
                if (sId) {
                    stagedSelectedId = reinterpret_cast<const char*>(sId);
                }
            }
            sqlite3_finalize(mapStmt);
        }
    }
    if (m_detailSheetOpen && !stagedDetailTask.id.empty()) {
        sqlite3_stmt* mapStmt = NULL;
        if (sqlite3_prepare_v2(m_db.handle(), "SELECT server_id FROM temp_id_mappings WHERE temp_id = ?;", -1, &mapStmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(mapStmt, 1, stagedDetailTask.id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(mapStmt) == SQLITE_ROW) {
                const unsigned char* sId = sqlite3_column_text(mapStmt, 0);
                if (sId) {
                    stagedDetailTask.id = reinterpret_cast<const char*>(sId);
                    stagedDetailTask.serverId = reinterpret_cast<const char*>(sId);
                }
            }
            sqlite3_finalize(mapStmt);
        }
    }

    // 2. Discover Inbox project ID if configured in metadata
    std::string stagedInboxProjectId = m_inboxProjectId;
    if (stagedInboxProjectId.empty()) {
        std::string metaInbox;
        if (m_metaRepo.getMetadata("inbox_project_id", metaInbox) && !metaInbox.empty()) {
            stagedInboxProjectId = metaInbox;
        }
    }

    // 3. Read queue and dead-letter counts under the snapshot lock
    int pendingCnt = m_cmdQueue.getPendingCount();
    int inFlightCnt = m_cmdQueue.getInFlightCount();
    int deadCnt = m_cmdQueue.getDeadLetterCount();

    if (pendingCnt < 0 || inFlightCnt < 0 || deadCnt < 0) {
        return false;
    }

    // 4. All reads succeeded atomically: publish complete new snapshot!
    m_inboxProjectId = stagedInboxProjectId;
    m_selectedTaskId = stagedSelectedId;
    m_detailTask = stagedDetailTask;
    m_pendingCommandCount = pendingCnt + inFlightCnt;
    std::string metaInbox;
    if (m_metaRepo.getMetadata("inbox_project_id", metaInbox) && !metaInbox.empty()) {
        m_inboxProjectId = metaInbox;
    }

    m_deadLetterCount = deadCnt;

    m_allTasks.swap(fetched);
    rebuildVisibleTasksAndCover();
    return true;
}

void TodoistAppController::rebuildVisibleTasksAndCover() {
    // 1. Collect available projects ("inbox" always first)
    std::set<std::string> projectSet;
    for (size_t i = 0; i < m_allTasks.size(); ++i) {
        if (!m_allTasks[i].project_id.empty() && !isInboxProject(m_allTasks[i].project_id)) {
            projectSet.insert(m_allTasks[i].project_id);
        }
    }

    // Also include any synced projects from metadata even if they currently have no tasks
    sqlite3_stmt* pStmt = NULL;
    const char* pSql = "SELECT key FROM sync_metadata WHERE key LIKE 'project_name:%';";
    if (sqlite3_prepare_v2(m_db.handle(), pSql, -1, &pStmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(pStmt) == SQLITE_ROW) {
            const unsigned char* k = sqlite3_column_text(pStmt, 0);
            if (k) {
                std::string kStr(reinterpret_cast<const char*>(k));
                if (kStr.length() > 13) {
                    std::string pId = kStr.substr(13);
                    if (!pId.empty() && !isInboxProject(pId)) {
                        projectSet.insert(pId);
                    }
                }
            }
        }
        sqlite3_finalize(pStmt);
    }

    m_availableProjects.clear();
    m_availableProjects.push_back("inbox");
    for (std::set<std::string>::const_iterator it = projectSet.begin(); it != projectSet.end(); ++it) {
        m_availableProjects.push_back(*it);
    }

    // 2. Compute global incomplete, today/overdue, and completed counts
    m_totalIncompleteCount = 0;
    m_todayIncompleteCount = 0;
    m_completedCount = 0;

    std::vector<Task> allIncomplete;
    for (size_t i = 0; i < m_allTasks.size(); ++i) {
        const Task& t = m_allTasks[i];
        if (t.is_completed) {
            ++m_completedCount;
        } else {
            ++m_totalIncompleteCount;
            allIncomplete.push_back(t);
            if (!t.due_date.empty() && extractDatePrefix(t.due_date) <= m_todayDateIso) {
                ++m_todayIncompleteCount;
            }
        }
    }

    // 3. Filter tasks for current view
    std::string lowerQuery = toLowerAscii(m_searchQuery);
    std::vector<Task> filtered;
    for (size_t i = 0; i < m_allTasks.size(); ++i) {
        const Task& t = m_allTasks[i];
        bool modeMatch = false;
        switch (m_filterMode) {
            case FilterMode::Inbox:
                modeMatch = (!t.is_completed && isInboxProject(t.project_id));
                break;
            case FilterMode::Today:
                modeMatch = (!t.is_completed && !t.due_date.empty() &&
                             extractDatePrefix(t.due_date) <= m_todayDateIso);
                break;
            case FilterMode::All:
                modeMatch = !t.is_completed;
                break;
            case FilterMode::Completed:
                modeMatch = t.is_completed;
                break;
            case FilterMode::Project:
                modeMatch = (!t.is_completed && t.project_id == m_activeProjectId);
                break;
        }
        if (!modeMatch) {
            continue;
        }

        if (!lowerQuery.empty()) {
            std::string lowerContent = toLowerAscii(t.content);
            std::string lowerDesc = toLowerAscii(t.description);
            std::string lowerProj = toLowerAscii(t.project_id);
            if (lowerContent.find(lowerQuery) == std::string::npos &&
                lowerDesc.find(lowerQuery) == std::string::npos &&
                lowerProj.find(lowerQuery) == std::string::npos) {
                continue;
            }
        }
        filtered.push_back(t);
    }

    std::sort(filtered.begin(), filtered.end(), TaskDeterministicComparator());

    // 4. Reconcile selection index and selected task ID
    int newSelectedIndex = -1;
    if (!m_selectedTaskId.empty()) {
        for (size_t i = 0; i < filtered.size(); ++i) {
            if (filtered[i].id == m_selectedTaskId) {
                newSelectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
    if (newSelectedIndex < 0 && !filtered.empty()) {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(filtered.size())) {
            newSelectedIndex = m_selectedIndex;
        } else if (m_selectedIndex >= static_cast<int>(filtered.size())) {
            newSelectedIndex = static_cast<int>(filtered.size()) - 1;
        } else {
            newSelectedIndex = 0;
        }
        m_selectedTaskId = filtered[static_cast<size_t>(newSelectedIndex)].id;
    } else if (filtered.empty()) {
        newSelectedIndex = -1;
        m_selectedTaskId.clear();
    }
    m_selectedIndex = newSelectedIndex;

    m_visibleTasks.clear();
    m_visibleTasks.reserve(filtered.size());
    for (size_t i = 0; i < filtered.size(); ++i) {
        bool isSel = (static_cast<int>(i) == m_selectedIndex);
        m_visibleTasks.push_back(toViewItem(filtered[i], isSel, getProjectDisplayName(filtered[i].project_id)));
    }

    // 5. Update Active Frame (SceneCover) snapshot from committed in-memory state
    std::sort(allIncomplete.begin(), allIncomplete.end(), TaskDeterministicComparator());

    m_activeFrameSnapshot.totalIncompleteCount = m_totalIncompleteCount;
    m_activeFrameSnapshot.todayIncompleteCount = m_todayIncompleteCount;
    m_activeFrameSnapshot.pendingMutationCount = m_pendingCommandCount;
    m_activeFrameSnapshot.deadLetterCount = m_deadLetterCount;
    m_activeFrameSnapshot.isSyncing = m_isSyncing;
    m_activeFrameSnapshot.requiresAuth = m_requiresAuth;

    if (m_requiresAuth || !hasApiToken()) {
        m_activeFrameSnapshot.statusHeadline = "Auth Required";
    } else if (m_isSyncing) {
        m_activeFrameSnapshot.statusHeadline = "Syncing...";
    } else if (m_networkType == NetworkConnectionType::Offline) {
        std::ostringstream oss;
        oss << "Offline (" << m_pendingCommandCount << " queued)";
        m_activeFrameSnapshot.statusHeadline = oss.str();
    } else if (m_pendingCommandCount > 0) {
        std::ostringstream oss;
        oss << "Pending (" << m_pendingCommandCount << ")";
        m_activeFrameSnapshot.statusHeadline = oss.str();
    } else {
        m_activeFrameSnapshot.statusHeadline = "Synced";
    }

    m_activeFrameSnapshot.topTasks.clear();
    size_t coverLimit = allIncomplete.size() < MAX_ACTIVE_FRAME_TASKS
                            ? allIncomplete.size()
                            : MAX_ACTIVE_FRAME_TASKS;
    for (size_t i = 0; i < coverLimit; ++i) {
        m_activeFrameSnapshot.topTasks.push_back(toViewItem(allIncomplete[i], false, getProjectDisplayName(allIncomplete[i].project_id)));
    }
}

bool TodoistAppController::selectTaskByIndex(int index) {
    if (index < 0 || index >= static_cast<int>(m_visibleTasks.size())) {
        return false;
    }
    m_selectedIndex = index;
    m_selectedTaskId = m_visibleTasks[static_cast<size_t>(index)].id;
    for (size_t i = 0; i < m_visibleTasks.size(); ++i) {
        m_visibleTasks[i].isSelected = (static_cast<int>(i) == m_selectedIndex);
    }
    return true;
}

bool TodoistAppController::selectTaskById(const std::string& taskId) {
    for (size_t i = 0; i < m_visibleTasks.size(); ++i) {
        if (m_visibleTasks[i].id == taskId) {
            return selectTaskByIndex(static_cast<int>(i));
        }
    }
    return false;
}

bool TodoistAppController::selectNextTask() {
    if (m_visibleTasks.empty()) {
        return false;
    }
    int nextIdx = m_selectedIndex + 1;
    if (nextIdx >= static_cast<int>(m_visibleTasks.size())) {
        nextIdx = static_cast<int>(m_visibleTasks.size()) - 1;
    }
    return selectTaskByIndex(nextIdx);
}

bool TodoistAppController::selectPreviousTask() {
    if (m_visibleTasks.empty()) {
        return false;
    }
    int prevIdx = m_selectedIndex <= 0 ? 0 : m_selectedIndex - 1;
    return selectTaskByIndex(prevIdx);
}

const TaskViewItem* TodoistAppController::getSelectedTask() const {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_visibleTasks.size())) {
        return NULL;
    }
    return &m_visibleTasks[static_cast<size_t>(m_selectedIndex)];
}

void TodoistAppController::openCreateSheet() {
    m_detailSheetOpen = false;
    m_settingsSheetOpen = false;
    m_createSheetOpen = true;
}

bool TodoistAppController::openSelectedTaskDetail() {
    const TaskViewItem* sel = getSelectedTask();
    if (!sel) {
        return false;
    }
    m_detailTask = *sel;
    m_createSheetOpen = false;
    m_settingsSheetOpen = false;
    m_detailSheetOpen = true;
    return true;
}

bool TodoistAppController::openTaskDetailById(const std::string& taskId) {
    if (!selectTaskById(taskId)) {
        Task raw;
        if (!m_taskRepo.getTaskById(taskId, raw) || raw.sync_state == SyncState::LocallyDeleted) {
            return false;
        }
        m_detailTask = toViewItem(raw, true, getProjectDisplayName(raw.project_id));
        m_createSheetOpen = false;
        m_settingsSheetOpen = false;
        m_detailSheetOpen = true;
        return true;
    }
    return openSelectedTaskDetail();
}

void TodoistAppController::openSettingsSheet() {
    m_createSheetOpen = false;
    m_detailSheetOpen = false;
    m_settingsSheetOpen = true;
}

void TodoistAppController::closeSheets(uint64_t nowMs) {
    bool wasDetailOpen = m_detailSheetOpen;
    m_createSheetOpen = false;
    m_detailSheetOpen = false;
    m_settingsSheetOpen = false;

    if (wasDetailOpen && m_deferredDebounceWhileDetailOpen) {
        m_deferredDebounceWhileDetailOpen = false;
        scheduleDebouncedSync(nowMs);
    }
}

bool TodoistAppController::handleKeyShortcut(const std::string& key, uint64_t nowMs) {
    if (key.empty()) {
        return false;
    }

    // When a modal sheet is open, block background list shortcuts so text input in fields
    // (Space, Backspace, C, R, Enter) never mutates or deletes background tasks.
    if (isAnySheetOpen()) {
        if (key == "Escape" || key == "Esc") {
            closeSheets(nowMs);
            return true;
        }
        return false;
    }

    if (key == "Space" || key == " ") {
        return toggleSelectedTaskCompleted(nowMs);
    }
    if (key == "Enter" || key == "Return") {
        return openSelectedTaskDetail();
    }
    if (key == "Delete" || key == "Backspace" || key == "Del") {
        return deleteSelectedTask(nowMs);
    }
    if (key == "c" || key == "C" || key == "n" || key == "N") {
        openCreateSheet();
        return true;
    }
    if (key == "r" || key == "R") {
        return triggerSyncNow(nowMs);
    }
    if (key == "j" || key == "J" || key == "Down") {
        return selectNextTask();
    }
    if (key == "k" || key == "K" || key == "Up") {
        return selectPreviousTask();
    }
    return false;
}

void TodoistAppController::scheduleDebouncedSync(uint64_t nowMs) {
    if (m_detailSheetOpen) {
        m_deferredDebounceWhileDetailOpen = true;
        return;
    }
    m_debouncePending = true;
    m_debounceDeadlineMs = nowMs + m_debounceWindowMs;
    if (m_networkType == NetworkConnectionType::Offline) {
        m_statusText = "Offline - changes queued locally";
    } else if (!hasApiToken()) {
        m_statusText = "Saved locally - API token required to sync";
    } else {
        m_statusText = "Changes saved locally (sync scheduled)";
    }
    rebuildVisibleTasksAndCover();
}

bool TodoistAppController::triggerSyncNow(uint64_t nowMs) {
    m_debouncePending = false;
    m_deferredDebounceWhileDetailOpen = false;

    if (!hasApiToken()) {
        m_requiresAuth = true;
        m_statusText = "API token required";
        rebuildVisibleTasksAndCover();
        return false;
    }
    if (m_requiresAuth) {
        m_statusText = "Authentication required (401/403); update API token to sync";
        rebuildVisibleTasksAndCover();
        return false;
    }
    if (m_networkType == NetworkConnectionType::Offline) {
        m_statusText = "Offline - changes queued locally";
        rebuildVisibleTasksAndCover();
        return false;
    }

    m_isSyncing = true;
    m_statusText = "Syncing...";
    m_lastBackgroundPollMs = nowMs;
    ++m_totalSyncTriggers;

    if (m_workerStarted) {
        m_worker.requestSync();
    }
    rebuildVisibleTasksAndCover();
    return true;
}

bool TodoistAppController::triggerFullSync(uint64_t nowMs) {
    m_debouncePending = false;
    m_deferredDebounceWhileDetailOpen = false;

    if (!hasApiToken()) {
        m_requiresAuth = true;
        m_statusText = "API token required";
        m_lastErrorText = "API token required to sync";
        rebuildVisibleTasksAndCover();
        return false;
    }
    if (m_requiresAuth) {
        m_statusText = "Authentication required (401/403); update API token to sync";
        m_lastErrorText = "Authentication required; update API token";
        rebuildVisibleTasksAndCover();
        return false;
    }
    if (m_networkType == NetworkConnectionType::Offline) {
        m_statusText = "Offline - changes queued locally";
        m_lastErrorText = "Device is offline";
        rebuildVisibleTasksAndCover();
        return false;
    }

    // Zero-timeout transaction to avoid blocking UI thread
    DatabaseManager::ScopedTransaction tx(m_db, 0);
    if (!tx.isActive()) {
        m_lastErrorText = "Database busy; please try again";
        return false;
    }

    sqlite3_stmt* stmt = NULL;
    const char* sql =
        "INSERT INTO sync_metadata (key, value) VALUES ('sync_token', '*') "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";

    if (sqlite3_prepare_v2(m_db.handle(), sql, -1, &stmt, NULL) != SQLITE_OK) {
        m_lastErrorText = "Failed to prepare reset sync token statement";
        return false;
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        m_lastErrorText = "Failed to reset sync token in database";
        return false;
    }

    if (!tx.commit()) {
        m_lastErrorText = "Failed to commit reset sync token";
        return false;
    }

    m_lastErrorText.clear();
    return triggerSyncNow(nowMs);
}

int TodoistAppController::drainWorkerNotifications(uint64_t nowMs) {
    std::vector<SyncEventSnapshot> events;
    m_notificationQueue.drainNotifications(events);
    if (events.empty()) {
        return 0;
    }

    bool needReload = false;
    for (size_t i = 0; i < events.size(); ++i) {
        const SyncEventSnapshot& ev = events[i];
        switch (ev.state) {
            case WORKER_STATE_SYNCING:
                m_isSyncing = true;
                m_statusText = "Syncing...";
                break;

            case WORKER_STATE_SUCCESS:
                m_isSyncing = false;
                if (!hasApiToken() || m_syncEngine.getState() == SyncEngineState::AuthError) {
                    m_requiresAuth = true;
                    m_settingsSheetOpen = true;
                    m_createSheetOpen = false;
                    m_detailSheetOpen = false;
                    m_statusText = "Authentication required";
                } else {
                    m_requiresAuth = false;
                    m_lastErrorText.clear();
                    m_statusText = "Synced";
                }
                m_lastSyncedAtMs = nowMs > 0 ? nowMs : static_cast<uint64_t>(ev.timestamp) * 1000ULL;
                needReload = true;
                break;

            case WORKER_STATE_BACKOFF_WAIT:
                m_isSyncing = false;
                m_lastErrorText = ev.errorMessage;
                m_statusText = "Transient sync issue - retry scheduled";
                needReload = true;
                break;

            case WORKER_STATE_FATAL_ERROR:
                m_isSyncing = false;
                m_lastErrorText = ev.errorMessage;
                if (ev.errorCode == 401 || ev.errorCode == 403 ||
                    m_syncEngine.getState() == SyncEngineState::AuthError) {
                    m_requiresAuth = true;
                    m_settingsSheetOpen = true;
                    m_createSheetOpen = false;
                    m_detailSheetOpen = false;
                    m_statusText = "Authentication failed - please update API token";
                } else {
                    m_statusText = "Sync error: " + ev.errorMessage;
                }
                needReload = true;
                break;

            case WORKER_STATE_IDLE:
                m_isSyncing = false;
                break;
        }
    }

    if (needReload) {
        reloadTasks();
    } else {
        rebuildVisibleTasksAndCover();
    }
    return static_cast<int>(events.size());
}

int TodoistAppController::tick(uint64_t nowMs) {
    int actions = 0;

    std::string todayIso = currentLocalDateIso();
    if (todayIso != m_todayDateIso) {
        m_todayDateIso = todayIso;
        rebuildVisibleTasksAndCover();
        ++actions;
    }

    // 1. Drain any completed worker events first
    actions += drainWorkerNotifications(nowMs);

    // 2. Fire coalesced 3-second mutation debounce if due
    if (m_debouncePending && !m_detailSheetOpen && nowMs >= m_debounceDeadlineMs) {
        m_debouncePending = false;
        if (triggerSyncNow(nowMs)) {
            ++actions;
        }
    }

    // 3. Conservative Active Frame / background periodic polling when minimized
    if (m_lifecycleState != AppLifecycleState::Foreground &&
        !m_isSyncing &&
        !m_requiresAuth &&
        hasApiToken() &&
        m_networkType != NetworkConnectionType::Offline) {
        uint64_t intervalMs = getActivePollIntervalMs();
        if (intervalMs > 0 && nowMs >= m_lastBackgroundPollMs + intervalMs) {
            if (triggerSyncNow(nowMs)) {
                ++actions;
            }
        }
    }

    return actions;
}

void TodoistAppController::setLifecycleState(AppLifecycleStateType state, uint64_t nowMs) {
    AppLifecycleStateType prev = m_lifecycleState;
    m_lifecycleState = state;

    std::string todayIso = currentLocalDateIso();
    if (todayIso != m_todayDateIso) {
        m_todayDateIso = todayIso;
        rebuildVisibleTasksAndCover();
    }

    if (state != AppLifecycleState::Foreground && prev == AppLifecycleState::Foreground) {
        m_lastBackgroundPollMs = nowMs;
        // Flush pending debounce immediately before settling into minimized Active Frame
        if (m_debouncePending && hasApiToken() && m_networkType != NetworkConnectionType::Offline) {
            triggerSyncNow(nowMs);
        } else {
            rebuildVisibleTasksAndCover();
        }
    } else if (state == AppLifecycleState::Foreground && prev != AppLifecycleState::Foreground) {
        reloadTasks();
    }
}

void TodoistAppController::setNetworkConnectionType(NetworkConnectionTypeVal type, uint64_t nowMs) {
    bool wasOffline = (m_networkType == NetworkConnectionType::Offline);
    m_networkType = type;
    bool reachable = (type != NetworkConnectionType::Offline);

    if (m_workerStarted && hasApiToken() && !m_requiresAuth) {
        m_worker.notifyReachabilityChanged(reachable);
    }

    if (!reachable) {
        m_isSyncing = false;
        m_statusText = "Offline - changes queued locally";
    } else if (wasOffline) {
        m_statusText = hasApiToken() ? (m_requiresAuth ? "Authentication required" : "Online") : "API token required";
        if (hasApiToken() && !m_requiresAuth && (m_pendingCommandCount > 0 || m_debouncePending)) {
            triggerSyncNow(nowMs);
            return;
        }
    }
    rebuildVisibleTasksAndCover();
}

uint64_t TodoistAppController::getActivePollIntervalMs() const {
    if (m_networkType == NetworkConnectionType::Offline) {
        return 0;
    }
    if (m_networkType == NetworkConnectionType::WiFi) {
        return m_wifiPollIntervalMs;
    }
    return m_cellularPollIntervalMs;
}

} // namespace todoist
