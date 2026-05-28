# cppandroidtest — C++/NDK Android template + on-device profiler

A **forkable starting point** for a serious C++ Android project, plus a built-in
**hardware profiler**. It's the plumbing that normally costs days — NDK + CMake +
C++23, a Kotlin↔C++ JNI bridge, four CI workflows, a tagged-release pipeline, a
native crash handler, and an optional **physical rooted-device test rig** — wired
together and battle-tested.

It is **not** a game engine or a world-scale app skeleton. It's a clean base a
**solo dev** forks to start a native Android project (game, DSP, ML inference,
firmware tooling, emulator…), with a profiler that tells you the performance
envelope of your target devices before you write a line of your own code.

---

## Use it

1. Click **"Use this template"** on GitHub (or fork).
2. Make it yours — one command from the repo root:

   ```bash
   bash scripts/init-template.sh com.yourname.app "Your App" [libname] [--minimal]
   ```

   This renames the package everywhere it's coupled — the Kotlin source
   **directory**, the **JNI symbol names** (`Java_<pkg>_MainActivity_*`, which
   must stay in lockstep or the app crashes at first JNI call), `applicationId`,
   the native lib, the app display name — then statically validates the result.
   `--minimal` also strips the rooted-device test rig (keep it only if you have
   a device like the author's).

3. `git add -A && commit && push`. CI builds the renamed APK. Done.

> Can't fully build locally without the NDK — `init-template.sh` does strong
> static checks (no stale identifiers, JNI⇄Kotlin package lockstep); CI confirms
> the build.

---

## Module map — keep what you need

| Layer | Lives in | Keep if… | Drop by |
|---|---|---|---|
| **Core scaffold** | `app/` (build.gradle, CMakeLists, MainActivity.kt, jni.cpp), `.github/workflows/{smoke,quality,tests,release}.yml` | always | — |
| **Profiler** | `app/src/main/cpp/bench/**`, `scripts/{build-bench,run-bench,compare,dashboard}` | you want CPU/memory/thermal/sensor/camera measurement | delete `bench/`, drop the registry calls in `jni.cpp`/`bench_main.cpp` |
| **Device-test rig** | `scripts/{device-harness,ui_tap,aggregate-runs,live-dashboard}`, `.github/workflows/device-test.yml` | you have a rooted test phone + self-hosted runner | `--minimal` at init, or `git rm` them |

The profiler and device rig are **opt-out**, not load-bearing — the core builds
and ships an APK without them.

---

## Build & run

```bash
# APK (debug): needs Android Studio's Gradle or a standalone gradle on PATH
gradle wrapper --gradle-version 8.7 --distribution-type bin   # once
./gradlew assembleDebug          # app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug           # build + adb install

# Standalone profiler ELF (arm64): no APK needed
bash scripts/build-bench.sh                                   # build/bench-arm64/cppbench
adb push build/bench-arm64/cppbench /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/cppbench
adb shell /data/local/tmp/cppbench --json                    # benchmarks
adb shell /data/local/tmp/cppbench --sensors                 # all sensors
adb shell /data/local/tmp/cppbench --cameras                 # all cameras

# Host unit tests (no device)
cmake -S tests -B tests/build -G Ninja && cmake --build tests/build && ctest --test-dir tests/build

# Release: tag it
git tag v1.0.0 && git push origin v1.0.0   # CI builds + publishes APK & ELF
```

## What the profiler measures

CPU `stream` (DRAM bandwidth), `latency` (cache hierarchy), `neon_fma` (FP32/FP16
GFLOPS), `dot_int8` (SDOT/UDOT), `i8mm` + `sve2` (ARMv9, opt-in), `perf_counters`
(PMU via `perf_event_open`), `sustained` (thermal-throttling curve) — each pinned
per CPU cluster. Plus full sensor & Camera2 enumeration and an NDJSON `--stream`
mode with a terminal dashboard. Extension benches are runtime-gated on
`getauxval(AT_HWCAP)` so they no-op (never SIGILL) on unsupported silicon.

## CI workflows

| Workflow | Runs on | Does |
|---|---|---|
| `smoke` | GitHub emulator (API 30, x86_64) | install APK, launch, assert it lives |
| `quality` | GitHub-hosted | clang-tidy + lizard cyclomatic complexity |
| `tests` | GitHub-hosted | host unit tests (doctest) |
| `release` | GitHub-hosted | build + publish APK & cppbench ELF on `v*` tags |
| `device-test` | self-hosted (optional) | bench + UI smoke on a real rooted device |

## Deeper docs

[`CLAUDE.md`](CLAUDE.md) — architecture, the `-u ANativeActivity_onCreate` /
HWCAP / per-file `-march` gotchas, the device rig, and the calibrated Exynos 9825
baseline.

Built with Claude Code. Debug-signed releases (not Play-Store eligible).
