# CLAUDE.md

Guidance for Claude Code (and any AI agent) working in this repository.
**This repo is a template** — a forkable C++/NDK Android base + on-device
profiler. If you're an agent that just landed here, read this top-to-bottom
once; it's the map. Cross-tool agents: see `AGENTS.md`. Invokable capabilities:
`.claude/skills/`.

> Everything down to "Author's device rig" is **generic** — true for any fork.
> The fenced section at the very bottom is the original author's specific
> hardware and is the only part you should replace when you fork.

## What this is

A Kotlin **`MainActivity`** (AppCompat) that loads `libcppandroidtest.so` and
calls into C++ through a thin **JNI bridge** (`app/src/main/cpp/jni.cpp`). The
C++ side is a profiler/benchmark harness under `app/src/main/cpp/bench/`. There
is no NativeActivity and no `android_native_app_glue` — an earlier revision used
those; they were removed when the UI moved to MainActivity. Don't reintroduce
the `-u ANativeActivity_onCreate` linker flag; it's gone for a reason.

**The load-bearing invariant:** the JNI function names in `jni.cpp`
(`Java_com_example_cppandroidtest_MainActivity_*`) must match the Kotlin
package + class exactly, or the app crashes at the first native call. The
package appears in ~5 coupled places (Kotlin dir path, package decl, JNI
symbols, `applicationId`, `namespace`). **Never rename the package by hand** —
run `scripts/init-template.sh` (it moves the Kotlin dir and rewrites the JNI
symbols in lockstep).

## Project map

```
app/
  build.gradle.kts            AGP config: minSdk 29, compileSdk 34, NDK 26.1.10909125,
                              Kotlin, -std=c++23 via CMAKE_CXX_STANDARD
  src/main/
    AndroidManifest.xml       MainActivity launcher, INTERNET perm (paste.rs upload)
    kotlin/.../MainActivity.kt UI (programmatic, no XML): buttons w/ content-desc
                              test IDs (btn_run, btn_hwcaps, ...) + paste.rs upload
    cpp/
      CMakeLists.txt          harden_target() = strict warnings + -O3 + LTO +
                              dead-code stripping; per-file -march for dot_int8/i8mm/sve2
      jni.cpp                 JNI bridge + native crash handler (SA_SIGINFO -> dump)
      bench/
        registry.h            Benchmark<T> concept + Registry tuple + fold dispatch
        json.h                dependency-free JSON emitter (unit-tested)
        hwcaps.{h,cpp}        getauxval(AT_HWCAP) feature gates (authoritative)
        diag.{h,cpp}          current_bench tag read by the crash handler
        affinity / soc_info / timer   cluster detection, /proc fingerprint, timing
        cpu/                  stream, latency, neon_fma, dot_int8, i8mm, sve2,
                              perf_counters, sustained
        sensors/ camera/ stream/   ASensorManager enum, Camera2 enum, NDJSON streamer
      bench_main.cpp          standalone cppbench ELF entry (CLI)
tests/                        host doctest unit tests (no NDK): json, registry concept
scripts/                      build/run/profile/template tooling (see below)
.github/workflows/            smoke, quality, tests, release, device-test
```

## Build / test / profile / release

```bash
# APK (needs Android Studio's Gradle or a standalone gradle on PATH)
gradle wrapper --gradle-version 8.7 --distribution-type bin   # once (wrapper is gitignored)
./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug

# Standalone profiler ELF (arm64-v8a) — no APK
bash scripts/build-bench.sh    # -> build/bench-arm64/cppbench (sets BUILD_BENCH_EXECUTABLE=ON)
adb shell /data/local/tmp/cppbench --json|--sensors|--cameras|--stream|--list

# Host unit tests
cmake -S tests -B tests/build -G Ninja && cmake --build tests/build && ctest --test-dir tests/build

# Release: tag triggers build+publish (APK + ELF) and, via release.yml's final
# step, dispatches the physical device-test.
git tag v1.2.3 && git push origin v1.2.3
```

## Architecture invariants & gotchas (don't relearn these the hard way)

- **JNI⇄Kotlin package lockstep** — see above. `init-template.sh` is the only
  safe renamer.
- **HWCAP gating, not /proc/cpuinfo** — extension benches (dot_int8/i8mm/sve2/
  neon_fma fp16) check `bench::has_*()` (`getauxval(AT_HWCAP/AT_HWCAP2)`) before
  executing. /proc/cpuinfo lies on some kernels (lists a feature the kernel
  disabled for userspace → SIGILL). Always gate new SIMD on HWCAP.
- **Per-file `-march`** — `dot_int8.cpp` (`+dotprod`), `i8mm.cpp` (`armv8.6-a+i8mm`),
  `sve2.cpp` (`armv9-a+sve2`) get per-source `-march` via `set_source_files_properties`
  (arm64-v8a only). Project-wide stays baseline armv8-a so the autovectorizer
  can't emit those instructions into other TUs and SIGILL on older cores.
- **C++23 on Clang 17 (NDK r26b)** = `-std=c++2b`, NOT `-std=c++23` (that alias
  is Clang 18+). Don't hard-code `-std=` in Gradle; let `CMAKE_CXX_STANDARD 23`
  emit the right flag.
- **MSYS path mangling** (Windows git-bash): `export MSYS_NO_PATHCONV=1` for adb
  `/sdcard`,`/data/...` args, but turn it OFF (`MSYS_NO_PATHCONV=0`) for local
  `python.exe` calls (it mangles `/c/...`). `ui_tap.py` calls adb directly so
  it's immune.
- **Ephemeral debug keystore** — each CI runner generates its own debug key, so
  a newer release APK won't install over an older one
  (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`). Always `adb uninstall` before install
  (smoke-check.sh and device-harness ui-test do this).
- **Single-run mobile numbers are noise** (DVFS + scheduler). Any perf claim
  needs `full-stats` (≥5 runs, median + CV); root + `performance` governor for
  clean numbers.

## Extending: add a benchmark

The one extension point you'll use most. See `.claude/skills/add-benchmark/`
for the step-by-step. In short: write `bench/cpu/yourbench.{h,cpp}` exposing
`run_yourbench_per_cluster(...)`, add a wrapper struct to `bench/registry.h`'s
tuple (the `Benchmark<T>` concept makes a malformed wrapper a compile error),
add the source to `CMakeLists.txt`. `bench_main.cpp` and the JNI bridge don't
change — dispatch is automatic.

## CI workflows

| Workflow | Runs on | Purpose |
|---|---|---|
| `smoke` | GitHub emulator (API 30 x86_64) | install APK + launch + assert process alive + JNI_OnLoad log |
| `quality` | GitHub-hosted | clang-tidy + lizard cyclomatic complexity (informational artifacts) |
| `tests` | GitHub-hosted | host doctest unit tests |
| `release` | GitHub-hosted | build + publish APK & cppbench ELF on `v*`; dispatches device-test |
| `device-test` | self-hosted (optional) | bench (full-stats) + content-desc UI smoke on a real rooted device |

`device-test` is **owner-only** (`workflow_dispatch` + a `repository_owner`
guard) — never `pull_request`, because a self-hosted runner on a public repo
must be unreachable by fork PRs.

## scripts/

`build-bench.sh` (build ELF) · `run-bench.sh` / `compare.py` (push+run+diff) ·
`device-harness.sh` (rooted-device session: probe/root/pin-perf/install/bench/
ui-test) · `ui_tap.py` (locate UI by content-desc, not coordinates) ·
`aggregate-runs.py` (median/CV over N runs) · `dashboard.py` / `live-dashboard.sh`
(live NDJSON terminal view) · `init-template.sh` (fork rename engine) ·
`smoke-check.sh` (emulator smoke logic).

---

<!-- ================= AUTHOR'S DEVICE RIG — replace for your project ================= -->
## Author's device rig (example — replace when you fork)

The original author's physical tester is a **rooted SM-N975F (Exynos 9825)**,
serial `REDACTED_SERIAL` (Tailscale `REDACTED_HOST:5555`), rooted via Magisk in the
recovery slot (lineage: a sibling WiFi-CSI project's `flash_f2.sh`). The
`device-harness.sh` device registry, the `device-test.yml` runner labels
(`self-hosted, n975f`), and the baseline below are all specific to that rig.
**If you fork:** edit the `device-harness.sh` registry to your device(s), or
`init-template.sh --minimal` to remove the device rig entirely.

Calibrated Exynos 9825 baseline (root, `performance` governor, median of 7):

| metric | A55 @1.95G | A75 @2.40G | M4 @2.73G |
|---|---|---|---|
| NEON FP32 FMA (GFLOPS) | 13.8 | 19.1 | 45.0 |
| SDOT int8 (GOps) | 61.9 | 38.2 | 173.3 |
| STREAM triad (GB/s) | 6.5 | 17.7 | 23.0 |
| IPC (PMU, FMA loop) | 1.25 | 1.66 | 1.67 |

Finding: the Cortex-A75 has **half-rate DotProd** (0.5 SDOT/cycle vs A55's 1.0,
M4's 2.0) — real, reproducible (CV 0%), not measurement noise. Self-hosted
runner `redacted-runner`; persisted via a current-user logon Task Scheduler task,
or `svc.cmd install` as admin for a true service.

## Local dev environment (author)

Windows + git-bash/MSYS2; Android Studio is the supported IDE (CLion's Android
support is limited). Build runs against NDK 26.1.10909125.
