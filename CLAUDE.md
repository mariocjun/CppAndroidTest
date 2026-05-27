# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A **pure C++ Android app** using `android.app.NativeActivity`. There is **no Java or Kotlin code** — `AndroidManifest.xml` declares `android:hasCode="false"`. The Activity lifecycle is bridged into C++ via the NDK's `android_native_app_glue` static library, and the app's entry point is `android_main(android_app*)` in `app/src/main/cpp/main.cpp`.

The Manifest's `<meta-data android:name="android.app.lib_name" android:value="cppandroidtest"/>` is what wires the OS to the shared library — it loads `libcppandroidtest.so` and calls `ANativeActivity_onCreate`. If the native library name in `CMakeLists.txt` changes, this value must change with it.

### Critical CMake detail

`CMakeLists.txt` sets:
```cmake
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")
```
This `-u` flag forces the linker to keep `ANativeActivity_onCreate` (from `android_native_app_glue`) as an exported symbol. Without it the dynamic linker can't find the entry point and the app crashes immediately on launch with no useful logcat output. Do not remove.

## Build

The Gradle wrapper is **not committed** (see `.gitignore` — `gradle/`, `gradlew`, `gradlew.bat` are excluded). On a fresh checkout you must bootstrap it once:

```bash
gradle wrapper --gradle-version 8.7 --distribution-type bin
```

This requires a standalone `gradle` binary on PATH (or use Android Studio's bundled one). After bootstrap:

```bash
./gradlew assembleDebug         # produces app/build/outputs/apk/debug/app-debug.apk
./gradlew assembleRelease       # produces app/build/outputs/apk/release/app-release.apk
./gradlew installDebug          # build + adb install to connected device/emulator
./gradlew clean
```

There are **no tests** in this project (no unit tests, no instrumentation tests). Do not invent test commands.

## Release flow

Releases are tag-driven. Pushing a tag matching `v*` triggers `.github/workflows/release.yml`, which builds `assembleRelease` and attaches the APK to a GitHub Release named after the tag.

```bash
git tag v0.2.0
git push origin v0.2.0
```

Released APKs land at `https://github.com/mariocjun/CppAndroidTest/releases/tag/<tag>`. The workflow also accepts manual triggers via `workflow_dispatch` (Actions tab → "Build and Release APK" → Run workflow), but those runs only upload an artifact — they do not create a Release (the `if: startsWith(github.ref, 'refs/tags/v')` guard).

## Version pinning — keep these in sync

Three places reference the toolchain versions. Changing one without the others will break CI:

| Setting | `app/build.gradle.kts` | `.github/workflows/release.yml` |
|---|---|---|
| `compileSdk` / platforms | `compileSdk = 34` | `platforms;android-34` |
| Build tools | (implicit, via AGP) | `build-tools;34.0.0` |
| NDK | `ndkVersion = "26.1.10909125"` | `ndk;26.1.10909125` |
| CMake | `version = "3.22.1"` | `cmake;3.22.1` |

AGP version (currently `8.5.2`) lives in root `build.gradle.kts`. AGP 8.5 requires JDK 17 and Gradle ≥ 8.6 — the workflow uses JDK 17 and Gradle 8.7.

## Signing

`buildTypes.release` in `app/build.gradle.kts` uses `signingConfigs.getByName("debug")` — release APKs are signed with the Android SDK debug key. This makes them installable on any device but **rejected by Google Play**. To publish for real, generate a keystore, register it as a `signingConfigs.create("release") { ... }`, and gate the secrets via the workflow's `secrets.*` context.

## QA / runtime verification

**Smoke test in CI:** `.github/workflows/smoke.yml` runs on every push to `main` and every PR. It installs the debug APK into an x86_64 AVD (API 30), launches the NativeActivity, and fails the build if:
- the process is dead 8s after `am start` (catches segfaults in `android_main`)
- the literal string `"Hello from C++ Native Activity"` doesn't appear in logcat (catches the case where `libcppandroidtest.so` loads but the entry point isn't reached — typically a missing `-u ANativeActivity_onCreate` linker flag)

If you change `LOG_TAG` or the initial `LOGI()` message in `main.cpp`, update the grep in `smoke.yml` too.

The AVD is cached between runs (`actions/cache@v4` keyed on `avd-30-x86_64-v1`) — bump the key to force a clean recreate.

**Performance benchmarking** is a separate concern — the user runs these on physical Galaxy Note10+ devices (Exynos 9825 and Snapdragon 855 variants). The benchmark code, when added, lives under `app/src/main/cpp/bench/` and is intended to be invocable both via the APK (as a JNI/native call from the NativeActivity) and as a standalone ELF executable pushed via `adb push ... /data/local/tmp/`. Desktop emulators (BlueStacks/MEmu/LDPlayer/Nox) are not used and are not CI-compatible.

## Local development environment

The author works on Windows. CLion's Android support is limited; **Android Studio is the supported IDE** for syncing and running this project. The `.idea/` directory and IDE-specific files are gitignored. Bash commands assume git-bash on Windows or WSL — paths use forward slashes inside scripts.
