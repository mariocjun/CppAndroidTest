#!/usr/bin/env bash
# Live terminal dashboard for an attached device. Pushes cppbench, runs it
# in --stream mode, pipes the NDJSON output through dashboard.py.
#
# Usage:
#   bash scripts/live-dashboard.sh                 # auto-detect single device
#   bash scripts/live-dashboard.sh <adb-serial>    # specific device
#   ADB=adb.exe bash scripts/live-dashboard.sh     # override adb binary
#
# Ctrl-C stops streaming and restores cursor. The remote cppbench process
# receives SIGPIPE when the local pipe closes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

SERIAL="${1:-}"
ADB="${ADB:-adb}"
PYTHON="${PYTHON:-python3}"

BENCH_BIN="build/bench-arm64/cppbench"
if [ ! -f "$BENCH_BIN" ]; then
    echo "Building cppbench..."
    bash scripts/build-bench.sh
fi

ADB_FLAGS=()
if [ -n "$SERIAL" ]; then
    ADB_FLAGS=(-s "$SERIAL")
fi

REMOTE=/data/local/tmp/cppbench
echo "==> Pushing $BENCH_BIN -> $REMOTE"
"$ADB" "${ADB_FLAGS[@]}" push "$BENCH_BIN" "$REMOTE" >/dev/null
"$ADB" "${ADB_FLAGS[@]}" shell chmod 755 "$REMOTE"

# Resolve python on Windows (python3 may not exist on some installs)
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    if command -v python >/dev/null 2>&1; then
        PYTHON=python
    fi
fi

echo "==> Streaming live (Ctrl-C to stop)"
# 'adb shell' adds \r to every line on some platforms; strip them before json parses.
"$ADB" "${ADB_FLAGS[@]}" shell "$REMOTE" --stream "$@" \
    | tr -d '\r' \
    | "$PYTHON" "$SCRIPT_DIR/dashboard.py"
