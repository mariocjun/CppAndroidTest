// SVE2 FP32 FMA throughput (S24 Ultra and any other ARMv9-A device).
//
// Compile gate: __ARM_FEATURE_SVE2 (defined when -march has +sve2 or
// -march=armv9-a). This TU built with per-file -march=armv9-a in CMake.
// Runtime gate: 'sve2' (or 'sve') in /proc/cpuinfo features.
//
// Why we count lanes via svcntw() rather than hard-coding 4: SVE is
// "sizeless" by design — the same source compiles to a binary that runs
// on any SVE implementation (128 / 256 / 512 / ... bit). On Cortex-X4
// / A720 / A520 the implementation is 128-bit so svcntw() returns 4.
// On future ARM cores it may return 8 or 16. The benchmark accounts for
// that when computing FLOPS.
#include "sve2.h"

#include "../soc_info.h"
#include "../timer.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_SVE2)
#  include <arm_sve.h>
#  define SVE2_HAS 1
#else
#  define SVE2_HAS 0
#endif

#include <cstdint>

namespace bench::cpu {

namespace {

#if SVE2_HAS

constexpr int CHAINS = 8;

volatile float g_sink_sve2 = 0.0f;

// SVE intrinsics are not constexpr-constructible, so we build the constants
// inside the function via svdup_n.
double measure_svmla_f32(int64_t outer_iters) {
    const svbool_t pg = svptrue_b32();              // all-true predicate
    svfloat32_t a0 = svdup_n_f32(0.1f), a1 = svdup_n_f32(0.2f);
    svfloat32_t a2 = svdup_n_f32(0.3f), a3 = svdup_n_f32(0.4f);
    svfloat32_t a4 = svdup_n_f32(0.5f), a5 = svdup_n_f32(0.6f);
    svfloat32_t a6 = svdup_n_f32(0.7f), a7 = svdup_n_f32(0.8f);
    const svfloat32_t b = svdup_n_f32(1.0000001f);
    const svfloat32_t c = svdup_n_f32(0.9999999f);
    const uint64_t lanes = svcntw();                // runtime vector lane count

    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) {
        a0 = svmla_f32_x(pg, a0, b, c);
        a1 = svmla_f32_x(pg, a1, b, c);
        a2 = svmla_f32_x(pg, a2, b, c);
        a3 = svmla_f32_x(pg, a3, b, c);
        a4 = svmla_f32_x(pg, a4, b, c);
        a5 = svmla_f32_x(pg, a5, b, c);
        a6 = svmla_f32_x(pg, a6, b, c);
        a7 = svmla_f32_x(pg, a7, b, c);
    }
    uint64_t t1 = now_ns();

    // Each iter: CHAINS * lanes FMAs * 2 FLOPs (mul + add)
    double flops = static_cast<double>(outer_iters) * CHAINS *
                   static_cast<double>(lanes) * 2.0;
    double secs = static_cast<double>(t1 - t0) * 1e-9;

    // Sum lanes of every chain into the volatile sink so the optimiser
    // can't drop the loop.
    svfloat32_t sum = svadd_f32_x(pg, svadd_f32_x(pg, svadd_f32_x(pg, a0, a1),
                                                       svadd_f32_x(pg, a2, a3)),
                                       svadd_f32_x(pg, svadd_f32_x(pg, a4, a5),
                                                       svadd_f32_x(pg, a6, a7)));
    g_sink_sve2 = svaddv_f32(pg, sum);

    return flops / secs / 1e9;
}

uint64_t vector_lanes_f32() { return svcntw(); }

#else
double measure_svmla_f32(int64_t /*outer_iters*/) { return -1.0; }
uint64_t vector_lanes_f32() { return 0; }
#endif

bool sve2_in_features() {
    // We require sve2 specifically — sve1 alone wouldn't expose svmla_f32_x
    // semantics we depend on. Devices that lack sve2 get -1 GOps.
    auto s = bench::collect_soc_info();
    for (const auto& f : s.features) {
        if (f == "sve2") return true;
    }
    return false;
}

double best_of(int iters, int warmup, double (*fn)(int64_t), int64_t arg) {
    for (int i = 0; i < warmup; ++i) fn(arg);
    double best = -1.0;
    for (int i = 0; i < iters; ++i) {
        double v = fn(arg);
        if (v > best) best = v;
    }
    return best;
}

} // namespace

Json run_sve2(const Sve2Config& cfg) {
    const bool supported = sve2_in_features();
    double gflops = -1.0;
    uint64_t lanes = 0;
#if SVE2_HAS
    if (supported) {
        gflops = best_of(cfg.iterations, cfg.warmup, measure_svmla_f32, cfg.inner_iters);
        lanes = vector_lanes_f32();
    }
#endif

    Json out;
    out.kv("inner_iters", static_cast<int64_t>(cfg.inner_iters))
       .kv("iterations", cfg.iterations)
       .kv("fp32_gflops_per_core", gflops)
       .kv("vector_lanes_fp32", static_cast<int64_t>(lanes))
       .kv("vector_bits", static_cast<int64_t>(lanes * 32))
       .kv("sve2_supported", supported)
       .kv("compiled_with_sve2", SVE2_HAS ? true : false);
    return out;
}

Json run_sve2_per_cluster(const Sve2Config& cfg,
                          const std::vector<CpuCluster>& clusters) {
    std::vector<Json> rows;
    rows.reserve(clusters.size());
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cpu(c.max_id);
        Json r = run_sve2(cfg);
        Json wrap;
        wrap.kv("cluster_idx", static_cast<int64_t>(i))
            .kv("cpu_pinned", c.max_id)
            .kv("max_freq_khz", c.max_freq_khz)
            .kv("pinned", pinned)
            .kv("sve2", r);
        rows.push_back(std::move(wrap));
    }
    Json out;
    out.kv("per_cluster", rows);
    return out;
}

} // namespace bench::cpu
