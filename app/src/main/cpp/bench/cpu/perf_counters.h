// PMU counters via perf_event_open(2). Reads hardware performance monitoring
// unit registers directly from userspace — the kernel maintains the per-task
// counter context across context switches and we read it via a file descriptor.
//
// Access on Android (NO ROOT required, but...):
//   - perf_event_paranoid >= 3 (Android 9+ default) restricts perf_event_open
//     to processes with CAP_PERFMON.
//   - The 'shell' UID (adb shell ... cppbench) has CAP_PERFMON on Android 11+
//     in AOSP defaults, so this works on most user-build phones from that line
//     forward. Samsung One UI generally preserves this capability.
//   - From within an APK (regular app UID), perf_event_open fails with EPERM
//     unless the app declares <uses-permission android:name="android.permission.READ_LOGS"/>
//     AND the user's device kernel is permissive. The library degrades
//     gracefully: PerfGroup::available() returns false and the bench reports
//     'pmu_available=false' in the JSON rather than crashing.
//
// What we measure: every hardware counter on the user's CPU. The standard
// PERF_TYPE_HARDWARE set (cycles/instructions/cache-references/cache-misses/
// branch-instructions/branch-misses) is portable across ARM/Intel; the
// ARMv8 PMUv3 raw events (L1D_CACHE, L2D_CACHE_REFILL, etc.) require ARM and
// the right kernel mapping.
#pragma once

#include "../affinity.h"
#include "../json.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bench::cpu {

enum class PerfEvent : int {
    CpuCycles,            // PERF_COUNT_HW_CPU_CYCLES
    Instructions,         // PERF_COUNT_HW_INSTRUCTIONS — IPC = inst/cyc
    CacheReferences,      // PERF_COUNT_HW_CACHE_REFERENCES (typically last-level)
    CacheMisses,          // PERF_COUNT_HW_CACHE_MISSES
    BranchInstructions,   // PERF_COUNT_HW_BRANCH_INSTRUCTIONS
    BranchMisses,         // PERF_COUNT_HW_BRANCH_MISSES — branch mispredicts
    BusCycles,            // PERF_COUNT_HW_BUS_CYCLES
    StalledCyclesFront,   // PERF_COUNT_HW_STALLED_CYCLES_FRONTEND (often unavailable on ARM)
    StalledCyclesBack,    // PERF_COUNT_HW_STALLED_CYCLES_BACKEND   (often unavailable on ARM)
    PageFaults,           // PERF_COUNT_SW_PAGE_FAULTS (software counter)
    ContextSwitches,      // PERF_COUNT_SW_CONTEXT_SWITCHES
    CpuMigrations,        // PERF_COUNT_SW_CPU_MIGRATIONS
};

// A grouped set of counters opened with PERF_FORMAT_GROUP — all counters in
// the group are scheduled together onto the PMU, so values are mutually
// consistent (no time skew). On ARMv8 most chips have 6 programmable + 1
// fixed (cycle) counter, so we can fit ~7 events per group; opening more
// degrades to time-multiplexed scheduling and the kernel scales values via
// the time_enabled/time_running fields.
class PerfGroup {
public:
    PerfGroup();
    ~PerfGroup();
    PerfGroup(const PerfGroup&) = delete;
    PerfGroup& operator=(const PerfGroup&) = delete;

    // Adds a counter to the group. Must be called before start(). Returns
    // false if the event isn't supported by the running kernel or
    // perf_event_open returned EPERM. Other counters in the group continue
    // to work; the failed event will read as 0.
    bool add(PerfEvent ev);

    // True iff at least one counter is open (i.e. we have a leader fd).
    bool available() const;

    void reset();
    void start();
    void stop();

    // Snapshot every counter as a (PerfEvent, value) pair in insertion order.
    struct Sample {
        PerfEvent ev;
        uint64_t value;       // raw counter value (may be time-scaled)
        uint64_t time_enabled_ns;
        uint64_t time_running_ns;
        bool time_scaled;     // true if time_enabled != time_running (multiplexed)
    };
    std::vector<Sample> read() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience: run `body()` wrapped in a PerfGroup, return one JSON object
// per counter with its raw and (if time-scaled) extrapolated values. `body`
// is anything callable with `()`.
template <typename Fn>
Json measure_with_counters(Fn&& body);

// Per-cluster benchmark: pin to each cluster's top CPU, run a small NEON FMA
// workload, report counter snapshot. Reveals microarchitectural differences
// (IPC, branch predictor accuracy, cache hierarchy efficiency) between the
// little/mid/big cores of the SoC.
Json run_perf_counters_per_cluster(const std::vector<CpuCluster>& clusters);

const char* event_name(PerfEvent e);

} // namespace bench::cpu
