#!/usr/bin/env bash
# Push cppbench to a connected device, run it, save JSON results locally.
#
# Usage:
#   scripts/run-bench.sh <label> [adb-serial] [extra cppbench args...]
#
# Examples:
#   scripts/run-bench.sh exynos                       # picks the only adb device
#   scripts/run-bench.sh snapdragon RF8M12345ABC      # specific serial
#   scripts/run-bench.sh exynos auto --iters=20       # override iterations
#
# Output: results/<label>-<timestamp>.json
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <label> [adb-serial|auto] [extra cppbench args...]" >&2
    echo "  label    short name for this device (e.g. 'exynos', 'snapdragon')" >&2
    exit 2
fi

LABEL="$1"; shift || true
SERIAL="${1:-auto}"; shift || true

ADB_FLAGS=()
if [ "$SERIAL" != "auto" ] && [ -n "$SERIAL" ]; then
    ADB_FLAGS=(-s "$SERIAL")
fi

BENCH_BIN="build/bench-arm64/cppbench"
if [ ! -f "$BENCH_BIN" ]; then
    echo "$BENCH_BIN not found. Running scripts/build-bench.sh..."
    bash scripts/build-bench.sh
fi

DEVICES=$(adb devices | awk 'NR>1 && $2=="device" {print $1}' | wc -l)
if [ "$DEVICES" -eq 0 ]; then
    echo "ERROR: no adb device connected." >&2
    exit 1
fi
if [ "$SERIAL" = "auto" ] && [ "$DEVICES" -gt 1 ]; then
    echo "ERROR: multiple adb devices connected; pass an explicit serial." >&2
    adb devices >&2
    exit 1
fi

TS=$(date +%Y%m%d-%H%M%S)
mkdir -p results
OUT="results/${LABEL}-${TS}.json"
STDERR_OUT="results/${LABEL}-${TS}.stderr.log"

REMOTE=/data/local/tmp/cppbench
echo "==> Push $BENCH_BIN -> $REMOTE"
adb "${ADB_FLAGS[@]}" push "$BENCH_BIN" "$REMOTE" >/dev/null
adb "${ADB_FLAGS[@]}" shell chmod 755 "$REMOTE"

echo "==> Run --json $*"
# Pipe stdout to local file, stderr to separate log
adb "${ADB_FLAGS[@]}" shell "$REMOTE" --json "$@" 1>"$OUT" 2>"$STDERR_OUT" || {
    rc=$?
    echo "cppbench exited $rc — stderr:" >&2
    cat "$STDERR_OUT" >&2
    exit $rc
}

# Strip CRLF that adb shell sometimes injects
sed -i 's/\r$//' "$OUT" 2>/dev/null || true

echo "==> Saved: $OUT"
echo "Preview:"
head -c 1500 "$OUT" | sed -e 's/,/,\n  /g' | head -40
echo
echo "Cleanup remote: adb ${ADB_FLAGS[*]} shell rm $REMOTE"
