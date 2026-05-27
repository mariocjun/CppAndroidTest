// NEON peak FMA throughput on a single core.
//
// This file is compiled for all ABIs the .so is built for (arm64-v8a is the
// one that matters; armeabi-v7a and x86_64 builds get a stub that reports -1
// for both FP32 and FP16). The standalone cppbench ELF is only ever built
// arm64-v8a, so the real path always runs on-device.
//
// On arm64-v8a:
//   FP32 path uses vfmaq_f32 — 4 lanes per instruction, 2 FLOPs per lane
//   (a mul + an add fused), so 8 FLOPs per instruction. We fire 8 independent
//   FMA chains in parallel to saturate the OoO scheduler (Cortex-A76 and
//   Mongoose M4 sustain ~2 FMA instructions per cycle with ~4-cycle latency;
//   8 chains gives comfortable headroom).
//
//   FP16 path uses vfmaq_f16 — 8 lanes per instruction, 2 FLOPs per lane =
//   16 FLOPs per instruction. Requires ARMv8.2-A FPHP (Cortex-A76 has it;
//   Mongoose M4 has it; Cortex-A55 has it; older A53 doesn't). We additionally
//   gate on /proc/cpuinfo "Features" containing "fphp" or "asimdhp" before
//   running the FP16 path, so the binary stays portable across older devices.
//
// Anti-cheat:
//   - Volatile sink consumes the final accumulators so DCE can't drop them.
//   - Initial values are read at runtime so the compiler can't constant-fold.
//
// What you should see on Note10+:
//   M4 / A76 Prime @ ~2.8 GHz:   ~10-12 GFLOPS FP32, ~20-24 GFLOPS FP16
//   A75 / A76 Gold @ ~2.4 GHz:   ~8-10 GFLOPS FP32, ~17-20 GFLOPS FP16
//   A55 @ ~1.8 GHz:              ~3-4  GFLOPS FP32, ~6-8 GFLOPS FP16
//
// If you see >2x what you expect, the loop got optimized away — add another
// dependent op or check g_sink_fma_* is actually wired up.
#include "neon_fma.h"

#include "../timer.h"
#include "../soc_info.h"

#if defined(__aarch64__)
#  include <arm_neon.h>
#  define BENCH_HAS_NEON 1
#else
#  define BENCH_HAS_NEON 0
#endif

#include <algorithm>
#include <cstdint>

namespace bench::cpu {

namespace {

#if BENCH_HAS_NEON

constexpr int FMA_CHAINS = 8;

volatile float g_sink_fma_f32 = 0.0f;
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
volatile float g_sink_fma_f16 = 0.0f;
#endif

double measure_fp32(int64_t outer_iters) {
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

    // 8 chains * 4 lanes * 2 FLOPs = 64 FLOPs per inner iter
    double flops = static_cast<double>(outer_iters) * FMA_CHAINS * 4 * 2;
    double secs = static_cast<double>(t1 - t0) * 1e-9;

    float32x4_t acc = vaddq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)),
                                vaddq_f32(vaddq_f32(a4, a5), vaddq_f32(a6, a7)));
    float lanes[4];
    vst1q_f32(lanes, acc);
    g_sink_fma_f32 = lanes[0] + lanes[1] + lanes[2] + lanes[3];

    return flops / secs / 1e9;
}

#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
// Only defined when the compiler has FP16 vector arith enabled. The call site
// in run_neon_fma() is gated by the same macro, so omitting the stub on the
// `#else` branch leaves the function genuinely undefined-and-unreferenced
// rather than declared-and-unused (which clang warns about under -Wall).
double measure_fp16(int64_t outer_iters) {
    float16x8_t a0 = vdupq_n_f16(static_cast<float16_t>(0.1f));
    float16x8_t a1 = vdupq_n_f16(static_cast<float16_t>(0.2f));
    float16x8_t a2 = vdupq_n_f16(static_cast<float16_t>(0.3f));
    float16x8_t a3 = vdupq_n_f16(static_cast<float16_t>(0.4f));
    float16x8_t a4 = vdupq_n_f16(static_cast<float16_t>(0.5f));
    float16x8_t a5 = vdupq_n_f16(static_cast<float16_t>(0.6f));
    float16x8_t a6 = vdupq_n_f16(static_cast<float16_t>(0.7f));
    float16x8_t a7 = vdupq_n_f16(static_cast<float16_t>(0.8f));
    const float16x8_t b = vdupq_n_f16(static_cast<float16_t>(1.0001f));
    const float16x8_t c = vdupq_n_f16(static_cast<float16_t>(0.9999f));

    uint64_t t0 = now_ns();
    for (int64_t i = 0; i < outer_iters; ++i) {
        a0 = vfmaq_f16(a0, b, c);
        a1 = vfmaq_f16(a1, b, c);
        a2 = vfmaq_f16(a2, b, c);
        a3 = vfmaq_f16(a3, b, c);
        a4 = vfmaq_f16(a4, b, c);
        a5 = vfmaq_f16(a5, b, c);
        a6 = vfmaq_f16(a6, b, c);
        a7 = vfmaq_f16(a7, b, c);
    }
    uint64_t t1 = now_ns();

    // 8 chains * 8 lanes * 2 FLOPs = 128 FLOPs per inner iter
    double flops = static_cast<double>(outer_iters) * FMA_CHAINS * 8 * 2;
    double secs = static_cast<double>(t1 - t0) * 1e-9;

    float16x8_t acc = vaddq_f16(vaddq_f16(vaddq_f16(a0, a1), vaddq_f16(a2, a3)),
                                vaddq_f16(vaddq_f16(a4, a5), vaddq_f16(a6, a7)));
    float16_t lanes[8];
    vst1q_f16(lanes, acc);
    float s = 0;
    for (int i = 0; i < 8; ++i) s += static_cast<float>(lanes[i]);
    g_sink_fma_f16 = s;

    return flops / secs / 1e9;
}
#endif  // FPHP

#else  // !BENCH_HAS_NEON: non-arm64 build (only happens for the x86_64 .so used
       // by the smoke emulator). Report sentinel so JSON consumers see "N/A".
double measure_fp32(int64_t) { return -1.0; }
#endif

bool fphp_in_features() {
    auto s = bench::collect_soc_info();
    for (const auto& f : s.features) {
        if (f == "fphp" || f == "asimdhp") return true;
    }
    return false;
}

double best_of(int iters, int warmup, double (*fn)(int64_t), int64_t arg) {
    for (int i = 0; i < warmup; ++i) fn(arg);
    double best = -1.0;
    for (int i = 0; i < iters; ++i) {
        double v = fn(arg);
        if (v > best) best = v;  // GFLOPS — bigger is better
    }
    return best;
}

} // namespace

Json run_neon_fma(const NeonFmaConfig& cfg) {
    double fp32 = best_of(cfg.iterations, cfg.warmup, measure_fp32, cfg.inner_iters);

    double fp16 = -1.0;
    bool fphp_supported = false;
#if BENCH_HAS_NEON
    fphp_supported = fphp_in_features();
#  if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
    if (fphp_supported) {
        fp16 = best_of(cfg.iterations, cfg.warmup, measure_fp16, cfg.inner_iters);
    }
#  endif
#endif

    Json out;
    out.kv("inner_iters", static_cast<int64_t>(cfg.inner_iters))
       .kv("iterations", cfg.iterations)
       .kv("fp32_gflops_per_core", fp32)
       .kv("fp16_gflops_per_core", fp16)
       .kv("fphp_supported", fphp_supported)
       .kv("arch_has_neon", BENCH_HAS_NEON ? true : false);
    return out;
}

Json run_neon_fma_per_cluster(const NeonFmaConfig& cfg,
                              const std::vector<CpuCluster>& clusters) {
    std::vector<Json> rows;
    rows.reserve(clusters.size());
    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cpu(c.max_id);
        Json r = run_neon_fma(cfg);
        Json wrap;
        wrap.kv("cluster_idx", static_cast<int64_t>(i))
            .kv("cpu_pinned", c.max_id)
            .kv("max_freq_khz", c.max_freq_khz)
            .kv("pinned", pinned)
            .kv("neon_fma", r);
        rows.push_back(std::move(wrap));
    }
    Json out;
    out.kv("per_cluster", rows);
    return out;
}

} // namespace bench::cpu
