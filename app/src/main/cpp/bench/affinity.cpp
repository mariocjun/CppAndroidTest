#include "affinity.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sched.h>
#include <unistd.h>

namespace bench {

int num_cpus() {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? static_cast<int>(n) : 0;
}

static uint64_t read_max_freq(int cpu) {
    char path[160];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    std::ifstream f(path);
    if (!f) return 0;
    uint64_t khz = 0;
    f >> khz;
    return khz;
}

std::vector<CpuCluster> detect_clusters() {
    int n = num_cpus();
    std::map<uint64_t, std::vector<int>> by_freq;
    for (int i = 0; i < n; ++i) {
        uint64_t f = read_max_freq(i);
        // CPUs may be hotplugged off; treat freq=0 as "unknown" and bucket separately.
        by_freq[f].push_back(i);
    }
    std::vector<CpuCluster> out;
    out.reserve(by_freq.size());
    for (auto& kv : by_freq) {
        auto& cpus = kv.second;
        std::sort(cpus.begin(), cpus.end());
        // If contiguous, one cluster; otherwise emit per-island clusters.
        int start = cpus.front();
        int prev = start;
        for (size_t i = 1; i < cpus.size(); ++i) {
            if (cpus[i] != prev + 1) {
                out.push_back({start, prev, kv.first});
                start = cpus[i];
            }
            prev = cpus[i];
        }
        out.push_back({start, prev, kv.first});
    }
    // Sort ascending by max_freq so caller can index little→big naturally.
    std::sort(out.begin(), out.end(),
              [](const CpuCluster& a, const CpuCluster& b) {
                  return a.max_freq_khz < b.max_freq_khz;
              });
    return out;
}

bool pin_to_cluster(const CpuCluster& c) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = c.min_id; i <= c.max_id; ++i) CPU_SET(i, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

bool pin_to_cpu(int cpu_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

} // namespace bench
