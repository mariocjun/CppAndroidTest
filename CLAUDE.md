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

## Performance benchmarking — `cppbench`

The benchmark harness under `app/src/main/cpp/bench/` exists to compare **Galaxy Note10+ Exynos 9825 (SM-N975F)** against **Note10+ Snapdragon 855 (SM-N975U)**. It is intentionally NOT run in CI — perf numbers from a virtualized x86_64 emulator are meaningless. CI only compile-checks the harness (the bench sources are linked into `libcppandroidtest.so`, so any compilation failure surfaces through the smoke workflow).

### Build the standalone ELF

`scripts/build-bench.sh` runs CMake with the NDK toolchain directly, sets `-DBUILD_BENCH_EXECUTABLE=ON`, and produces `build/bench-arm64/cppbench` (ARM64 PIE ELF). It auto-locates NDK 26.x at the standard Android Studio install paths; override with `ANDROID_NDK=/path/to/ndk`.

### Push and run on a device

```bash
# One device connected via USB:
bash scripts/run-bench.sh exynos                  # label this run "exynos"
bash scripts/run-bench.sh snapdragon              # later, on the other device

# Multiple devices — pass adb serial:
bash scripts/run-bench.sh exynos RF8M12345ABC

# Pass extra args to cppbench:
bash scripts/run-bench.sh exynos auto --filter=stream --iters=20
```

JSON output lands in `results/<label>-<timestamp>.json`. The script auto-rebuilds the binary if missing.

### Compare two runs

```bash
python scripts/compare.py results/exynos-*.json results/snapdragon-*.json
```

Prints per-cluster STREAM bandwidth with ratios. Extend with more metrics as benchmarks are added.

### Benchmark list

| Name | What it measures | Method | Where it lives |
|---|---|---|---|
| `stream` | DRAM bandwidth GB/s (copy/scale/add/triad) | McCalpin STREAM on 16 MB float arrays, best-of-N | `bench/cpu/stream.cpp` |
| `latency` | L1d/L2/SLC/DRAM latency in ns/load | Pointer-chase, random Hamiltonian cycle, 4 KB → 64 MB sweep | `bench/cpu/latency.cpp` |
| `neon_fma` | Single-core peak GFLOPS (FP32+FP16) | `vfmaq_f32`/`vfmaq_f16` × 8 chains, gated on `fphp`/`asimdhp` | `bench/cpu/neon_fma.cpp` |
| `dot_int8` | ARMv8.2-A SDOT/UDOT GOps/s | `vdotq_s32`/`vdotq_u32` × 8 chains, gated on `asimddp`. The TU is compiled with per-file `-march=armv8.2-a+dotprod` so other TUs don't acquire DOTPROD via autovectorization (would SIGILL on Exynos 9825 A75 cores). | `bench/cpu/dot_int8.cpp` |
| `sustained` | GFLOPS vs time + thermal + freq throttling curve | NEON FMA pinned to big core for `duration_sec`, sampling temps + freqs every `sample_interval_ms` (auto-rescaled chunk size to keep cadence honest as kernel throttles). Opt-in via `--filter=sustained` (heavy). | `bench/cpu/sustained.cpp` |

CLI: `cppbench [--json] [--filter=NAME] [--iters=N] [--elems=N] [--list]`. Each benchmark is run per cluster (LITTLE / mid / big) using `sched_setaffinity` to pin to that cluster's CPUs. Cluster topology is discovered at runtime from `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` — no SoC-specific hardcoding.

### Live dynamic stream (sensors + thermal + freqs)

`cppbench --stream` emits NDJSON to stdout (one JSON object per line) continuously until SIGINT:

1. **header line** — once at startup. Contains `env` (SoC, model, Android version, num CPUs), `thermal_zones` (every `/sys/class/thermal/thermal_zone*` discovered), `sensors_meta` (full enumeration via `ASensorManager_getSensorList`), and `cameras` (full enumeration via `ACameraManager_getCameraIdList` + characteristics — no capture).
2. **frame lines** — emitted every `--interval-ms` (default 250 ms). Each frame has `temps_mc` (array of milicelsius per zone), `freqs_khz` (array of current cpufreq per CPU), and `sensors` (array of latest readings, one per active sensor).
3. **footer line** — once when SIGINT received.

Sensors enabled automatically: every CONTINUOUS and ON_CHANGE sensor at `--sensor-rate-us` (default 50 000 µs = 20 Hz). ONE_SHOT and SPECIAL sensors are skipped by default. The C++ side calls `fflush(stdout)` after every line so adb-shell pipes update in real time.

#### Live terminal dashboard

`scripts/dashboard.py` reads the NDJSON from stdin and re-renders a terminal dashboard with ANSI escape codes (CPU freqs in GHz with green-for-active colouring, thermal zones with traffic-light colouring keyed on °C, every active sensor with named axes — `x/y/z` for vectors, `v` for scalars, etc.). Requires Python 3.10+.

`scripts/live-dashboard.sh` is the one-shot end-to-end: push cppbench, run it on-device in stream mode, pipe through dashboard.py. Strips CR injected by `adb shell` on some platforms so the JSON parser doesn't reject lines.

```bash
bash scripts/live-dashboard.sh                    # auto-detect single device
bash scripts/live-dashboard.sh RF8M12345ABC       # specific serial
```

### One-shot enumeration

```bash
cppbench --sensors    # JSON dump of all sensors (handle, type, name, vendor,
                      # string_type, resolution, min_delay_us, reporting_mode,
                      # FIFO depth — everything ASensor_* exposes)
cppbench --cameras    # JSON dump of every Camera2 camera (facing, hw_level,
                      # capabilities, focal lengths, sensor physical size,
                      # pixel array geometry, ISO range, exposure range,
                      # every output stream config — format/resolution/input)
```

### Methodology

For reproducible numbers between Exynos and Snapdragon devices:

1. Same Android version and security patch (`getprop ro.build.fingerprint`).
2. Battery ≥ 80%, charger plugged, display on at max brightness.
3. Airplane mode + Do Not Disturb to kill background scheduling noise.
4. 3 warm-up runs, 5 measured runs; report median + IQR.
5. 10-minute cooldown between benchmark sets (Exynos 9825 throttles within ~3-5 min of sustained big-cluster load).

### Architecture notes

The harness is built into both `libcppandroidtest.so` (currently no runtime trigger; serves as CI compile-check) and the standalone `cppbench` ELF (gated by the `BUILD_BENCH_EXECUTABLE` CMake option, set only by `scripts/build-bench.sh`). NEON intrinsics are guarded by `#if defined(__aarch64__)` so the x86_64 .so build for the smoke emulator still compiles — non-arm64 paths return sentinel `-1` GFLOPS.

Desktop emulators (BlueStacks/MEmu/LDPlayer/NoxPlayer) are not used. They translate ARM instructions for x86 hosts; perf numbers are meaningless and the NEON paths don't execute natively anyway.

### Adding a new benchmark

The project compiles as **C++20** (NDK 26 ships Clang 17, which supports the full C++20 surface — concepts, ranges, designated init, fold expressions, three-way comparison). Benchmarks are registered in `bench/registry.h` via a typelist tuple and dispatched with a fold expression:

```cpp
struct MyBench {
    static constexpr const char* name = "my_bench";
    using Config = MyConfig;
    static Config make_config(const Args& a) { /* map CLI args -> Config */ }
    static bool opt_in() { return false; }  // true = only runs under --filter
    bench::Json run_per_cluster(const Config& cfg,
                                const std::vector<bench::CpuCluster>& cl) const {
        return ::my_run(cfg, cl);
    }
};

// In registry.h:
using Registry = std::tuple<..., MyBench>;
```

The `Benchmark<T>` concept + `static_assert(all_benchmarks(...))` at the bottom of `registry.h` make a malformed wrapper a compile error rather than a silent miscompile. `bench_main.cpp` does not change when you add a benchmark — `dispatch()` iterates the tuple automatically.

C++26 (reflection P2996) isn't in Clang 17 yet, so the per-Config `make_config` mapping is still hand-written. When/if reflection lands, that boilerplate should collapse into a single generic visitor.

## Local development environment

The author works on Windows. CLion's Android support is limited; **Android Studio is the supported IDE** for syncing and running this project. The `.idea/` directory and IDE-specific files are gitignored. Bash commands assume git-bash on Windows or WSL — paths use forward slashes inside scripts.
