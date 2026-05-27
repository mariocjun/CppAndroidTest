#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct NeonFmaConfig {
    // Number of FMA-quad iterations per timed run. Each iteration does
    // 16 independent vfmaq lanes per chain × FMA_CHAINS chains × 2 FMAs
    // (mul+add fused) — see kernel for the count formula.
    int64_t inner_iters = 1ll << 22;   // ~4M outer steps -> ~few seconds on big core
    int iterations = 7;
    int warmup = 2;
};

// Tight-loop NEON FMA throughput. Reports GFLOPS for FP32 and FP16 separately
// (HP arithmetic requires ARMv8.2-A FPHP, present on both Note10+ SoCs but
// not on older A53/A57). Caller pins the thread; this benchmark does NOT
// thread itself — run once per core (or per cluster, pinning to one core) to
// get the peak FLOPS/core figure.
Json run_neon_fma(const NeonFmaConfig& cfg);

// Convenience: pin to the *fastest* CPU in each cluster and measure single-core
// peak. (Multi-core thread scaling is a separate benchmark — runs on top of this.)
Json run_neon_fma_per_cluster(const NeonFmaConfig& cfg,
                              const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
