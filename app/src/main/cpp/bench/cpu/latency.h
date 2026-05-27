#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct LatencyConfig {
    // Buffer sizes to probe, in bytes. Defaults cover L1d (32-64K) through
    // DRAM (>>SLC, 64 MB) for the Note10+ SoCs. Each size produces one row.
    std::vector<size_t> sizes_bytes = {
        4 * 1024,           //  4 KB  — well inside L1d
        32 * 1024,          // 32 KB  — A55/A76 L1d boundary
        128 * 1024,         // 128 KB — inside L2 on M4/A76, outside L2 on A55
        256 * 1024,         // 256 KB
        512 * 1024,         // 512 KB
        1 * 1024 * 1024,    // 1 MB   — inside cluster L2 on big cores
        2 * 1024 * 1024,    // 2 MB   — SD855 SLC boundary
        4 * 1024 * 1024,    // 4 MB   — Exynos 9825 SLC boundary
        16 * 1024 * 1024,   // 16 MB  — solidly DRAM territory
        64 * 1024 * 1024    // 64 MB  — sanity DRAM
    };
    // Number of dependent loads per timed run. Higher => less variance,
    // longer wall-clock. 256K is ~2-200ms depending on cache level.
    int chase_steps = 256 * 1024;
    int iterations = 5;
    int warmup = 2;
};

// Runs pointer-chase at each configured size. Caller should have pinned the
// thread before calling. Result JSON:
//   { sizes: [...], ns_per_load: [...], total_chase_steps, iterations }
Json run_latency(const LatencyConfig& cfg);

// Per-cluster wrapper, like run_stream_per_cluster.
Json run_latency_per_cluster(const LatencyConfig& cfg,
                             const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
