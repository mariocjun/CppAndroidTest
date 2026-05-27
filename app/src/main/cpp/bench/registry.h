// Benchmark registry (C++20).
//
// Each benchmark is represented by a stateless wrapper struct that exposes:
//   - a compile-time `name`        : how the user selects it on the CLI
//   - a `Config` typedef           : the per-benchmark configuration struct
//   - `make_config(const Args&)`   : maps CLI args -> Config
//   - `opt_in()`                   : true iff the benchmark must be explicitly
//                                    --filter'd (e.g. 'sustained' takes 30s+)
//   - `run_per_cluster(cfg, cl)`   : returns a Json with the result
//
// `Benchmark<T>` (the concept) enforces the contract at compile time. The
// fold expression in `dispatch` instantiates `try_one` for every wrapper in
// the tuple, so adding a new benchmark = adding one wrapper struct + one
// entry in the `Registry` tuple alias. The body of `bench_main.cpp` doesn't
// change.
#pragma once

#include "affinity.h"
#include "json.h"

#include "cpu/stream.h"
#include "cpu/latency.h"
#include "cpu/neon_fma.h"
#include "cpu/dot_int8.h"
#include "cpu/sustained.h"

#include <concepts>
#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bench::registry {

// CLI-arg view passed to each wrapper's make_config(). Field names mirror
// bench_main's local Args; we re-declare here so registry.h doesn't depend
// on the CLI parsing TU.
struct Args {
    std::string filter;
    int iters = 0;
    std::size_t elems = 0;
};

template <typename T>
concept Benchmark = requires(T b, const Args& args,
                              const typename T::Config& cfg,
                              const std::vector<bench::CpuCluster>& clusters) {
    { T::name }                       -> std::convertible_to<const char*>;
    { T::make_config(args) }          -> std::same_as<typename T::Config>;
    { T::opt_in() }                   -> std::same_as<bool>;
    { b.run_per_cluster(cfg, clusters) } -> std::same_as<bench::Json>;
};

inline bool wanted(const std::string& filter, const char* name) {
    return filter.empty() || filter.find(name) != std::string::npos;
}

// -- Wrappers ---------------------------------------------------------------

struct StreamBench {
    static constexpr const char* name = "stream";
    using Config = bench::cpu::StreamConfig;
    static Config make_config(const Args& a) {
        Config c;
        if (a.iters > 0) c.iterations = a.iters;
        if (a.elems > 0) c.elems = a.elems;
        return c;
    }
    static bool opt_in() { return false; }
    bench::Json run_per_cluster(const Config& cfg,
                                const std::vector<bench::CpuCluster>& cl) const {
        return bench::cpu::run_stream_per_cluster(cfg, cl);
    }
};

struct LatencyBench {
    static constexpr const char* name = "latency";
    using Config = bench::cpu::LatencyConfig;
    static Config make_config(const Args& a) {
        Config c;
        if (a.iters > 0) c.iterations = a.iters;
        return c;
    }
    static bool opt_in() { return false; }
    bench::Json run_per_cluster(const Config& cfg,
                                const std::vector<bench::CpuCluster>& cl) const {
        return bench::cpu::run_latency_per_cluster(cfg, cl);
    }
};

struct NeonFmaBench {
    static constexpr const char* name = "neon_fma";
    using Config = bench::cpu::NeonFmaConfig;
    static Config make_config(const Args& a) {
        Config c;
        if (a.iters > 0) c.iterations = a.iters;
        return c;
    }
    static bool opt_in() { return false; }
    bench::Json run_per_cluster(const Config& cfg,
                                const std::vector<bench::CpuCluster>& cl) const {
        return bench::cpu::run_neon_fma_per_cluster(cfg, cl);
    }
};

struct DotInt8Bench {
    static constexpr const char* name = "dot_int8";
    using Config = bench::cpu::DotInt8Config;
    static Config make_config(const Args& a) {
        Config c;
        if (a.iters > 0) c.iterations = a.iters;
        return c;
    }
    static bool opt_in() { return false; }
    bench::Json run_per_cluster(const Config& cfg,
                                const std::vector<bench::CpuCluster>& cl) const {
        return bench::cpu::run_dot_int8_per_cluster(cfg, cl);
    }
};

struct SustainedBench {
    static constexpr const char* name = "sustained";
    using Config = bench::cpu::SustainedConfig;
    static Config make_config(const Args&) { return Config{}; }
    // Heavy (30s+ default, multi-MB JSON) — only runs when explicitly filtered.
    static bool opt_in() { return true; }
    bench::Json run_per_cluster(const Config& cfg,
                                const std::vector<bench::CpuCluster>& cl) const {
        return bench::cpu::run_sustained_on_big_core(cfg, cl);
    }
};

// -- Registry tuple ---------------------------------------------------------
// Adding a benchmark: write the wrapper above, then add its type here.
// The Benchmark<T> concept blocks compilation if the wrapper is malformed.
using Registry = std::tuple<StreamBench, LatencyBench, NeonFmaBench,
                            DotInt8Bench, SustainedBench>;

// Static check: every wrapper in Registry satisfies Benchmark<T>.
template <typename... Bs>
constexpr bool all_benchmarks(std::tuple<Bs...>*) {
    return (Benchmark<Bs> && ...);
}
static_assert(all_benchmarks(static_cast<Registry*>(nullptr)),
              "every entry in Registry must satisfy Benchmark<T>");

// -- Helpers ----------------------------------------------------------------

template <typename... Bs>
std::vector<const char*> names(std::tuple<Bs...>) {
    return {Bs::name...};
}

inline std::vector<const char*> all_names() {
    return names(Registry{});
}

// Run every wrapper in Registry against the given args/clusters and return
// the (name, json) pairs for results we actually produced.
inline std::vector<std::pair<std::string, bench::Json>>
dispatch(const Args& args, const std::vector<bench::CpuCluster>& clusters) {
    std::vector<std::pair<std::string, bench::Json>> results;

    auto try_one = [&](auto& b) {
        using B = std::decay_t<decltype(b)>;
        if (!wanted(args.filter, B::name)) return;
        // Opt-in benchmarks require the filter to explicitly name them
        // (an empty filter selects "all default benchmarks", not opt-ins).
        if (B::opt_in() && args.filter.find(B::name) == std::string::npos) return;
        auto cfg = B::make_config(args);
        results.emplace_back(B::name, b.run_per_cluster(cfg, clusters));
    };

    std::apply([&](auto&... bs) { (try_one(bs), ...); }, Registry{});
    return results;
}

} // namespace bench::registry
