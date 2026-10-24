#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$ROOT/build/host-deps"
SRC_DIR="$ROOT/build/dependency-sources"

KEY=$(cat "$ROOT/dependencies.lock.json" "$PREFIX/openssl.complete" "$ROOT/scripts/build-curl-host.sh" | sha256sum | cut -d ' ' -f 1)
STAMP="$PREFIX/curl.complete"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$KEY" ]; then
    exit 0
fi

mkdir -p "$SRC_DIR" "$PREFIX"
tar -xf "$ROOT/vendor/sources/curl-8.23.0.tar.xz" -C "$SRC_DIR"

cd "$SRC_DIR/curl-8.23.0"
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" CPPFLAGS="-I$PREFIX/include" \
    ./configure --prefix="$PREFIX" \
    --disable-shared --enable-static --with-openssl="$PREFIX" \
    --disable-docs --disable-ldap --disable-ldaps \
    --disable-ftp --disable-file --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smtp --disable-rtsp \
    --disable-gopher --disable-mqtt --disable-ipfs --disable-cookies \
    --disable-alt-svc --disable-hsts --disable-websockets --enable-threaded-resolver \
    --disable-openssl-auto-load-config \
    --without-zlib --without-brotli --without-zstd --without-libpsl \
    --without-libidn2 --without-nghttp2 --without-nghttp3 --without-libssh2 \
    --without-ca-path --without-ca-bundle CFLAGS="-fPIC"

make -C lib -j"$(nproc)"
make -C lib install
make -C include install

printf '%s\n' "$KEY" > "$STAMP"
echo "libcurl 8.23.0 host build complete at $PREFIX"
