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

## 5. Consumer Examples and Build Entry

`bb10_tls` is developed in this project and distributed under MIT; bundled OpenSSL
retains its Apache 2.0 license. See [LICENSE](LICENSE).

Use the [root build instructions](../../README.md#building-from-source) for the
pinned host and BB10 environments. The exported CMake target is
`bb10_tls::bb10_tls`; it links the static TLS libraries and required platform
runtime libraries without exposing OpenSSL in the public API.

For runnable examples with checked handshake, I/O and teardown behavior, see:

- [Standalone consumer](../../tests/standalone_consumer/test_consumer.cpp), built
  against the installed SDK prefix.
- [TLS tests](../../tests/tls/test_bb10_tls_all.cpp).
- [Diagnostic CLI](../../src/tools/bb10_tls_probe.cpp).

These are developer examples. Phone installation is described in the
[root README](../../README.md#installation).
