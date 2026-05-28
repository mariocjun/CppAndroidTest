#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct Sve2Config {
    int64_t inner_iters = 1ll << 22;
    int iterations = 7;
    int warmup = 2;
};

// SVE2 (Scalable Vector Extension 2) — ARMv9-A baseline. Unlike NEON which
// has fixed 128-bit vectors, SVE2 has runtime-determined vector length
// (between 128 and 2048 bits in increments of 128). On Cortex-X4 / A720 /
// A520 (S24 Ultra's 8 Gen 3 cores) the implementation is 128-bit — same
// width as NEON — but the programming model is fundamentally different:
//
//   - Predicate registers (P0..P15) for masked operations
//   - svfloat32_t / svint32_t etc. — "sizeless" vector types
//   - svptrue_b32() — get an all-true predicate for 32-bit lanes
//   - svcntw() — query lane count at runtime (4 for 128-bit, 8 for 256-bit, ...)
//   - Gather/scatter loads, vector-of-pointers, bit permutes (all new)
//
// What this benchmark measures: peak FMA throughput using svmla_f32_x —
// the SVE2 equivalent of vfmaq_f32. Comparing the FP32 GFLOPS number here
// against the NEON FMA bench on the same core tells you whether your
// vendor's SVE implementation is at parity with NEON (it should be).
//
// Note10+ (Snapdragon 855, Exynos 9825) does NOT support SVE/SVE2 — that
// arrived in ARMv8.2-A SVE / ARMv9-A SVE2 era, after these chips. Gated
// at compile time by __ARM_FEATURE_SVE2 and at runtime by 'sve2' (or 'sve')
// appearing in /proc/cpuinfo's Features.
Json run_sve2(const Sve2Config& cfg);

Json run_sve2_per_cluster(const Sve2Config& cfg,
                          const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
