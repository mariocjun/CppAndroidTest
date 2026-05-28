#!/usr/bin/env bash
# Smoke check: installs APK in connected emulator/device, launches NativeActivity,
# verifies process stays alive and android_main reached.
#
# Called from .github/workflows/smoke.yml inside the
# reactivecircus/android-emulator-runner shell, which runs each line of the
# action's `script:` field as a separate `sh -c`. To keep the multi-line
# validation logic working, we put it in this file and the workflow just runs
# `bash scripts/smoke-check.sh`.
set -euo pipefail

APK_PATH="${APK_PATH:-app/build/outputs/apk/debug/app-debug.apk}"
PACKAGE="${PACKAGE:-com.example.cppandroidtest}"
ACTIVITY="${ACTIVITY:-com.example.cppandroidtest.MainActivity}"
LOG_TAG="${LOG_TAG:-CppAndroidTest}"
# JNI_OnLoad fires from MainActivity's static initialiser
# (System.loadLibrary("cppandroidtest")) as soon as the class is loaded.
EXPECT_LOG="${EXPECT_LOG:-JNI_OnLoad: libcppandroidtest.so ready}"
WAIT_SECONDS="${WAIT_SECONDS:-8}"

echo "==> Uninstalling any previous install (debug keystore differs between CI runners)"
adb uninstall "$PACKAGE" 2>/dev/null || true

echo "==> Installing $APK_PATH"
adb install -r "$APK_PATH"

adb logcat -c
echo "==> Launching $PACKAGE/$ACTIVITY"
adb shell am start -W -n "$PACKAGE/$ACTIVITY"

echo "==> Sleeping ${WAIT_SECONDS}s"
sleep "$WAIT_SECONDS"

PID=$(adb shell pidof "$PACKAGE" || true)
adb logcat -d > logcat-full.txt
adb logcat -d -s "$LOG_TAG" > native.log || true

echo "::group::Native log ($LOG_TAG tag)"
cat native.log
echo "::endgroup::"

if [ -z "$PID" ]; then
    echo "::error::Process $PACKAGE is not running after ${WAIT_SECONDS}s"
    echo "::group::FATAL/DEBUG entries from logcat"
    grep -E "FATAL|DEBUG|tombstone|signal|backtrace" logcat-full.txt | head -100 || true
    echo "::endgroup::"
    exit 1
fi

echo "OK: process alive, PID=$PID"

if ! grep -q "$EXPECT_LOG" native.log; then
    echo "::error::Expected log line '$EXPECT_LOG' not found - android_main never reached?"
    echo "    Possible cause: missing '-u ANativeActivity_onCreate' linker flag in CMakeLists.txt"
    exit 1
fi

echo "OK: android_main reached (found '$EXPECT_LOG' in logcat)"
