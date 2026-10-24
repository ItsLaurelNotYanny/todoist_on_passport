# RFC: Standalone Modern TLS 1.3 & HTTPS Subsystem for BlackBerry 10

## 1. Metadata & Overview

| Field | Value |
| :--- | :--- |
| **RFC Title** | Standalone Modern TLS 1.3 & HTTPS Subsystem for BlackBerry 10 (Passport / QNX 6.6) |
| **Milestone Context** | Milestone 3 (`todoist_network`) & Universal BB10 Ecosystem Integration (`LocalSend-BB10`) |
| **Author** | agy (Implementation Lead) |
| **Reviewer & Architecture Partner** | Codex (Review Lead & Architecture Partner) |
| **Status** | Consensus-Aligned RFC (Post Round 0 Revision 2 Resolution) |
| **Target Runtime** | BlackBerry 10 OS 10.3.3.3216 (QNX Neutrino 6.6 RTOS, ARMv7-A / Krait 400) |
| **Host Toolchain** | Clang 19.1.7 (`bb10-cc`) with BB10 sysroot targeting `arm-unknown-nto-qnx8.0.0eabi` (`bb10-native-dev:local`) |

---

## 2. Executive Summary & Problem Statement

### 2.1 The Legacy Platform Problem
The native network libraries included in BlackBerry 10 OS 10.3.3 (QNX 6.6) were frozen in 2012–2014:
- **System OpenSSL (`OpenSSL 1.0.1i`, August 2014)**:
  - Lacks TLS 1.3 (RFC 8446 was finalized in 2018).
  - Lacks modern elliptic curves (`X25519`) and AEAD ciphers (`ChaCha20-Poly1305`, modern AES-GCM configurations).
  - Encounters legacy certificate verification failures on modern cross-signed CA chains (e.g., ISRG Root X1).
- **System libcurl (`libcurl 7.24.0`, January 2012)**:
  - Predates TLS 1.2 / 1.3 configuration symbols.
  - Dynamically linked to system `libssl.so.2` (OpenSSL 1.0.1i).
- **QtNetwork (`Qt 4.8.6`)**:
  - Dynamically bound to system OpenSSL 1.0.1i, making native Qt SSL incapable of TLS 1.3 negotiation.

### 2.2 Immediate Consumer Requirements
1. **Todoist on Passport (Milestone 3)**:
   - Todoist Sync API (`https://api.todoist.com/api/v1/sync`) requires modern TLS 1.2/1.3 with SNI and public CA root verification.
   - Synchronous HTTP POST payloads with `Authorization: Bearer <token>` authentication.
   - Deterministic concurrency, backoff scheduling, and background thread lifecycle managed outside UI and transport layers.
2. **LocalSend-BB10 (Peer-to-Peer LAN Transfer)**:
   - Needs raw TLS 1.3 socket support over local Wi-Fi (default port 53317).
   - Protocol requirement: LocalSend v2.2 utilizes transient self-signed X.509 certificates and pins peer identity via SHA-256 fingerprint verification (rather than public PKI CA roots).
   - Needs bidirectional TLS capability: **TLS Client** (to send requests/files to peers; currently blocked by `SendManager.cpp:98` which explicitly rejects HTTPS peers) and **TLS Server** (to accept inbound transfers in `HttpServer` / `HttpConnection`).

### 2.3 Architectural Directive
Rather than constructing an ad-hoc, tightly coupled HTTP client embedded in Todoist, we deliver:
1. **`bb10_tls` (`libbb10_tls.a`)**: A standalone, static C/C++03 TLS 1.3 library providing raw socket client and server abstractions with strict policy separation (Public PKI vs P2P SHA-256 fingerprint pinning), packaged as a relocatable CMake package.
2. **`todoist_network`**: A clean implementation of `ISyncTransport` backed by a trimmed, static `libcurl 8.23.0` build linked against `bb10_tls`'s isolated engine, driven by a joinable background worker thread (`SyncWorkerThread`).

---

## 3. Core Architectural Decisions (Resolving M3-D001 through M3-D005)

### 3.1 Topic 1: Cryptographic Engine Selection — OpenSSL 3.5.9 LTS (M3-D001)

#### Comparative Analysis & Evaluation:

| Dimension | **Option A: OpenSSL 3.5.9 LTS** (Selected) | **Option B: Mbed TLS 3.6 LTS** (Evaluated) | **Option C: Mbed TLS 4.1 LTS** (Evaluated) |
| :--- | :--- | :--- | :--- |
| **Upstream Support Horizon** | **April 2030** (~3.5 years active LTS support) | **March 2027** (~5 months remaining; near EOL) | **March 2029** (~2.5 years remaining) |
| **Symbol Collision Isolation** | **Proven zero-export isolation** via `-Wl,--exclude-libs,ALL`. Verified by `passport-weather-bb10`. | Zero symbol collision via `mbedtls_*` prefix. | Zero symbol collision via `psa_*` / `mbedtls_*` prefix. |
| **Dynamic Symbol Table Check** | `.dynsym` contains **0** exported `SSL_*`, `OPENSSL_*`, `EVP_*`, `X509_*`, `CRYPTO_*` symbols. | Clean. | Clean. |
| **LocalSend Synergy** | **Direct reuse**: LocalSend `CryptoUtil.cpp` uses OpenSSL `RAND_bytes()` and `SHA256_*()`. (See migration contract below). | Requires introducing disjoint PSA crypto APIs or duplicate hashes. | Classic crypto APIs removed; mandates complete PSA Crypto rewrite. |
| **libcurl Compatibility** | Native first-class backend for `libcurl 8.23.0`. | Supported, but requires custom curl build configuration. | Upstream curl support for Mbed 4.x is experimental/unstable. |
| **Target Build Recipe** | **Existing & tested** in `passport-weather-bb10`: static build via Clang 19.1.7 (`bb10-cc`). | Would require authoring and maintaining new QNX build recipes. | Requires new PSA backend recipes on QNX 6.6. |
| **ARMv7 Linked Footprint** | Static archive: `libcrypto.a` ~7.0 MB, `libssl.a` ~968 KB. Estimated linked code in binary ~1.5 MB; entire weather BAR is ~2.4 MB. *(Exact linked size pending M3 target compile gate)*. | Estimated ~400 KB - 500 KB linked. | Estimated ~500 KB - 600 KB linked. |
| **Target RAM & Runtime** | Target per-session RAM overhead: *(Pending target hardware execution gate)*. | Estimated ~35 KB per session. | Estimated ~40 KB per session. |

#### Architectural Consensus & Evidence Boundary:
- **Decision**: Adopt **OpenSSL 3.5.9 LTS** as the sole cryptographic engine for `bb10_tls`.
- **Linker Isolation Evidence vs Target Runtime Evidence**:
  - *Link-Time Evidence (Verified)*: The static build linked with `-Wl,--exclude-libs,ALL` hides all static symbols from the dynamic symbol table (`.dynsym`). Verification in `passport-weather-bb10/scripts/check-package.py:62` proves zero exported symbols matching `SSL_*`, `OPENSSL_*`, `EVP_*`, `X509_*`, `CRYPTO_*`, or `curl_*`.
  - *Target Runtime Coexistence (Pending Hardware Gate)*: While build isolation is established, target phone execution of Cascades/QtNetwork alongside OpenSSL 3.5.9 in the same process is explicitly scheduled under Gate 2 & Gate 3.
- **Build Configuration**:
  ```sh
  CC=bb10-cc AR=llvm-ar RANLIB=llvm-ranlib perl Configure gcc \
      --prefix="$prefix" --libdir=lib no-shared no-tests no-apps \
      no-autoload-config no-dso no-module no-asm no-async no-engine \
      no-legacy no-quic -DOPENSSL_NO_SECURE_MEMORY -DNO_SYSLOG -fPIC
  ```
- **LocalSend Crypto Migration Contract**:
  - LocalSend's `src/protocol/CryptoUtil.cpp` uses `RAND_bytes()` and `SHA256_*()` context functions. Merely linking a newer archive without matching header layouts would risk struct size mismatches (e.g. `sizeof(SHA256_CTX)`).
  - *Migration Contract*: `bb10_tls` exports clean, opaque cryptographic helper functions:
    ```c
    int bb10_tls_random_bytes(unsigned char* buf, size_t len);
    int bb10_tls_sha256(const unsigned char* data, size_t len, unsigned char* out_hash_32);
    int bb10_tls_sha256_file(const char* filepath, char* out_hex_65);
    ```
    LocalSend can either adopt these opaque entry points (guaranteeing ABI stability without exposing OpenSSL headers) or compile `CryptoUtil.cpp` with the matching OpenSSL 3.5.9 headers provided by the `bb10_tls` SDK package.
- **QNX Entropy Failure Contract**:
  - OpenSSL CSPRNG acquires entropy from `/dev/urandom` on QNX Neutrino.
  - Return values of `RAND_bytes()` are strictly checked: `if (RAND_bytes(buf, len) != 1) return BB10_TLS_ERR_ENTROPY_FAILURE;`.
  - On entropy failure, TLS context initialization and key generation fail closed immediately.

---

### 3.2 Topic 2: HTTP Framing & Competing Socket Ownership Resolution (M3-D002)

#### Client HTTP Framing (Todoist M3):
- **Decision**: Reuse the trimmed static `libcurl 8.23.0` build recipe from `passport-weather-bb10`.
- **Security & Operational Bounds**:
  1. **Bounded Response Buffer**: Enforce maximum response allocation (10 MiB ceiling via write callback) to guarantee protection against OOM on memory-constrained devices.
  2. **Bounded Timeouts**: Enforce `CURLOPT_CONNECTTIMEOUT_MS` (15000ms) and `CURLOPT_TIMEOUT_MS` (30000ms).
  3. **Credential Isolation**: Disable automatic cross-origin credential leaks on redirection (`CURLOPT_UNRESTRICTED_AUTH = 0L`).
  4. **Protocol & ALPN Restriction**: Limit ALPN to HTTP/1.1 (`CURL_HTTP_VERSION_1_1`). Do not advertise HTTP/2 unless explicit nghttp2 dependencies are added.
  5. **TLS Version Enforcement**: Require `CURL_SSLVERSION_TLSv1_2` as baseline, enabling negotiation up to TLS 1.3.

#### Raw TLS Server & Socket Ownership Resolution (LocalSend-BB10):
- **Competing Socket Ownership Hazard**:
  Passing an active `QTcpSocket::socketDescriptor()` to OpenSSL while `QTcpSocket` is still registered in Qt's event loop creates competing ciphertext readers and double-close hazards. Furthermore, in Qt 4.8 `QAbstractSocket::setSocketDescriptor(-1)` calls `resetSocketLayer()`, which closes the underlying descriptor.
- **Sole Socket Ownership Contract**:
  1. **Inbound Server Path**:
     - When an incoming connection arrives on the listening port, the native descriptor `int client_fd = ::accept(...)` is taken directly, OR by overriding `QTcpServer::incomingConnection(int socketDescriptor)` to take sole ownership before any `QTcpSocket` is constructed.
     - Sole ownership of `client_fd` is transferred directly to `bb10_tls_connection_t`. A pending `QTcpSocket` is **never** constructed first.
  2. **Descriptor Sentinel**:
     - All uninitialized or released descriptors are initialized and invalidated to `-1`.
     - All descriptor validity checks strictly evaluate `if (m_fd >= 0)`. Zero (`0` / `STDIN_FILENO`) is recognized as a valid POSIX descriptor.
  3. **Sole I/O Channel**:
     - The adapter uses `QSocketNotifier` or POSIX `poll()` / `select()` strictly to monitor socket readiness (readable/writable events).
     - **All** physical socket reads and writes (`recv()`, `send()`) are executed exclusively by OpenSSL (`SSL_read()`, `SSL_write()`). Qt never reads ciphertext from the descriptor, completely eliminating ciphertext read competition and buffer conflicts.
  4. **Authoritative `WANT_READ` / `WANT_WRITE` Handling**:
     - Checking `errno == EAGAIN` is explicitly forbidden. OpenSSL's `SSL_get_error(ssl, rc)` is the sole authority.
     - If `SSL_write()` returns `SSL_ERROR_WANT_READ` (e.g., during TLS 1.3 KeyUpdate or renegotiation), the adapter monitors socket *readability* before retrying `SSL_write()`.
     - If `SSL_read()` returns `SSL_ERROR_WANT_WRITE`, the adapter monitors socket *writability* before retrying `SSL_read()`.
  5. **Teardown & Clean Closure**:
     - `bb10_tls_connection_close()` initiates a clean TLS shutdown (`SSL_shutdown()`, sending close-notify alert).
     - `bb10_tls_connection_destroy()` checks `if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }`, closing the owned fd once (including on failed handshakes) and invalidating the descriptor to `-1` to prevent double-close.
  6. **HTTP Protocol Layering**:
     - LocalSend's HTTP request/response parsing (e.g. `HttpConnection` parsing `/api/localsend/v2/prepare-upload`) sits strictly *above* decrypted I/O. `bb10_tls` delivers plain bytes; LocalSend processes plaintext.

---

### 3.3 Topic 3: Module Architecture & Relocatable CMake Package Contract (M3-D003)

```
+-------------------------------------------------------------------------------+
|                            Application Layer                                  |
|   Todoist Sync Engine (M3)            LocalSend-BB10 (SendManager/HttpServer) |
+------------------------------------+------------------------------------------+
|  Todoist Transport Layer           |  LocalSend Adapter Layer                 |
|  (ISyncTransport + SyncWorker)     |  (Pure Socket TLS + QSocketNotifier)     |
+------------------------------------+------------------------------------------+
|  Client HTTP Client                |  Raw TLS Socket API                      |
|  Trimmed libcurl 8.23.0            |  bb10_tls_connect() / bb10_tls_accept()  |
+------------------------------------+------------------------------------------+
|                          bb10_tls Core C / C++03 SDK                          |
|         Public PKI Context              P2P SHA-256 Fingerprint Context       |
+-------------------------------------------------------------------------------+
|             Isolated OpenSSL 3.5.9 LTS (-Wl,--exclude-libs,ALL)               |
+-------------------------------------------------------------------------------+
|                   QNX 6.6 POSIX Socket Layer (libsocket.so.3)                 |
+-------------------------------------------------------------------------------+
```

#### Relocatable CMake Export Package:
To ensure an external project (such as `LocalSend-BB10` or a standalone test consumer) can build without access to Todoist source or private build paths, `bb10_tls` installs a standard relocatable CMake package:
- **Package Config File**: `lib/cmake/bb10_tls/bb10_tls-config.cmake`
- **Imported Target**: `bb10_tls::bb10_tls` (STATIC IMPORTED).
- **Target Transitive Properties**:
  - `INTERFACE_INCLUDE_DIRECTORIES`: `${PACKAGE_PREFIX_DIR}/include`
  - `INTERFACE_LINK_LIBRARIES`:
    - `${PACKAGE_PREFIX_DIR}/lib/libbb10_tls.a`
    - `${PACKAGE_PREFIX_DIR}/lib/libssl.a`
    - `${PACKAGE_PREFIX_DIR}/lib/libcrypto.a`
    - `socket` (`libsocket.so.3`), `m`
  - `INTERFACE_LINK_OPTIONS`: `-Wl,--exclude-libs,ALL`
- **Installed Artifacts**:
  - `include/bb10_tls/bb10_tls.h`: Pure C public API with opaque struct pointers (`typedef struct bb10_tls_context bb10_tls_context_t;`). **Zero OpenSSL internal headers are exposed in public headers**.
  - `include/bb10_tls/bb10_tls.hpp`: Header-only C++03 RAII wrapper.
  - `lib/libbb10_tls.a`, `lib/libssl.a`, `lib/libcrypto.a`.
  - `share/bb10_tls/certs/cacert.pem`: Bundled Mozilla Root CA.
  - `share/bb10_tls/LICENSE`: Licenses for OpenSSL 3.5.9 and bb10_tls.
- **Context & Connection Ownership Rules**:
  - `bb10_tls_context_t` is thread-safe for read operations after initialization. Multiple connections can share a single context.
  - `bb10_tls_connection_t` is thread-bound (managed by a single thread at a time).
  - Context must outlive all active connections created from it.
  - Destroy order: All `bb10_tls_connection_t` instances must be destroyed before destroying the parent `bb10_tls_context_t`.
  - Error strings and codes have lifetime managed internally by the connection/context until the next API call on that handle.

#### Explicitly Deferred Scope:
1. **Loopback Proxy (Tier 2 Sidecar)**: **DEFERRED**. A loopback proxy rewrites URLs/origins, introduces security risks via unauthenticated local port listeners on the device, complicates process lifecycle, and creates QML `XMLHttpRequest` origin policy conflicts without a current necessity.
2. **Shared Library (`.so`) Packaging**: **DEFERRED**. On BlackBerry 10, BAR sandboxes isolate static applications reliably. Packaging shared libraries introduces loader search path complexities and symbol collision risks across sandboxes. Static embedding with `-Wl,--exclude-libs,ALL` is the preferred BB10 distribution model.
3. **Qt Declarative Bridge**: **DEFERRED** until a concrete consumer explicitly requires QObject signal/slot bindings.

---

### 3.4 Topic 4: Separation of Public PKI and P2P Pinning Policies (M3-D004)

#### Client/Server Role Authentication Matrix:

| Role | Policy Mode | Local Credentials | Peer Cert Required? | Verification & Validation Rules |
| :--- | :--- | :--- | :--- | :--- |
| **Client** | Public PKI (Todoist API) | None | **Mandatory** (Server Certificate) | 1. Mozilla Root CA chain verification.<br>2. SAN precedence (SAN checked first; CN used only if SAN absent).<br>3. Validity time window.<br>4. Fail-closed on missing/invalid CA.<br>5. TLS 0-RTT disabled on POST. |
| **Client** | P2P Pinning (LocalSend Send) | None (default) or optional client cert | **Mandatory** (Server Certificate) | 1. Bypasses Public Root CA validation.<br>2. Validates expected pin format (must be 64 hex chars).<br>3. Extracts peer DER certificate, computes SHA-256.<br>4. Rejects mismatch, missing pin, or absent cert before application data. |
| **Server** | P2P Pinning (LocalSend Receive) | In-Memory Self-Signed Cert + EC P-256 Key | **Not Required** (No mTLS in LocalSend v2.2) | 1. Server presents local certificate to connecting client.<br>2. Identity presentation only; client certificate verification disabled.<br>3. Does NOT require expected client pin or client certificate.<br>4. Optional mTLS is deferred. |

#### Detailed Policy Rules:
1. **Public PKI Policy (Todoist)**:
   - **Verification Rule**: Full X.509 certificate chain validation against bundled Mozilla CA bundle (`assets/certs/cacert.pem`).
   - **Fail-Closed Guarantee**: If CA bundle is absent, unreadable, or corrupted, handshake terminates immediately with `BB10_TLS_ERR_CA_LOAD_FAILED`. Never falls back to expired system trust stores (`/etc/ssl/certs`).
   - **SAN Precedence (RFC 6125)**: Subject Alternative Names (`dNSName`, `iPAddress`) take strict precedence over Common Name (`CN`). If a SAN is present, CN is completely ignored and cannot override conflicting SANs.
   - **Anti-Replay**: TLS 1.3 0-RTT is disabled for mutating HTTP requests (`POST`, `PUT`, `DELETE`).
   - **libcurl PKI Parity**: `todoist_network` enforces identical verification flags in libcurl (`CURLOPT_SSL_VERIFYPEER = 1L`, `CURLOPT_SSL_VERIFYHOST = 2L`, `CURLOPT_CAINFO = <path>`).
2. **P2P Verifying-Client Contract (`bb10_tls_connect` with P2P Policy)**:
   - **Pin Validation**: An expected server pin must be configured via `bb10_tls_context_set_expected_fingerprint()`.
   - **Pin Format**: Expected pin must be a valid SHA-256 string (exactly 64 hexadecimal characters, normalized case-insensitively with colons stripped). If missing or malformed, handshake fails closed before network I/O (`BB10_TLS_ERR_INVALID_PIN`).
   - **Peer Certificate Check**: If peer server presents no certificate during handshake, handshake aborts immediately with `BB10_TLS_ERR_NO_PEER_CERT`.
   - **Fingerprint Comparison**: Extracts peer certificate DER bytes, computes SHA-256 digest, and compares against expected pin. On mismatch, handshake aborts immediately before application data (`BB10_TLS_ERR_PIN_MISMATCH`).
3. **P2P Non-mTLS Server Contract (`bb10_tls_accept` with P2P Policy)**:
   - **Identity Presentation Only**: Server requires its own local certificate and private key (generated via `bb10_tls_generate_self_signed_cert()`).
   - **No Client Pin or Certificate**: Does NOT require an expected client pin, and does NOT request or require a client certificate. Normal clients connect and receive server's certificate without presenting client certificates.
   - **Optional mTLS**: Explicitly **DEFERRED** as LocalSend v2.2 does not use mutual TLS.
4. **Key & Certificate Lifetime**:
   - Server self-signed certificate and EC (P-256) private key are generated in-memory via `bb10_tls_generate_self_signed_cert()` at server startup.
   - Persisted in memory for the process lifetime; regenerated upon app restart.
5. **Protocol Reconcilement**: In LocalSend v2.2, peer discovery occurs via UDP multicast / `/api/localsend/v2/register`. First-contact trust is confirmed by the user at the application layer; `bb10_tls` strictly enforces the fingerprint match once provided.

---

### 3.5 Topic 5: Concurrency, Thread Synchronization & Lifecycle (M3-D005)

#### Transport Signature:
Strictly adheres to [sync_transport_interface.hpp](../include/todoist/sync/sync_transport_interface.hpp):
```cpp
virtual HttpResponse executeSyncPost(
    const std::string& endpointUrl,
    const std::string& requestBody,
    const std::string& bearerToken) = 0;
```

#### C++03 Thread Synchronization & Predicate Protection:
Plain `bool` variables are not thread-safe in C++03. Synchronization is enforced using POSIX primitives:
- `pthread_mutex_t m_stateMutex`: Protects all state variables (`m_state`, `m_stopping`, `m_wakeRequested`).
- `pthread_cond_t m_stateCond`: Signals state changes and backoff interruptions.
- Predicate loop:
  ```cpp
  pthread_mutex_lock(&m_stateMutex);
  while (!m_stopping && !m_syncRequested && ...) {
      pthread_cond_timedwait(&m_stateCond, &m_stateMutex, &ts);
  }
  pthread_mutex_unlock(&m_stateMutex);
  ```

#### Bounded Cancellation & Shutdown Ordering:
- **Cancellation of Blocking Transfer**:
  - `CURLOPT_XFERINFOFUNCTION` (or `CURLOPT_PROGRESSFUNCTION`) reads `m_stopping` under mutex lock. Returning non-zero immediately aborts the active curl transfer with `CURLE_ABORTED_BY_CALLBACK`.
  - Combined with `CURLOPT_CONNECTTIMEOUT_MS = 15000` and `CURLOPT_TIMEOUT_MS = 30000`, the worker thread join latency is strictly bounded.
- **Deterministic Teardown Sequence**:
  1. Acquire `m_stateMutex`, set `m_stopping = true`, release lock.
  2. Signal condition variable: `pthread_cond_signal(&m_stateCond)`.
  3. Abort active curl transfer via progress callback.
  4. Wait for worker completion: `pthread_join(m_thread, NULL)`.
  5. Destroy transport, database, and sync engine instances.

#### UI Notification Dispatch & Lifecycle Invalidation:
- **Value-Snapshot Notification Queue (Qt-Independent)**:
  - Concurrency decoupling between the worker thread and UI is achieved via a thread-safe value queue (`SyncNotificationQueue`).
  - Events placed into the queue are pure plain-old-data value snapshots containing **NO raw UI pointers, delegates, or callback functions**:
    ```cpp
    struct SyncEventSnapshot {
        SyncState state;
        int errorCode;
        std::string errorMessage;
        unsigned long timestamp;
        int itemsProcessed;
    };
    ```
  - The worker thread pushes snapshots under `m_stateMutex`.
  - The UI (or Cascades thread) polls/drains this queue on its own thread via:
    `void drainNotifications(std::vector<SyncEventSnapshot>& outEvents);`
  - **UI Destruction Invalidation**: When the UI is destroyed, it simply ceases calling `drainNotifications()`. Queued snapshots remain passive inert data that are deallocated safely when the queue or engine is destroyed. This structurally eliminates any possible dangling pointer dereference or callback to destroyed UI objects.

#### SQLite Database Thread-Confinement:
- The SQLite database connection (`TodoistStorage`) is strictly thread-confined to the `SyncWorkerThread`. The UI thread never accesses SQLite directly.

#### Retry & Backoff Architecture:
- **Stateless Transport**: `ISyncTransport::executeSyncPost` performs a single HTTP request with **no hidden retries**.
- **Worker-Owned Saturating Exponential Backoff with Full Jitter**:
  - Formula: `t_wait = uniform_random(0, min(MAX_BACKOFF, BASE_BACKOFF * (1 << min(attempt, MAX_EXPONENT))))`
  - Constants: `BASE_BACKOFF = 1.0s`, `MAX_BACKOFF = 60.0s`, `MAX_EXPONENT = 6` (saturates exponent at 64s, preventing integer overflow).
  - Network reachability events signal `m_stateCond` to interrupt backoff wait immediately for rapid reconnect.
- **Terminal vs Transient Error Classification & Retry Policy**:
  - **Terminal (Non-Spinning) Error States (`STATE_FATAL_ERROR`)**:
    - Invalid CA path / corrupt CA bundle (`BB10_TLS_ERR_CA_LOAD_FAILED`).
    - Public PKI verification failure / hostname mismatch / expired certificate (`BB10_TLS_ERR_CERT_VERIFY`).
    - P2P missing / malformed expected pin (`BB10_TLS_ERR_INVALID_PIN`).
    - P2P fingerprint mismatch (`BB10_TLS_ERR_PIN_MISMATCH`).
    - HTTP 400 Bad Request, 401 Unauthorized, 403 Forbidden.
    - *Behavior*: Immediately transitions to `STATE_FATAL_ERROR`. Completely halts the automatic retry loop (no spinning). Requires explicit user or configuration action (e.g. updating auth token, fixing CA bundle, updating network config) before an explicit `requestSync()` call resumes execution.
  - **Transient Network Errors (`STATE_BACKOFF_WAIT`)**:
    - TCP connection timeout / connect failure (`CURLE_COULDNT_CONNECT`, `ETIMEDOUT`).
    - DNS resolution failure (`CURLE_COULDNT_RESOLVE_HOST`).
    - HTTP 500/502/503/504 server error.
    - *Policy*: Continues retrying with saturating exponential backoff and full jitter:
      `t_wait = uniform_random(0, min(60.0, 1.0 * (1 << min(attempt, 6))))`.
      Retries continue at the saturated delay (60s) until explicitly stopped via shutdown, or interrupted immediately by a network reachability recovery event (`notifyReachabilityChanged(true)`).
  - `Retry-After` header parsing is explicitly deferred (current `HttpResponse` does not capture response headers).
  - **M2 Invariants Preserved**: If the server accepts a sync request but the TCP response is lost, the durable command UUIDs, idempotency tokens, and sync tokens prevent duplicate execution on subsequent retries.

---

## 4. Target Runtime Acceptance Gates (Resolving M3-D006)

To ensure TLS 1.3 is verifiably delivered rather than merely cross-compiled, M3 adheres to a four-gate verification plan with reproducible commands and explicit pending labels for hardware tests:

```
[Gate 1: Host Tests (Debian x86_64 Container)]
- Request/response serialization & header formatting
- Bounded memory buffers (10 MiB limit) & timeout enforcement
- Bounded backoff math (saturating full jitter) & worker thread cancellation
- Thread synchronization: mutex/condvar state loop & snapshot drain queue
- Terminal error classification: bad CA / bad pin / auth errors halt retry loop
- P2P verifying-client SHA-256 fingerprint parsing, normalization, malformed pin rejection & matching
- Entropy failure rejection test (fail-closed behavior)
- SAN precedence verification (SAN present, CN ignored)
- Pinned OpenSSL 3.5.9 host build (not host system OpenSSL)

            |
            v
[Gate 2: Target Cross-Compilation & Symbol Isolation]
- Cross-compile with Clang 19.1.7 (bb10-cc) against BB10 sysroot
- Inspect ELF dynamic symbol table (.dynsym) with Python ELF parser
- Assert 0 exported private TLS symbols (SSL_*, OPENSSL_*, EVP_*, etc.)
- Build and link relocatable CMake package: bb10_tls::bb10_tls
- Build standalone external consumer test strictly against installed prefix
- Build target coexistence test: Native binary linking Cascades/QtNetwork (libQtNetwork.so.4)
  and libbb10_tls.a in the same process

            |
            v
[Gate 3: Passport Target Client Probe & Runtime Coexistence]
*(Status: Pending hardware execution)*
- Deploy native CLI probe executable (`bb10_tls_probe`) and coexistence test to physical BlackBerry Passport.
- **Coexistence Execution on Target**:
  Execute `test_target_coexistence` on Passport: runs QtNetwork/Cascades legacy crypto and modern TLS 1.3 in the same process, asserting zero symbol collisions or layout corruption.
- **Controlled Local Server Fixture**:
  Run separate controlled OpenSSL servers on reachable host using pinned build:
  openssl s_server -accept 8443 -cert server.pem -key server.key -tls1_3 -www
  openssl s_server -accept 8442 -cert server.pem -key server.key -tls1_2 -www
- **Probe Client Execution**:
  - Controlled TLS 1.3 Handshake:
    bb10_tls_probe client --url https://<lan_host>:8443 --ca-bundle test_ca.pem --min-tls-version TLSv1.3 --max-tls-version TLSv1.3
    -> Assert and log negotiated protocol (TLSv1.3) and cipher suite (e.g. TLS_AES_256_GCM_SHA384).
  - Controlled TLS 1.2 Handshake:
    bb10_tls_probe client --url https://<lan_host>:8442 --ca-bundle test_ca.pem --min-tls-version TLSv1.2 --max-tls-version TLSv1.2
    -> Assert and log negotiated protocol (TLSv1.2).
  - Controlled Protocol Cross-Version Rejection:
    bb10_tls_probe client --url https://<lan_host>:8442 --ca-bundle test_ca.pem --min-tls-version TLSv1.3 --max-tls-version TLSv1.3
    -> Assert handshake failure (version mismatch).
- **Supplemental Smoke & Negative Tests**:
  - Smoke test against live Todoist: bb10_tls_probe client --url https://api.todoist.com/api/v1/sync --ca-bundle assets/certs/cacert.pem
  - Expired certificate rejection: bb10_tls_probe client --url https://expired.badssl.com --expect-error CERT_VERIFY_FAIL
  - Wrong hostname rejection: bb10_tls_probe client --url https://wrong.host.badssl.com --expect-error CERT_VERIFY_FAIL
  - Absent/corrupted CA bundle: bb10_tls_probe client --url https://api.todoist.com/api/v1/sync --ca-bundle non_existent.pem --expect-error CA_LOAD_FAIL

            |
            v
[Gate 4: Standalone P2P Server/Client Exchange]
*(Status: Pending hardware execution)*
- Execute independent consumer test binary on target:
  - Server binds port 53317 with in-memory transient self-signed cert (identity presentation only).
  - Verifying client connects, validates expected SHA-256 fingerprint, performs bidirectional streaming.
  - Negative test: Verifying client connects with mismatched expected pin -> assert PIN_MISMATCH before data exchange.
  - Negative test: Verifying client connects with malformed expected pin -> assert INVALID_PIN before network I/O.
  - Bounded memory streaming and clean close-notify shutdown.
```

### Deliverable Artifacts:
1. `modules/bb10_tls/libbb10_tls.a` and public C headers `include/bb10_tls/bb10_tls.h`.
2. Relocatable CMake package: `lib/cmake/bb10_tls/bb10_tls-config.cmake`.
3. `src/network/` implementation of `ISyncTransport` and `SyncWorkerThread`.
4. Standalone external consumer test: `tests/standalone_consumer/test_consumer.cpp`.
5. Diagnostic probe executable: `bin/bb10_tls_probe` with reproducible CLI commands (`client`, `server`, `fingerprint`).

---

## 5. Review Summary & Milestone 3 Scope Agreement

By adopting the architectural resolutions above:
- **Engine Selection (M3-D001, Closed)**: OpenSSL 3.5.9 LTS selected; link-time `-Wl,--exclude-libs,ALL` verified; runtime coexistence explicitly scheduled under Gate 2/3; LocalSend migration contract defined; entropy failure checked.
- **Framing & Server Decoupling (M3-D002)**: Trimmed `libcurl 8.23.0` powers client HTTP with memory bounds; raw socket TLS 1.3 with sole socket ownership, native accept / incomingConnection override, descriptor sentinel `-1`, and authoritative `SSL_get_error()` powers LocalSend client and server roles.
- **Packaging & Scoping (M3-D003, Closed)**: Relocatable CMake package (`bb10_tls::bb10_tls`) and external consumer test are primary deliverables; loopback proxy and shared libraries are deferred.
- **Trust Policies (M3-D004)**: Client/server role matrix defined; P2P verifying-client contract separated from non-mTLS server identity presentation; SAN precedence over CN; libcurl PKI parity.
- **Lifecycle & Contract (M3-D005)**: Parameter order is corrected, worker lifecycle uses POSIX mutex/condvar protection, value-snapshot queue eliminates UI use-after-free, DB is thread-confined, terminal vs transient error retry policies are formalized with saturating full jitter.
- **Runtime Gates (M3-D006)**: Controlled local `openssl s_server` TLS 1.2/1.3 endpoints with min/max version flags and cipher logging, Passport runtime coexistence execution, and explicit pending hardware labels.
