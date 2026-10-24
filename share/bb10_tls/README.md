# bb10_tls: Universal Modern TLS 1.3 Subsystem for BlackBerry 10

## 1. File Descriptor (fd) Ownership Contract

- **Creation Transfer**: When calling `bb10_tls_connection_create(ctx, socket_fd)`:
  - If creation **fails** (returns `NULL`), the socket descriptor remains owned by the caller, and the caller is responsible for calling `close(socket_fd)`.
  - If creation **succeeds** (returns a non-NULL `bb10_tls_connection_t*`), sole ownership of `socket_fd` transfers unconditionally to the connection object.
- **Destruction Cleanup**: Calling `bb10_tls_connection_destroy(conn)` closes the underlying socket file descriptor exactly once and frees all associated connection memory and SSL state.
- **No Follow-up Close**: The caller **MUST NOT** close `socket_fd` after calling `bb10_tls_connection_destroy(conn)`. Doing so would close a stale descriptor number that may have already been reallocated by the kernel to another thread's socket or file.

## 2. Handle Lifetime and Thread Safety

- **Context Lifetime**: `bb10_tls_context_t*` must outlive all `bb10_tls_connection_t*` handles created from it. Destroying a context while connections are still active causes undefined behavior.
- **String Ownership**:
  - `bb10_tls_get_last_error(conn)` returns a pointer to internal static or connection-managed storage valid until the next operation on `conn`. The caller must not modify or free this string.
  - `bb10_tls_generate_self_signed_cert(cert_out, cert_len, key_out, key_len)` allocates heap buffers for the PEM outputs. The caller must release each allocated string with `bb10_tls_free_string()`.
- **Thread Safety**: Distinct `bb10_tls_connection_t` handles may be operated concurrently on separate threads. Simultaneous access to a single connection handle across threads requires external synchronization.

## 3. Nonblocking I/O and Retry-Buffer Rules

- For nonblocking sockets (e.g. `O_NONBLOCK`), operations (`bb10_tls_connect`, `bb10_tls_accept`, `bb10_tls_read`, `bb10_tls_write`, `bb10_tls_shutdown`) may return `BB10_TLS_ERR_WANT_READ` or `BB10_TLS_ERR_WANT_WRITE`.
- When `BB10_TLS_ERR_WANT_READ` is returned, wait for readability on the underlying socket (e.g. via `select`, `poll`, or an event loop) before retrying.
- When `BB10_TLS_ERR_WANT_WRITE` is returned, wait for writability on the underlying socket before retrying.
- **Buffer Stability Rule**: When retrying an interrupted or nonblocking write (`bb10_tls_write`), the caller MUST pass the exact same buffer pointer and length parameter as the previous invocation. Modifying or reallocating the buffer between retries violates TLS record boundaries.

## 4. Shutdown Semantics

- `bb10_tls_shutdown(conn)` transmits the local TLS `close_notify` alert.
- Once the local `close_notify` alert is sent (or if the peer closure has already been received), the connection transitions to the closed state and `bb10_tls_shutdown` returns `BB10_TLS_OK` (0).
- Any subsequent call to `bb10_tls_shutdown` on a connection that has reached the closed state returns `BB10_TLS_ERR_CLOSED` (-3).
- Any subsequent call to `bb10_tls_read` or `bb10_tls_write` on a connection that has reached the closed state returns `BB10_TLS_ERR_IO_FAILED` (-13), distinguishing post-shutdown I/O attempts from a peer closure alert received during active transfer (`BB10_TLS_ERR_CLOSED` / -3).
- In nonblocking mode, if alert transmission cannot complete immediately, `bb10_tls_shutdown` returns `BB10_TLS_ERR_WANT_WRITE` or `BB10_TLS_ERR_WANT_READ`. The application must wait for socket readiness and retry until `BB10_TLS_OK` or an error is returned.

## 5. Canonical Consumer Example (C99 / C++03)

```c
#include <bb10_tls/bb10_tls.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int fetch_status(const char* host, int port, const char* ca_bundle) {
    // 1. Create client context
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_PUBLIC_PKI);
    if (!ctx) return -1;

    if (ca_bundle && bb10_tls_context_set_ca_path(ctx, ca_bundle) != BB10_TLS_OK) {
        bb10_tls_context_destroy(ctx);
        return -2;
    }

    // 2. Open underlying TCP socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        bb10_tls_context_destroy(ctx);
        return -3;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock); // socket closed by caller since connection creation was not reached
        bb10_tls_context_destroy(ctx);
        return -4;
    }

    // 3. Create TLS connection handle (transfers socket ownership upon success)
    bb10_tls_connection_t* conn = bb10_tls_connection_create(ctx, sock);
    if (!conn) {
        close(sock); // socket closed by caller because creation failed
        bb10_tls_context_destroy(ctx);
        return -5;
    }
    // Note: sock is now owned by conn! Caller must NOT close(sock) from this point on.

    // 4. Perform TLS handshake
    if (bb10_tls_connect(conn, host) != BB10_TLS_OK) {
        fprintf(stderr, "TLS error: %s\n", bb10_tls_get_last_error(conn));
        bb10_tls_connection_destroy(conn); // automatically closes sock!
        bb10_tls_context_destroy(ctx);
        return -6;
    }

    // 5. Send HTTP probe
    const char* req = "GET / HTTP/1.0\r\n\r\n";
    size_t written = 0;
    bb10_tls_write(conn, req, strlen(req), &written);

    // 6. Graceful TLS shutdown
    bb10_tls_shutdown(conn);

    // 7. Destroy connection (closes underlying socket exactly once)
    bb10_tls_connection_destroy(conn);

    // 8. Destroy context
    bb10_tls_context_destroy(ctx);
    return 0;
}
```
