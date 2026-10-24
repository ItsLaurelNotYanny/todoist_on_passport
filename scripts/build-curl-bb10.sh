#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/build/bb10-deps"
SRC_DIR="$ROOT/build/dependency-sources-bb10"

KEY=$(cat "$ROOT/dependencies.lock.json" "$PREFIX/openssl.complete" "$ROOT/scripts/build-curl-bb10.sh" | sha256sum | cut -d ' ' -f 1)
STAMP="$PREFIX/curl.complete"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$KEY" ]; then
    exit 0
fi

rm -rf "$SRC_DIR/curl-8.23.0"
mkdir -p "$SRC_DIR" "$PREFIX"
python3 -c "import tarfile; tarfile.open('$ROOT/vendor/sources/curl-8.23.0.tar.xz').extractall('$SRC_DIR')"

cd "$SRC_DIR/curl-8.23.0"
CC=bb10-cc AR=/opt/bb10-toolchain/bin/arm-unknown-nto-qnx6.5.0eabi-ar RANLIB=/opt/bb10-toolchain/bin/arm-unknown-nto-qnx6.5.0eabi-ranlib \
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" CPPFLAGS="-I$PREFIX/include" \
./configure --host=arm-unknown-nto-qnx6.5.0eabi --prefix="$PREFIX" \
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

make -C lib CPPFLAGS="-I$PREFIX/include" -j"$(nproc)"
make -C lib install
make -C include install

printf '%s\n' "$KEY" > "$STAMP"
echo "libcurl 8.23.0 BB10 build complete at $PREFIX"
