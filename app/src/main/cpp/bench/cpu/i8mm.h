#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct I8mmConfig {
    int64_t inner_iters = 1ll << 22;
    int iterations = 7;
    int warmup = 2;
};

// ARMv8.6-A / ARMv9-A "i8mm" extension — the SMMLA / UMMLA / USMMLA instructions.
// Each smmla executes a 2x2 = 2x8 * 8x2 int8 matrix multiply with int32
// accumulator in ONE instruction:
//
//   For input A[2x8] (signed int8), B[8x2] (signed int8), output C[2x2] (int32):
//   C[i,j] += sum_{k=0..7} A[i,k] * B[k,j]    ; per output lane
//
// That's 16 multiplies + 16 adds = 32 ops per instruction. With 8 independent
// chains we feed enough work to saturate the Cortex-X4 / A720 / A520 issue width.
//
// Why this matters specifically for S24 Ultra: 8 Gen 3 (Cortex-X4 / A720 / A520)
// all support i8mm. Compared to `vdotq_s32` (16 mults/instr), `smmla` does 16
// mults too BUT outputs a 2x2 matrix block so dense matmul kernels (the inner
// loops of every quantised ML model) can use it directly without manual
// transposition. Expected single-core throughput on X4 @ 3.39 GHz:
//   ~350-450 GOps/s int8 — roughly 2x SDOT in the same workload.
//
// Note10+ Snapdragon 855 (Cortex-A76 / A55) does NOT support i8mm — feature
// added in ARMv8.6. Note10+ Exynos 9825 also doesn't have it. The runtime
// gate (`i8mm` token in /proc/cpuinfo) keeps the binary portable.
Json run_i8mm(const I8mmConfig& cfg);

Json run_i8mm_per_cluster(const I8mmConfig& cfg,
                          const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
