#include "perf_counters.h"

#include "../timer.h"

#include <cerrno>
#include <cstring>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__aarch64__)
#  include <arm_neon.h>
#  define PERF_HAS_NEON 1
#else
#  define PERF_HAS_NEON 0
#endif

namespace bench::cpu {

namespace {

// glibc/bionic don't ship a perf_event_open wrapper — go direct.
long perf_event_open_syscall(struct perf_event_attr* attr, pid_t pid,
                              int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

struct EventDesc {
    uint32_t type;
    uint64_t config;
    const char* name;
};

EventDesc desc_for(PerfEvent e) {
    switch (e) {
        case PerfEvent::CpuCycles:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "cpu_cycles"};
        case PerfEvent::Instructions:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "instructions"};
        case PerfEvent::CacheReferences:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES, "cache_references"};
        case PerfEvent::CacheMisses:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, "cache_misses"};
        case PerfEvent::BranchInstructions:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branch_instructions"};
        case PerfEvent::BranchMisses:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, "branch_misses"};
        case PerfEvent::BusCycles:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BUS_CYCLES, "bus_cycles"};
        case PerfEvent::StalledCyclesFront:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND, "stalled_cycles_front"};
        case PerfEvent::StalledCyclesBack:
            return {PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_BACKEND, "stalled_cycles_back"};
        case PerfEvent::PageFaults:
            return {PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS, "page_faults"};
        case PerfEvent::ContextSwitches:
            return {PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, "context_switches"};
        case PerfEvent::CpuMigrations:
            return {PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS, "cpu_migrations"};
    }
    return {PERF_TYPE_HARDWARE, 0, "unknown"};
}

} // namespace

const char* event_name(PerfEvent e) {
    return desc_for(e).name;
}

struct PerfGroup::Impl {
    // First fd opened is the group leader; subsequent counters are added with
    // group_fd = leader. read() on the leader returns all counters at once
    // when PERF_FORMAT_GROUP is set.
    int leader_fd = -1;
    std::vector<int> fds;       // parallel to events
    std::vector<PerfEvent> events;
};

PerfGroup::PerfGroup() : impl_(std::make_unique<Impl>()) {}

PerfGroup::~PerfGroup() {
    if (!impl_) return;
    for (int fd : impl_->fds) {
        if (fd >= 0) ::close(fd);
    }
}

bool PerfGroup::available() const {
    return impl_ && impl_->leader_fd >= 0;
}

bool PerfGroup::add(PerfEvent ev) {
    if (!impl_) return false;
    EventDesc d = desc_for(ev);
    struct perf_event_attr attr{};
    attr.type = d.type;
    attr.size = sizeof(attr);
    attr.config = d.config;
    attr.disabled = (impl_->leader_fd < 0) ? 1 : 0;  // leader starts disabled
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
                       PERF_FORMAT_TOTAL_TIME_RUNNING |
                       PERF_FORMAT_GROUP |
                       PERF_FORMAT_ID;

    int group_fd = impl_->leader_fd;  // -1 for the very first counter
    long fd = perf_event_open_syscall(&attr, /*pid=*/0 /*self*/, /*cpu=*/-1,
                                       group_fd, /*flags=*/0);
    if (fd < 0) {
        return false;  // EPERM (paranoid), ENOENT (event unsupported), etc.
    }
    if (impl_->leader_fd < 0) impl_->leader_fd = static_cast<int>(fd);
    impl_->fds.push_back(static_cast<int>(fd));
    impl_->events.push_back(ev);
    return true;
}

void PerfGroup::reset() {
    if (!available()) return;
    ioctl(impl_->leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
}

void PerfGroup::start() {
    if (!available()) return;
    ioctl(impl_->leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

void PerfGroup::stop() {
    if (!available()) return;
    ioctl(impl_->leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

std::vector<PerfGroup::Sample> PerfGroup::read() const {
    std::vector<Sample> out;
    if (!available()) return out;

    // PERF_FORMAT_GROUP layout when reading from the leader fd:
    //   u64 nr;                          // number of events in group
    //   u64 time_enabled;
    //   u64 time_running;
    //   { u64 value; u64 id; } values[nr];
    //
    // We sized for our worst case (16 counters * 16 bytes + 24 header bytes).
    uint64_t buf[64];
    ssize_t n = ::read(impl_->leader_fd, buf, sizeof(buf));
    if (n <= 0) return out;

    uint64_t nr = buf[0];
    uint64_t te = buf[1];
    uint64_t tr = buf[2];
    if (nr != impl_->events.size()) return out;  // sanity

    const bool scaled = (te != tr);
    for (uint64_t i = 0; i < nr; ++i) {
        Sample s;
        s.ev = impl_->events[static_cast<size_t>(i)];
        s.value = buf[3 + i * 2];
        // buf[3 + i*2 + 1] is the event ID; we don't need to map it because
        // values are returned in insertion order.
        s.time_enabled_ns = te;
        s.time_running_ns = tr;
        s.time_scaled = scaled;
        out.push_back(s);
    }
    return out;
}

// -- run_perf_counters_per_cluster ------------------------------------------

namespace {

// Small FMA workload — independent of the dedicated neon_fma benchmark so we
// can run it without leaking any unique implementation detail. The point
// here is to GIVE the PMU something deterministic to count, not to maximise
// throughput.
constexpr int64_t WORKLOAD_ITERS = 1 << 20;

#if PERF_HAS_NEON
volatile float g_sink_pmu = 0.0f;

void do_workload() {
    float32x4_t a = vdupq_n_f32(0.1f);
    float32x4_t b = vdupq_n_f32(0.2f);
    float32x4_t c = vdupq_n_f32(0.3f);
    const float32x4_t v = vdupq_n_f32(1.0000001f);
    const float32x4_t w = vdupq_n_f32(0.9999999f);
    for (int64_t i = 0; i < WORKLOAD_ITERS; ++i) {
        a = vfmaq_f32(a, v, w);
        b = vfmaq_f32(b, v, w);
        c = vfmaq_f32(c, v, w);
    }
    float32x4_t s = vaddq_f32(vaddq_f32(a, b), c);
    float lanes[4];
    vst1q_f32(lanes, s);
    g_sink_pmu = lanes[0] + lanes[1] + lanes[2] + lanes[3];
}
#else
void do_workload() {
    volatile double a = 0.1;
    for (int64_t i = 0; i < WORKLOAD_ITERS; ++i) a = a * 0.999 + 0.001;
}
#endif

Json group_to_json(const PerfGroup& g) {
    auto samples = g.read();
    std::vector<Json> rows;
    for (const auto& s : samples) {
        // If time-multiplexed (the kernel couldn't keep all counters live the
        // whole time), provide both raw and scaled-up values. Scaling formula:
        // value_scaled = value * (time_enabled / time_running)
        double scaled = static_cast<double>(s.value);
        if (s.time_scaled && s.time_running_ns > 0) {
            scaled = static_cast<double>(s.value) *
                     (static_cast<double>(s.time_enabled_ns) /
                      static_cast<double>(s.time_running_ns));
        }
        Json r;
        r.kv("event", event_name(s.ev))
         .kv("value", static_cast<int64_t>(s.value))
         .kv("scaled_value", scaled)
         .kv("time_enabled_ns", s.time_enabled_ns)
         .kv("time_running_ns", s.time_running_ns)
         .kv("time_multiplexed", s.time_scaled);
        rows.push_back(std::move(r));
    }
    Json out;
    out.kv("counters", rows);
    return out;
}

} // namespace

Json run_perf_counters_per_cluster(const std::vector<CpuCluster>& clusters) {
    std::vector<Json> per_cluster;
    bool any_available = false;

    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cpu(c.max_id);

        PerfGroup g;
        // Order matters: keep the most universally-supported events first.
        // The leader (first call to add) opens disabled — we enable the whole
        // group with one ioctl after configuration.
        g.add(PerfEvent::CpuCycles);
        g.add(PerfEvent::Instructions);
        g.add(PerfEvent::CacheReferences);
        g.add(PerfEvent::CacheMisses);
        g.add(PerfEvent::BranchInstructions);
        g.add(PerfEvent::BranchMisses);
        g.add(PerfEvent::PageFaults);

        Json row;
        row.kv("cluster_idx", static_cast<int64_t>(i))
           .kv("cpu_pinned", c.max_id)
           .kv("max_freq_khz", c.max_freq_khz)
           .kv("pinned", pinned);

        if (!g.available()) {
            row.kv("pmu_available", false)
               .kv("note", "perf_event_open denied — kernel.perf_event_paranoid"
                            " likely >= 2 and process lacks CAP_PERFMON");
            per_cluster.push_back(std::move(row));
            continue;
        }

        any_available = true;
        g.reset();
        g.start();
        uint64_t t0 = now_ns();
        do_workload();
        uint64_t t1 = now_ns();
        g.stop();

        row.kv("pmu_available", true)
           .kv("workload_ns", static_cast<int64_t>(t1 - t0))
           .kv("workload_iters", static_cast<int64_t>(WORKLOAD_ITERS))
           .kv("workload_kind", PERF_HAS_NEON ? "neon_fma" : "scalar_fma")
           .kv("samples", group_to_json(g));
        per_cluster.push_back(std::move(row));
    }

    Json out;
    out.kv("any_pmu_available", any_available)
       .kv("per_cluster", per_cluster);
    return out;
}

template <typename Fn>
Json measure_with_counters(Fn&& body) {
    PerfGroup g;
    g.add(PerfEvent::CpuCycles);
    g.add(PerfEvent::Instructions);
    g.add(PerfEvent::CacheMisses);
    g.add(PerfEvent::BranchMisses);

    Json out;
    if (!g.available()) {
        body();
        out.kv("pmu_available", false);
        return out;
    }
    g.reset();
    g.start();
    body();
    g.stop();
    out.kv("pmu_available", true).kv("counters", group_to_json(g));
    return out;
}
// Explicit instantiation for the common case (void())
template Json measure_with_counters<void(*)()>(void(*&&)());

} // namespace bench::cpu
