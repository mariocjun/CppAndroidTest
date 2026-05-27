// ARMv8.2-A DotProd: SDOT / UDOT int8 throughput.
//
// vdotq_s32(int32x4_t acc, int8x16_t a, int8x16_t b):
//   acc.lane[i] += a.byte[4i+0]*b.byte[4i+0] + a.byte[4i+1]*b.byte[4i+1]
//               + a.byte[4i+2]*b.byte[4i+2] + a.byte[4i+3]*b.byte[4i+3]
//
// One vdotq = 16 int8 multiplies + 16 int32 adds = 32 ops by our convention.
// 8 independent chains × 32 ops = 256 ops per inner iteration.
//
// Compile-time gate: __ARM_FEATURE_DOTPROD (defined when -mcpu=cortex-a76 or
// armv8.2-a+dotprod, which is the NDK arm64-v8a default for API 24+).
// Runtime gate: /proc/cpuinfo Features must include "asimddp".
//
// On x86_64 / armv7 builds the function returns -1 and the wrapper marks
// dotprod as unsupported.
#include "dot_int8.h"

#include "../timer.h"
#include "../soc_info.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_DOTPROD)
#  include <arm_neon.h>
#  define DOT_HAS_DOTPROD 1
#else
#  define DOT_HAS_DOTPROD 0
#endif

#include <algorithm>
#include <cstdint>

namespace bench::cpu {

namespace {

constexpr int DOT_CHAINS = 8;

#if DOT_HAS_DOTPROD
volatile int32_t g_sink_dot_s = 0;
volatile uint32_t g_sink_dot_u = 0;

double measure_sdot(int64_t outer_iters) {
    int32x4_t a0 = vdupq_n_s32(0); int32x4_t a1 = vdupq_n_s32(0);
    int32x4_t a2 = vdupq_n_s32(0); int32x4_t a3 = vdupq_n_s32(0);
    int32x4_t a4 = vdupq_n_s32(0); int32x4_t a5 = vdupq_n_s32(0);
    int32x4_t a6 = vdupq_n_s32(0); int32x4_t a7 = vdupq_n_s32(0);
    const int8x16_t v = vdupq_n_s8(1);
    const int8x16_t w = vdupq_n_s8(2);

    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) {
        a0 = vdotq_s32(a0, v, w);
        a1 = vdotq_s32(a1, v, w);
        a2 = vdotq_s32(a2, v, w);
        a3 = vdotq_s32(a3, v, w);
        a4 = vdotq_s32(a4, v, w);
        a5 = vdotq_s32(a5, v, w);
        a6 = vdotq_s32(a6, v, w);
        a7 = vdotq_s32(a7, v, w);
    }
    uint64_t t1 = now_ns();

    // 8 chains * 16 mults * 2 (mul+add) = 256 ops per inner iter
    double ops = static_cast<double>(outer_iters) * DOT_CHAINS * 16 * 2;
    double secs = static_cast<double>(t1 - t0) * 1e-9;

    int32x4_t acc = vaddq_s32(vaddq_s32(vaddq_s32(a0, a1), vaddq_s32(a2, a3)),
                              vaddq_s32(vaddq_s32(a4, a5), vaddq_s32(a6, a7)));
    int32_t lanes[4];
    vst1q_s32(lanes, acc);
    g_sink_dot_s = lanes[0] + lanes[1] + lanes[2] + lanes[3];

    return ops / secs / 1e9;
}

double measure_udot(int64_t outer_iters) {
    uint32x4_t a0 = vdupq_n_u32(0); uint32x4_t a1 = vdupq_n_u32(0);
    uint32x4_t a2 = vdupq_n_u32(0); uint32x4_t a3 = vdupq_n_u32(0);
    uint32x4_t a4 = vdupq_n_u32(0); uint32x4_t a5 = vdupq_n_u32(0);
    uint32x4_t a6 = vdupq_n_u32(0); uint32x4_t a7 = vdupq_n_u32(0);
    const uint8x16_t v = vdupq_n_u8(1);
    const uint8x16_t w = vdupq_n_u8(2);

    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) {
        a0 = vdotq_u32(a0, v, w);
        a1 = vdotq_u32(a1, v, w);
        a2 = vdotq_u32(a2, v, w);
        a3 = vdotq_u32(a3, v, w);
        a4 = vdotq_u32(a4, v, w);
        a5 = vdotq_u32(a5, v, w);
        a6 = vdotq_u32(a6, v, w);
        a7 = vdotq_u32(a7, v, w);
    }
    uint64_t t1 = now_ns();

    double ops = static_cast<double>(outer_iters) * DOT_CHAINS * 16 * 2;
    double secs = static_cast<double>(t1 - t0) * 1e-9;

    uint32x4_t acc = vaddq_u32(vaddq_u32(vaddq_u32(a0, a1), vaddq_u32(a2, a3)),
                               vaddq_u32(vaddq_u32(a4, a5), vaddq_u32(a6, a7)));
    uint32_t lanes[4];
    vst1q_u32(lanes, acc);
    g_sink_dot_u = lanes[0] + lanes[1] + lanes[2] + lanes[3];

    return ops / secs / 1e9;
}
#else
double measure_sdot(int64_t) { return -1.0; }
double measure_udot(int64_t) { return -1.0; }
#endif

bool dotprod_in_features() {
    auto s = bench::collect_soc_info();
    for (const auto& f : s.features) {
        if (f == "asimddp") return true;
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

Json run_dot_int8(const DotInt8Config& cfg) {
    bool supported = dotprod_in_features();

    double sdot = -1.0;
    double udot = -1.0;
#if DOT_HAS_DOTPROD
    if (supported) {
        sdot = best_of(cfg.iterations, cfg.warmup, measure_sdot, cfg.inner_iters);
        udot = best_of(cfg.iterations, cfg.warmup, measure_udot, cfg.inner_iters);
    }
#endif

    Json out;
    out.kv("inner_iters", static_cast<int64_t>(cfg.inner_iters))
       .kv("iterations", cfg.iterations)
       .kv("sdot_gops_per_core", sdot)
       .kv("udot_gops_per_core", udot)
       .kv("dotprod_supported", supported)
       .kv("compiled_with_dotprod", DOT_HAS_DOTPROD ? true : false);
    return out;
}

Json run_dot_int8_per_cluster(const DotInt8Config& cfg,
                              const std::vector<CpuCluster>& clusters) {
    std::vector<Json> rows;
    rows.reserve(clusters.size());
    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cpu(c.max_id);
        Json r = run_dot_int8(cfg);
        Json wrap;
        wrap.kv("cluster_idx", static_cast<int64_t>(i))
            .kv("cpu_pinned", c.max_id)
            .kv("max_freq_khz", c.max_freq_khz)
            .kv("pinned", pinned)
            .kv("dot_int8", r);
        rows.push_back(std::move(wrap));
    }
    Json out;
    out.kv("per_cluster", rows);
    return out;
}

} // namespace bench::cpu
