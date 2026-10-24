#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_ORIG=$(mktemp -d "${TMPDIR:-/tmp}/bb10_tls_install_orig.XXXXXX")
INSTALL_RELOC=$(mktemp -d "${TMPDIR:-/tmp}/bb10_tls_install_reloc.XXXXXX")
rm -rf "$INSTALL_RELOC" # Destination for relocation move
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/bb10_tls_consumer_test.XXXXXX")
trap 'rm -rf "$INSTALL_ORIG" "$INSTALL_RELOC" "$WORK_DIR"' EXIT

# 1. Install to original prefix
cmake --install "$ROOT/build" --prefix "$INSTALL_ORIG" >/dev/null

# Verify contract documentation and license presence
test -f "$INSTALL_ORIG/share/bb10_tls/LICENSE" || { echo "FAIL: missing installed LICENSE" >&2; exit 1; }
test -f "$INSTALL_ORIG/share/bb10_tls/README.md" || { echo "FAIL: missing installed README.md contract" >&2; exit 1; }

# 2. Relocate installation directory (tests relocatability of CMake configs)
mv "$INSTALL_ORIG" "$INSTALL_RELOC"

# 3. Independent pure C consumer test
mkdir -p "$WORK_DIR/c_consumer"
cat << 'EOF' > "$WORK_DIR/c_consumer/CMakeLists.txt"
cmake_minimum_required(VERSION 3.5)
project(PureCConsumer LANGUAGES C)
find_package(bb10_tls REQUIRED PATHS "${BB10_TLS_PREFIX}" NO_DEFAULT_PATH)

add_executable(consumer_c main.c)
target_link_libraries(consumer_c PRIVATE bb10_tls::bb10_tls)
target_link_options(consumer_c PRIVATE -rdynamic)
EOF

cat << 'EOF' > "$WORK_DIR/c_consumer/main.c"
#include <bb10_tls/bb10_tls.h>
#include <stdio.h>

int main(void) {
    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    if (!ctx) return 1;
    bb10_tls_context_destroy(ctx);
    return 0;
}
EOF

cmake -S "$WORK_DIR/c_consumer" -B "$WORK_DIR/c_consumer/build" -DBB10_TLS_PREFIX="$INSTALL_RELOC" >/dev/null
cmake --build "$WORK_DIR/c_consumer/build" >/dev/null
"$WORK_DIR/c_consumer/build/consumer_c" >/dev/null

# 4. Independent C++ consumer test
mkdir -p "$WORK_DIR/cpp_consumer"
cat << 'EOF' > "$WORK_DIR/cpp_consumer/CMakeLists.txt"
cmake_minimum_required(VERSION 3.5)
project(PureCppConsumer LANGUAGES CXX)
find_package(bb10_tls REQUIRED PATHS "${BB10_TLS_PREFIX}" NO_DEFAULT_PATH)

add_executable(consumer_cpp main.cpp)
target_link_libraries(consumer_cpp PRIVATE bb10_tls::bb10_tls)
target_link_options(consumer_cpp PRIVATE -rdynamic)
EOF

cat << 'EOF' > "$WORK_DIR/cpp_consumer/main.cpp"
#include <bb10_tls/bb10_tls.hpp>
#include <iostream>

int main() {
    bb10_tls::Context ctx(BB10_TLS_POLICY_P2P);
    if (!ctx.isValid()) return 1;
    return 0;
}
EOF

cmake -S "$WORK_DIR/cpp_consumer" -B "$WORK_DIR/cpp_consumer/build" -DBB10_TLS_PREFIX="$INSTALL_RELOC" >/dev/null
cmake --build "$WORK_DIR/cpp_consumer/build" >/dev/null
"$WORK_DIR/cpp_consumer/build/consumer_cpp" >/dev/null

# 5. Verify strict symbol isolation on both consumer binaries
python3 "$ROOT/scripts/check-symbol-isolation.py" \
    "$WORK_DIR/c_consumer/build/consumer_c" \
    "$WORK_DIR/cpp_consumer/build/consumer_cpp" >/dev/null

echo "OK: Relocated pure C and C++ consumers built and verified cleanly from $INSTALL_RELOC"
