#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct StreamConfig {
    // Per-array element count (each array is sizeof(float) * elems bytes).
    // 4M floats = 16 MB per array; with copy/scale needing 2 arrays = 32 MB and
    // triad needing 3 = 48 MB. All well above the 2-4 MB SLC on Note10+ SoCs.
    size_t elems = 4 * 1024 * 1024;
    int iterations = 12;     // we report best of N (per STREAM convention)
    int warmup = 3;          // discarded
};

// Run STREAM (copy/scale/add/triad) on the current thread. Caller is responsible
// for pinning (see bench::pin_to_cluster). Returns a Json object with:
//   { copy_gbps, scale_gbps, add_gbps, triad_gbps, elems, iterations }
Json run_stream(const StreamConfig& cfg);

// Convenience: run STREAM once per detected cluster, pinning to it. Returns
// an array of {cluster_idx, min_id, max_id, max_freq_khz, ...stream results...}.
Json run_stream_per_cluster(const StreamConfig& cfg,
                            const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
