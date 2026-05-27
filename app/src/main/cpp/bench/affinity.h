#pragma once

#include <cstdint>
#include <vector>

namespace bench {

struct CpuCluster {
    int min_id;             // first cpu id in this cluster
    int max_id;             // last cpu id (inclusive)
    uint64_t max_freq_khz;  // peak frequency reported by cpufreq

    int core_count() const { return max_id - min_id + 1; }
};

// Detect clusters by reading /sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_max_freq.
// CPUs with identical max_freq are coalesced into one cluster. Returned vector is
// sorted ascending by max_freq, so [0] is the LITTLE cluster and back() is the
// fastest (Prime / big) cluster.
//
// On Note10+ this yields 3 entries on Snapdragon (Silver A55 / Gold A76 / Prime A76)
// and 3 entries on Exynos (A55 / A75 / Mongoose M4). Layout assumed contiguous —
// true for all Note10+ firmwares observed.
std::vector<CpuCluster> detect_clusters();

// Pin the calling thread to all CPUs in the cluster.
bool pin_to_cluster(const CpuCluster& c);

// Pin the calling thread to one specific CPU. Useful for single-core benchmarks
// where you want the scheduler out of the loop entirely.
bool pin_to_cpu(int cpu_id);

// Number of online logical CPUs.
int num_cpus();

} // namespace bench
