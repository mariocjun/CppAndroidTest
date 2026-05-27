#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct SustainedConfig {
    int duration_sec = 30;        // total wall-clock of the workload
    int sample_interval_ms = 250; // how often to snapshot temps/freqs/GFLOPS
    // FMA work chunk size — set so each chunk takes ~sample_interval_ms.
    // Tuned for ~10 GFLOPS big core; auto-scales if measured GFLOPS differs.
    int64_t inner_iters_per_chunk = 1ll << 20;
};

// Run a continuous NEON FMA workload pinned to the FASTEST cpu, sampling
// temperatures and per-cpu frequencies every sample_interval_ms. Reveals
// thermal throttling curves: GFLOPS-vs-time, freq-vs-time, temps-vs-time.
//
// Result schema:
//   {
//     duration_sec, sample_interval_ms,
//     thermal_zones: [{idx, type}, ...],
//     samples: [
//       {t_ms, gflops, temps_mc: [...], freqs_khz: [...]},
//       ...
//     ]
//   }
//
// On non-arm64 builds the NEON workload is replaced by a cheap busy-loop so
// the code path still compiles and runs end-to-end (gflops will be 0).
Json run_sustained(const SustainedConfig& cfg);

// Run on the highest-freq cluster's top CPU.
Json run_sustained_on_big_core(const SustainedConfig& cfg,
                               const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
