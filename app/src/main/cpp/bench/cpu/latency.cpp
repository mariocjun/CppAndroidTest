// Pointer-chase memory latency benchmark.
//
// Method:
//   1. Allocate a buffer of S bytes, treat it as an array of (S / sizeof(void*))
//      void*-slots, each one cache-line-spaced (so the index step is 64 bytes,
//      not sizeof(void*)). This puts every "node" on its own cache line and
//      keeps line-spatial-locality from bleeding into the result.
//   2. Build a random Hamiltonian cycle through the nodes — each slot points
//      to the next one in a permuted order. The chain length equals the node
//      count; following all of them visits every node exactly once.
//   3. Time a dependent-load loop that follows the chain for N steps. Each
//      load address depends on the previous load's result, so the CPU's
//      stride/stream prefetchers can't help — every miss costs a full round trip.
//   4. ns_per_load = (elapsed_ns) / N. As S grows past each cache level the
//      number jumps to the next-tier latency, mapping out the hierarchy.
//
// Anti-cheat:
//   - The first iteration's result is fed back as a seed so dead-code
//     elimination can't remove the chase loop.
//   - sizeof(node) == cache line (64 B on ARM A55/A75/A76/M4) prevents the
//     hardware spatial prefetcher from amortizing the cost across hits.
//   - Random permutation prevents the stride prefetcher from detecting
//     a pattern.
//
// What you should see on Note10+:
//   4 KB:    ~1.0-1.5 ns/load  (L1d hit, ~3-4 cycles)
//   32-64 KB ~1.5-2.0 ns/load  (still L1d on M4/A76, edge of L1d on A55)
//   128-256K ~4-6 ns/load      (L2 hit)
//   1 MB:    ~5-8 ns/load      (L2)
//   4-16 MB: ~15-30 ns/load    (SLC / DSU)
//   64 MB:   ~90-160 ns/load   (DRAM)
#include "latency.h"

#include "../timer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

namespace bench::cpu {

namespace {

constexpr size_t LINE = 64;  // cache line size; same on A55/A76/M4

void* aligned_alloc_bytes(size_t bytes, size_t alignment = LINE) {
    void* p = nullptr;
    // posix_memalign rounds size internally, but we still want page-sized for
    // very large buffers — that's fine, 64-byte alignment is a subset.
    if (posix_memalign(&p, alignment, bytes) != 0) return nullptr;
    return p;
}

// Build a buffer of 'nodes' slots, line-spaced, each pointing to the next slot
// in a random permutation. Returns the buffer base and the entry pointer.
struct Chain { void* base = nullptr; void** entry = nullptr; size_t nodes = 0; };

Chain build_chain(size_t bytes, std::mt19937_64& rng) {
    size_t nodes = bytes / LINE;
    if (nodes < 2) nodes = 2;
    size_t total = nodes * LINE;
    void* base = aligned_alloc_bytes(total);
    if (!base) return {};

    // Each node's payload is its own slot's first word: a void* to the next.
    std::vector<size_t> order(nodes);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), rng);

    auto slot_addr = [&](size_t idx) -> void** {
        return reinterpret_cast<void**>(static_cast<uint8_t*>(base) + idx * LINE);
    };

    for (size_t i = 0; i < nodes; ++i) {
        *slot_addr(order[i]) = slot_addr(order[(i + 1) % nodes]);
    }
    return { base, slot_addr(order[0]), nodes };
}

// Pin a sink so the optimizer can't drop the chase loop.
volatile uintptr_t g_sink_addr;

// Returns ns per dependent load.
double chase(const Chain& ch, int steps) {
    void** p = ch.entry;
    uint64_t t0 = now_ns();
    // Hand-unrolled by 8 — gives the OoO core enough in-flight loads to keep
    // the pipeline fed; doesn't break the dependency chain since each load's
    // address still comes from the previous load.
    int n = steps;
    while (n >= 8) {
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        p = static_cast<void**>(*p);
        n -= 8;
    }
    while (n > 0) {
        p = static_cast<void**>(*p);
        --n;
    }
    uint64_t t1 = now_ns();
    g_sink_addr = reinterpret_cast<uintptr_t>(p);
    return static_cast<double>(t1 - t0) / static_cast<double>(steps);
}

} // namespace

Json run_latency(const LatencyConfig& cfg) {
    std::mt19937_64 rng(0xC0FFEE'CAFE'BABEull);  // fixed seed → reproducible

    std::vector<double> sizes;
    std::vector<double> ns;
    std::vector<double> nodes_per_size;

    sizes.reserve(cfg.sizes_bytes.size());
    ns.reserve(cfg.sizes_bytes.size());
    nodes_per_size.reserve(cfg.sizes_bytes.size());

    for (size_t s : cfg.sizes_bytes) {
        Chain ch = build_chain(s, rng);
        if (!ch.base) {
            sizes.push_back(static_cast<double>(s));
            ns.push_back(-1.0);
            nodes_per_size.push_back(0);
            continue;
        }

        // Warm: walk the chain once to populate caches at each level appropriately.
        for (int w = 0; w < cfg.warmup; ++w) chase(ch, cfg.chase_steps);

        double best = 1e30;
        for (int i = 0; i < cfg.iterations; ++i) {
            double v = chase(ch, cfg.chase_steps);
            if (v < best) best = v;
        }
        sizes.push_back(static_cast<double>(s));
        ns.push_back(best);
        nodes_per_size.push_back(static_cast<double>(ch.nodes));
        std::free(ch.base);
    }

    Json out;
    out.kv("sizes_bytes", sizes)
       .kv("ns_per_load", ns)
       .kv("nodes_per_size", nodes_per_size)
       .kv("chase_steps", cfg.chase_steps)
       .kv("iterations", cfg.iterations);
    return out;
}

Json run_latency_per_cluster(const LatencyConfig& cfg,
                             const std::vector<CpuCluster>& clusters) {
    std::vector<Json> rows;
    rows.reserve(clusters.size());
    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cluster(c);
        Json r = run_latency(cfg);
        Json wrap;
        wrap.kv("cluster_idx", static_cast<int64_t>(i))
            .kv("cpu_min", c.min_id)
            .kv("cpu_max", c.max_id)
            .kv("max_freq_khz", c.max_freq_khz)
            .kv("pinned", pinned)
            .kv("latency", r);
        rows.push_back(std::move(wrap));
    }
    Json out;
    out.kv("per_cluster", rows);
    return out;
}

} // namespace bench::cpu
