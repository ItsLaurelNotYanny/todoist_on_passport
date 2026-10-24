#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$ROOT/build/host-deps"
SRC_DIR="$ROOT/build/dependency-sources"

KEY=$(cat "$ROOT/dependencies.lock.json" "$ROOT/scripts/build-openssl-host.sh" | sha256sum | cut -d ' ' -f 1)
STAMP="$PREFIX/openssl.complete"

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$KEY" ]; then
    exit 0
fi

mkdir -p "$SRC_DIR" "$PREFIX"
tar -xf "$ROOT/vendor/sources/openssl-3.5.9.tar.gz" -C "$SRC_DIR"

cd "$SRC_DIR/openssl-3.5.9"
./Configure linux-x86_64 --prefix="$PREFIX" --libdir=lib \
    no-shared no-tests no-apps no-autoload-config no-dso no-module no-asm \
    no-async no-engine no-legacy no-quic -DOPENSSL_NO_SECURE_MEMORY -DNO_SYSLOG -fPIC

make -j"$(nproc)" build_libs
make install_dev

printf '%s\n' "$KEY" > "$STAMP"
echo "OpenSSL 3.5.9 host build complete at $PREFIX"
