// Sustained-performance / thermal-throttling benchmark.
//
// What it shows: how peak GFLOPS degrades over wall-clock time as the SoC
// heats up and the kernel's thermal governor drops cpufreq. Plotting
// gflops-vs-t_ms against temps_mc-vs-t_ms is the Note10+ Exynos vs Snapdragon
// money shot — Exynos 9825 typically holds peak for 2-4 min then throttles
// to 60-70% of peak; SD855 holds for 5-8 min.
//
// Method:
//   1. Pin to the fastest core (Prime on SD, Mongoose on Exynos).
//   2. Enumerate thermal zones once.
//   3. Loop: run a NEON FMA chunk sized to take ~sample_interval_ms, then
//      sample (temps, freqs, measured chunk GFLOPS) into a row. Repeat until
//      duration_sec elapsed.
//   4. Emit all rows in the result JSON.
//
// The chunk size auto-adapts: after each chunk we compute how long it took
// and rescale inner_iters_per_chunk for the next one to target the configured
// sample interval. This keeps the cadence honest as the chip throttles —
// otherwise late chunks would balloon to seconds each.
#include "sustained.h"

#include "../timer.h"
#include "../thermal/sysfs.h"

#if defined(__aarch64__)
#  include <arm_neon.h>
#  define SUSTAIN_HAS_NEON 1
#else
#  define SUSTAIN_HAS_NEON 0
#endif

#include <algorithm>
#include <vector>

namespace bench::cpu {

namespace {

#if SUSTAIN_HAS_NEON
volatile float g_sink_sustained = 0.0f;

// Returns measured GFLOPS for the chunk and updates t_ns_out with elapsed ns.
double run_fma_chunk(int64_t outer_iters, uint64_t& t_ns_out) {
    float32x4_t a0 = vdupq_n_f32(0.1f);
    float32x4_t a1 = vdupq_n_f32(0.2f);
    float32x4_t a2 = vdupq_n_f32(0.3f);
    float32x4_t a3 = vdupq_n_f32(0.4f);
    float32x4_t a4 = vdupq_n_f32(0.5f);
    float32x4_t a5 = vdupq_n_f32(0.6f);
    float32x4_t a6 = vdupq_n_f32(0.7f);
    float32x4_t a7 = vdupq_n_f32(0.8f);
    const float32x4_t b = vdupq_n_f32(1.0000001f);
    const float32x4_t c = vdupq_n_f32(0.9999999f);

    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) {
        a0 = vfmaq_f32(a0, b, c);
        a1 = vfmaq_f32(a1, b, c);
        a2 = vfmaq_f32(a2, b, c);
        a3 = vfmaq_f32(a3, b, c);
        a4 = vfmaq_f32(a4, b, c);
        a5 = vfmaq_f32(a5, b, c);
        a6 = vfmaq_f32(a6, b, c);
        a7 = vfmaq_f32(a7, b, c);
    }
    uint64_t t1 = now_ns();
    t_ns_out = t1 - t0;

    float32x4_t acc = vaddq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)),
                                vaddq_f32(vaddq_f32(a4, a5), vaddq_f32(a6, a7)));
    float lanes[4];
    vst1q_f32(lanes, acc);
    g_sink_sustained = lanes[0] + lanes[1] + lanes[2] + lanes[3];

    double flops = static_cast<double>(outer_iters) * 8 * 4 * 2;  // 8 chains * 4 lanes * 2 FLOPs
    double secs = static_cast<double>(t_ns_out) * 1e-9;
    return flops / secs / 1e9;
}
#else
double run_fma_chunk(int64_t outer_iters, uint64_t& t_ns_out) {
    // No-op on non-arm64 (only the smoke emulator's x86_64 .so build hits this).
    // Spin-wait to consume wall time so the surrounding loop still terminates.
    volatile int64_t s = 0;
    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) s += i;
    uint64_t t1 = now_ns();
    t_ns_out = t1 - t0;
    return 0.0;
}
#endif

} // namespace

Json run_sustained(const SustainedConfig& cfg) {
    auto zones = bench::thermal::list_thermal_zones();
    int n_cpus = bench::num_cpus();

    Json result;
    result.kv("duration_sec", cfg.duration_sec)
          .kv("sample_interval_ms", cfg.sample_interval_ms);

    std::vector<Json> zone_meta;
    zone_meta.reserve(zones.size());
    for (const auto& z : zones) {
        Json zj;
        zj.kv("idx", z.idx).kv("type", z.type);
        zone_meta.push_back(std::move(zj));
    }
    result.kv("thermal_zones", zone_meta);

    std::vector<Json> samples;
    int64_t inner = cfg.inner_iters_per_chunk;
    const uint64_t t_start = now_ns();
    const uint64_t t_end_target = t_start
        + static_cast<uint64_t>(cfg.duration_sec) * 1'000'000'000ull;
    const uint64_t target_chunk_ns =
        static_cast<uint64_t>(cfg.sample_interval_ms) * 1'000'000ull;

    while (now_ns() < t_end_target) {
        uint64_t chunk_ns = 0;
        double gflops = run_fma_chunk(inner, chunk_ns);
        auto temps = bench::thermal::sample_temps_mc(zones);
        auto freqs = bench::thermal::sample_cpu_freqs_khz(n_cpus);

        Json row;
        row.kv("t_ms", static_cast<int64_t>((now_ns() - t_start) / 1'000'000ull))
           .kv("gflops", gflops)
           .kv("chunk_ns", chunk_ns);
        // temps_mc and freqs_khz as arrays of numbers
        std::vector<double> temps_d(temps.begin(), temps.end());
        std::vector<double> freqs_d(freqs.begin(), freqs.end());
        row.kv("temps_mc", temps_d);
        row.kv("freqs_khz", freqs_d);
        samples.push_back(std::move(row));

        // Auto-scale next chunk to target the sample interval (within 4x bounds).
        if (chunk_ns > 0) {
            double scale = static_cast<double>(target_chunk_ns) / static_cast<double>(chunk_ns);
            scale = std::min(4.0, std::max(0.25, scale));
            inner = static_cast<int64_t>(static_cast<double>(inner) * scale);
            if (inner < 1024) inner = 1024;
        }
    }

    result.kv("samples", samples);
    result.kv("samples_count", static_cast<int64_t>(samples.size()));
    return result;
}

Json run_sustained_on_big_core(const SustainedConfig& cfg,
                               const std::vector<CpuCluster>& clusters) {
    if (clusters.empty()) {
        Json err;
        err.kv("error", "no_clusters_detected");
        return err;
    }
    const auto& big = clusters.back();  // sorted ascending by freq
    bool pinned = bench::pin_to_cpu(big.max_id);

    Json r = run_sustained(cfg);
    Json wrap;
    wrap.kv("cpu_pinned", big.max_id)
        .kv("max_freq_khz", big.max_freq_khz)
        .kv("pinned", pinned)
        .kv("sustained", r);
    return wrap;
}

} // namespace bench::cpu
