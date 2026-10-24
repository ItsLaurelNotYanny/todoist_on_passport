#!/usr/bin/env bash
# Project-gate: configure stack checks before use. Checks must not modify source.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
FAST=0
case "${1:-}" in
    "") [ "$#" -eq 0 ] || { echo "ERROR: unexpected arguments" >&2; exit 2; } ;;
    --fast) [ "$#" -eq 1 ] || { echo "ERROR: unexpected arguments" >&2; exit 2; }; FAST=1 ;;
    *) echo "Usage: ./check.sh [--fast]" >&2; exit 2 ;;
esac
MODE=full
[ "$FAST" -eq 0 ] || MODE=fast
umask 077
LOG=$(mktemp "${TMPDIR:-/tmp}/project-gate.XXXXXX")
trap 'echo "ERROR: gate interrupted; log=$LOG" >&2; exit 2' INT TERM
need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: missing required tool: $1; log=$LOG" >&2
        exit 2
    fi
}
COUNT=0
run() {
    local label=$1 status
    shift
    printf '\n--- %s ---\n' "$label" >>"$LOG"
    if "$@" >>"$LOG" 2>&1; then
        COUNT=$((COUNT + 1))
    else
        status=$?
        printf 'FAIL: %s (exit=%s); log=%s\n' "$label" "$status" "$LOG" >&2
        tail -n 60 "$LOG" | { head -c 8000; cat >/dev/null; } >&2
        printf '\n' >&2
        case "$status" in 126|127) exit 2 ;; *) exit 1 ;; esac
    fi
}

# --- stack checks (C++ / CMake / CTest) ---
need cmake
need make
need python3
need git

run "public path hygiene" python3 -B scripts/check-public-paths.py
run "dependencies integrity" python3 -B scripts/dependencies.py
run "cmake configure" cmake -B build -S .
run "cmake build" cmake --build build --target test_storage test_sync test_bb10_tls test_standalone_consumer test_network test_ui bb10_tls_probe test_target_coexistence -j"$(nproc)"

if [ "$FAST" -eq 0 ]; then
    run "ctest (storage + sync + tls + consumer + network + ui)" ctest --test-dir build --output-on-failure --no-tests=error
    run "host symbol isolation check" python3 -B scripts/check-symbol-isolation.py build/bb10_tls_probe build/test_network build/test_ui
    run "relocated consumer package check" bash scripts/check-installed-consumer.sh
    run "bar descriptor and assets check" python3 -B -c 'import xml.etree.ElementTree as ET, pathlib, struct; ET.parse("bar-descriptor.xml"); icon=pathlib.Path("assets/icon.png").read_bytes(); assert icon[:8]==b"\x89PNG\r\n\x1a\n" and struct.unpack_from(">II",icon,16)==(144,144); assert pathlib.Path("assets/main.qml").is_file() and pathlib.Path("assets/ActiveFrameCover.qml").is_file()'
fi

# --- custom checks (preserve during refresh) ---
# Host-only workflow checks do not change the BB10 runtime stack.
if [ -d "tools/alternating-review/scripts" ]; then
    run "workflow Python syntax" python3 -B -c 'import ast,pathlib; [ast.parse(p.read_text(),filename=str(p)) for p in pathlib.Path("tools/alternating-review/scripts").glob("*.py")]'
fi
run "scripts Python syntax" python3 -B -c 'import ast,pathlib; [ast.parse(p.read_text(),filename=str(p)) for p in pathlib.Path("scripts").glob("*.py")]'
if [ -d ".githooks" ]; then
    run "gate hook shell syntax" bash -c 'for hook in .githooks/pre-commit .githooks/pre-push; do [ ! -f "$hook" ] || bash -n "$hook" || exit; done'
fi
if [ "$FAST" -eq 0 ] && [ -f "tests/workflow/test_reminder.py" ]; then
    run "document reminder tests" python3 -B tests/workflow/test_reminder.py
fi
[ "$COUNT" -gt 0 ] || { echo "ERROR: no checks ran; log=$LOG" >&2; exit 2; }
printf 'OK: project-gate (%s, checks=%s)\n' "$MODE" "$COUNT"
rm -f -- "$LOG"
