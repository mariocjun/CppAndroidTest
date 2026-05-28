#!/usr/bin/env bash
# Build the standalone cppbench ELF for arm64-v8a using the NDK toolchain
# directly (not through Gradle/AGP — AGP doesn't produce executables).
#
# Requires:
#   - ANDROID_NDK env var pointing at NDK 26.x, OR
#   - $HOME/Android/Sdk/ndk/26.1.10909125 present (default Android Studio path)
#
# Output: build/bench-arm64/cppbench
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

ABI="${ABI:-arm64-v8a}"
PLATFORM="${PLATFORM:-android-29}"  # ASensor_getHandle / getInstanceForPackage
BUILD_TYPE="${BUILD_TYPE:-Release}"
# Output dir uses the short ABI form (arm64 instead of arm64-v8a) because
# this script's primary use case is the standalone arm64 bench and all the
# downstream consumers (release.yml, run-bench.sh, live-dashboard.sh, smoke
# CI compile-check) reference build/bench-arm64/cppbench. Override via
# BUILD_DIR=... if you need per-ABI output dirs.
case "$ABI" in
    arm64-v8a)   DEFAULT_DIR="build/bench-arm64" ;;
    armeabi-v7a) DEFAULT_DIR="build/bench-armv7" ;;
    x86_64)      DEFAULT_DIR="build/bench-x86_64" ;;
    *)           DEFAULT_DIR="build/bench-${ABI}" ;;
esac
BUILD_DIR="${BUILD_DIR:-$DEFAULT_DIR}"

# Locate NDK
if [ -z "${ANDROID_NDK:-}" ]; then
    for candidate in \
        "$HOME/Android/Sdk/ndk/26.1.10909125" \
        "$HOME/Library/Android/sdk/ndk/26.1.10909125" \
        "/c/Users/$USER/AppData/Local/Android/Sdk/ndk/26.1.10909125"; do
        if [ -d "$candidate" ]; then ANDROID_NDK="$candidate"; break; fi
    done
fi
if [ -z "${ANDROID_NDK:-}" ] || [ ! -d "$ANDROID_NDK" ]; then
    echo "ERROR: NDK not found. Set ANDROID_NDK env var to your NDK 26.x path." >&2
    exit 1
fi
echo "Using NDK: $ANDROID_NDK"

TOOLCHAIN="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "ERROR: toolchain file not found at $TOOLCHAIN" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
cmake -S app/src/main/cpp -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$PLATFORM" \
    -DANDROID_STL=c++_static \
    -DBUILD_BENCH_EXECUTABLE=ON \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

cmake --build "$BUILD_DIR" --target cppbench -j

OUT="$BUILD_DIR/cppbench"
if [ ! -f "$OUT" ]; then
    echo "ERROR: build succeeded but $OUT not found." >&2
    exit 1
fi

# Quick sanity: should be ARM64 ELF
file "$OUT" || true
echo
echo "Built: $OUT"
echo "Push:  adb push $OUT /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/cppbench"
