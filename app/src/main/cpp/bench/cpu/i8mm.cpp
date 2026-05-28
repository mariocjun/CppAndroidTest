// i8mm — ARMv8.6-A / ARMv9 Int8 Matrix Multiply Accumulate.
//
// Compile-time gate: __ARM_FEATURE_MATMUL_INT8 (defined when -march
// contains +i8mm or -march=armv8.6-a+ / armv9-a). This TU is built with
// per-file -march=armv8.2-a+dotprod+i8mm via set_source_files_properties in
// CMakeLists.txt, identical strategy to dot_int8.cpp — limits the i8mm
// emission to this single TU so we don't SIGILL on older devices.
//
// Runtime gate: 'i8mm' must appear in /proc/cpuinfo's "Features" line.
// We check via SocInfo on every call; if absent (e.g. Note10+), the
// benchmark returns -1 GOps and a flag.
#include "i8mm.h"

#include "../soc_info.h"
#include "../timer.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_MATMUL_INT8)
#  include <arm_neon.h>
#  define I8MM_HAS 1
#else
#  define I8MM_HAS 0
#endif

#include <cstdint>

namespace bench::cpu {

namespace {

#if I8MM_HAS

constexpr int CHAINS = 8;

volatile int32_t g_sink_i8mm = 0;

double measure_smmla(int64_t outer_iters) {
    // Each int32x4_t holds the four int32 outputs of one 2x2 result block.
    int32x4_t a0 = vdupq_n_s32(0), a1 = vdupq_n_s32(0);
    int32x4_t a2 = vdupq_n_s32(0), a3 = vdupq_n_s32(0);
    int32x4_t a4 = vdupq_n_s32(0), a5 = vdupq_n_s32(0);
    int32x4_t a6 = vdupq_n_s32(0), a7 = vdupq_n_s32(0);
    // int8x16_t holds 2 rows of 8 int8 (2x8) for one operand.
    const int8x16_t v = vdupq_n_s8(1);
    const int8x16_t w = vdupq_n_s8(2);

    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) {
        a0 = vmmlaq_s32(a0, v, w);
        a1 = vmmlaq_s32(a1, v, w);
        a2 = vmmlaq_s32(a2, v, w);
        a3 = vmmlaq_s32(a3, v, w);
        a4 = vmmlaq_s32(a4, v, w);
        a5 = vmmlaq_s32(a5, v, w);
        a6 = vmmlaq_s32(a6, v, w);
        a7 = vmmlaq_s32(a7, v, w);
    }
    uint64_t t1 = now_ns();

    // 8 chains * 16 mults * 2 (mul+add) = 256 ops per inner iter
    double ops = static_cast<double>(outer_iters) * CHAINS * 16 * 2;
    double secs = static_cast<double>(t1 - t0) * 1e-9;

    int32x4_t acc = vaddq_s32(vaddq_s32(vaddq_s32(a0, a1), vaddq_s32(a2, a3)),
                              vaddq_s32(vaddq_s32(a4, a5), vaddq_s32(a6, a7)));
    int32_t lanes[4];
    vst1q_s32(lanes, acc);
    g_sink_i8mm = lanes[0] + lanes[1] + lanes[2] + lanes[3];

    return ops / secs / 1e9;
}

#else
double measure_smmla(int64_t /*outer_iters*/) { return -1.0; }
#endif

bool i8mm_in_features() {
    auto s = bench::collect_soc_info();
    for (const auto& f : s.features) {
        if (f == "i8mm") return true;
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

Json run_i8mm(const I8mmConfig& cfg) {
    const bool supported = i8mm_in_features();
    double smmla_gops = -1.0;
#if I8MM_HAS
    if (supported) {
        smmla_gops = best_of(cfg.iterations, cfg.warmup, measure_smmla, cfg.inner_iters);
    }
#endif

    Json out;
    out.kv("inner_iters", static_cast<int64_t>(cfg.inner_iters))
       .kv("iterations", cfg.iterations)
       .kv("smmla_gops_per_core", smmla_gops)
       .kv("i8mm_supported", supported)
       .kv("compiled_with_i8mm", I8MM_HAS ? true : false);
    return out;
}

Json run_i8mm_per_cluster(const I8mmConfig& cfg,
                          const std::vector<CpuCluster>& clusters) {
    std::vector<Json> rows;
    rows.reserve(clusters.size());
    for (std::size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cpu(c.max_id);
        Json r = run_i8mm(cfg);
        Json wrap;
        wrap.kv("cluster_idx", static_cast<int64_t>(i))
            .kv("cpu_pinned", c.max_id)
            .kv("max_freq_khz", c.max_freq_khz)
            .kv("pinned", pinned)
            .kv("i8mm", r);
        rows.push_back(std::move(wrap));
    }
    Json out;
    out.kv("per_cluster", rows);
    return out;
}

} // namespace bench::cpu
