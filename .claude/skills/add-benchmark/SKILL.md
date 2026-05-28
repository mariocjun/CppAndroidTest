---
name: add-benchmark
description: Add a new CPU/SIMD benchmark to the cppbench profiler. Use when the user wants to add, write, or wire a new benchmark, microbenchmark, or perf probe to this Android C++ project — covers the registry wrapper, the Benchmark<T> concept, HWCAP runtime gating, per-file -march for ISA extensions, and CMake wiring. Triggers: "add a benchmark", "new benchmark", "measure X throughput", "add a perf probe", "benchmark the GPU/crypto/AES/BF16".
---

# Add a benchmark to cppbench

The profiler dispatches benchmarks through a typelist registry. Adding one is
local — `bench_main.cpp` and `jni.cpp` never change. Follow these steps; the
`Benchmark<T>` concept turns any mistake into a compile error.

## 1. Write the benchmark source

Create `app/src/main/cpp/bench/cpu/<name>.{h,cpp}` (or `bench/gpu/...` etc.).
Mirror an existing one — `neon_fma` is the cleanest template for compute,
`stream` for memory. Required shape:

```cpp
// <name>.h
#pragma once
#include "../json.h"
#include "../affinity.h"
namespace bench::cpu {
struct <Name>Config { int iterations = 7; int warmup = 2; /* knobs */ };
// Runs once per detected CPU cluster (pin -> measure -> wrap in Json).
bench::Json run_<name>_per_cluster(const <Name>Config& cfg,
                                   const std::vector<bench::CpuCluster>& clusters);
}
```

The `.cpp` does the measurement. **Conventions that matter:**

- **Timing:** `bench::now_ns()` (CLOCK_MONOTONIC_RAW). Report best-of-N for
  throughput, median elsewhere. Defeat dead-code elimination with a
  `volatile` sink.
- **Pin per cluster:** use `bench::pin_to_cpu(cluster.max_id)` /
  `pin_to_cluster`. Clusters come from `bench::detect_clusters()`.
- **Sentinel:** return `-1` for any metric the device can't produce (so the
  aggregator skips it).

## 2. If it uses an ISA extension (NEON-dotprod / i8mm / SVE / BF16 / crypto)

Two gates, both required:

- **Compile-time:** guard the intrinsics with the feature macro
  (`#if defined(__aarch64__) && defined(__ARM_FEATURE_*)`), stub to `-1` otherwise.
- **Per-file `-march`:** in `CMakeLists.txt`, inside the
  `if(ANDROID_ABI STREQUAL "arm64-v8a")` block, add
  `set_source_files_properties(bench/cpu/<name>.cpp PROPERTIES COMPILE_OPTIONS "-march=<arch>")`.
  Pick the minimal arch (`armv8.2-a+dotprod`, `armv8.6-a+i8mm`, `armv9-a+sve2`,
  `armv8-a+crypto`). **Never** put the extension `-march` on the whole project —
  the autovectorizer would emit those instructions into other TUs and SIGILL on
  cores that lack them.
- **Runtime gate:** before executing, check `bench::has_*()` from
  `bench/hwcaps.h` (`getauxval(AT_HWCAP/AT_HWCAP2)` — authoritative; /proc/cpuinfo
  lies). Add a new `has_<feature>()` to `hwcaps.{h,cpp}` if needed (the HWCAP bit
  constants are already defined there).

## 3. Register it

In `app/src/main/cpp/bench/registry.h`:

```cpp
#include "cpu/<name>.h"          // with the other includes

struct <Name>Bench {
    static constexpr const char* name = "<name>";
    using Config = bench::cpu::<Name>Config;
    static Config make_config(const Args& a) { Config c; if (a.iters>0) c.iterations=a.iters; return c; }
    static bool opt_in() { return false; }   // true = only runs under --filter=<name>
    bench::Json run_per_cluster(const Config& cfg, const std::vector<bench::CpuCluster>& cl) const {
        return bench::cpu::run_<name>_per_cluster(cfg, cl);
    }
};

using Registry = std::tuple< ... , <Name>Bench>;   // add to the tuple
```

Mark `opt_in() { return true; }` if it's heavy (>5s) or ARMv9-exclusive (so the
default run on older devices doesn't waste time / report only `-1`).

## 4. Wire CMake + aggregator

- Add `bench/cpu/<name>.cpp` to `BENCH_SOURCES` in `CMakeLists.txt`.
- If it has scalar throughput metrics you want in `full-stats`, add an entry to
  `SCALAR_METRICS` in `scripts/aggregate-runs.py` (`"<name>": ("<name>", ["metric_key", ...])`).

## 5. Verify

- Host: `cmake -S tests -B tests/build -G Ninja && ctest --test-dir tests/build`
  (the registry `static_assert` runs at compile time — a malformed wrapper
  fails here).
- Compile-check across ABIs: push to a branch; the `smoke` workflow builds the
  `.so` for arm64/armv7/x86_64 (your guards must compile on all three).
- On-device: `bash scripts/device-harness.sh bench <serial> <name>` (or
  `full-stats` for median/CV).

## Pitfalls

- The registry dispatch lambda is `auto&&` over a materialised `Registry reg{}`
  (lvalue) — don't change it to `auto&` or `Registry{}` (prvalue), it won't
  compile (std::apply forwarding).
- Replacement order in any tooling that touches the lib name: the package
  string contains the lib name as a substring.
