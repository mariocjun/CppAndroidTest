// cppbench: standalone benchmark harness, intended to be pushed via
//   adb push <build>/cppbench /data/local/tmp/ && adb shell /data/local/tmp/cppbench --json
// and produce JSON on stdout that scripts/compare.py can diff between devices.
//
// CLI:
//   --json           emit results as a single JSON object (no human prose)
//   --filter=NAME    run only the named benchmark (substring match)
//   --iters=N        override default iteration count
//   --elems=N        override per-array element count for memory benchmarks
//   --list           list available benchmarks and exit
//   --help           print usage
#include "affinity.h"
#include "json.h"
#include "soc_info.h"
#include "timer.h"
#include "cpu/stream.h"
#include "cpu/latency.h"
#include "cpu/neon_fma.h"
#include "cpu/sustained.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Args {
    bool json = false;
    bool list = false;
    std::string filter;
    int iters = 0;       // 0 == use bench default
    size_t elems = 0;    // 0 == use bench default
};

bool arg_match(const char* a, const char* prefix, const char** value_out) {
    size_t plen = std::strlen(prefix);
    if (std::strncmp(a, prefix, plen) == 0) {
        if (a[plen] == '=') { *value_out = a + plen + 1; return true; }
        if (a[plen] == '\0') { *value_out = nullptr; return true; }
    }
    return false;
}

void print_help() {
    std::fprintf(stderr,
        "cppbench — Note10+ Exynos / Snapdragon performance harness\n\n"
        "Usage: cppbench [--json] [--filter=NAME] [--iters=N] [--elems=N]\n"
        "                [--list] [--help]\n\n"
        "  --json         emit a single JSON object on stdout (no human prose)\n"
        "  --filter=NAME  run only benchmarks whose name contains NAME\n"
        "  --iters=N      override default iteration count\n"
        "  --elems=N      override per-array element count (memory benchmarks)\n"
        "  --list         list available benchmarks and exit\n"
        "  --help         this message\n");
}

const std::vector<std::string>& available_benchmarks() {
    static const std::vector<std::string> v = {"stream", "latency", "neon_fma", "sustained"};
    return v;
}

bool wanted(const std::string& filter, const std::string& name) {
    return filter.empty() || name.find(filter) != std::string::npos;
}

bench::Json build_env_info() {
    bench::Json env;
    auto soc = bench::collect_soc_info();
    env.kv("soc_identified", bench::identify_soc(soc))
       .kv("ro_hardware", soc.hardware)
       .kv("ro_board_platform", soc.board)
       .kv("ro_product_brand", soc.brand)
       .kv("ro_product_model", soc.model)
       .kv("android_release", soc.android_release)
       .kv("android_sdk", soc.android_sdk)
       .kv("cpu_implementer", soc.cpu_implementer)
       .kv("cpu_part_first_seen", soc.cpu_part)
       .kv("cpu_features", soc.features)
       .kv("mem_total_kb", soc.mem_total_kb)
       .kv("page_size", soc.page_size);

    std::vector<bench::Json> clusters_json;
    for (const auto& c : bench::detect_clusters()) {
        bench::Json cj;
        cj.kv("cpu_min", c.min_id)
          .kv("cpu_max", c.max_id)
          .kv("max_freq_khz", c.max_freq_khz);
        clusters_json.push_back(std::move(cj));
    }
    env.kv("cpu_clusters", clusters_json);
    env.kv("num_cpus", bench::num_cpus());
    env.kv("timestamp_ns", bench::now_ns());
    return env;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = nullptr;
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_help(); return 0;
        }
        if (std::strcmp(a, "--list") == 0) { args.list = true; continue; }
        if (std::strcmp(a, "--json") == 0) { args.json = true; continue; }
        if (arg_match(a, "--filter", &v) && v) { args.filter = v; continue; }
        if (arg_match(a, "--iters",  &v) && v) { args.iters = std::atoi(v); continue; }
        if (arg_match(a, "--elems",  &v) && v) {
            args.elems = static_cast<size_t>(std::strtoull(v, nullptr, 10));
            continue;
        }
        std::fprintf(stderr, "cppbench: unknown argument: %s\n", a);
        print_help();
        return 2;
    }

    if (args.list) {
        for (const auto& n : available_benchmarks()) std::printf("%s\n", n.c_str());
        return 0;
    }

    auto env = build_env_info();
    auto clusters = bench::detect_clusters();

    std::vector<std::pair<std::string, bench::Json>> results;

    if (wanted(args.filter, "stream")) {
        bench::cpu::StreamConfig cfg;
        if (args.iters > 0) cfg.iterations = args.iters;
        if (args.elems > 0) cfg.elems = args.elems;
        auto r = bench::cpu::run_stream_per_cluster(cfg, clusters);
        results.emplace_back("stream", std::move(r));
    }
    if (wanted(args.filter, "latency")) {
        bench::cpu::LatencyConfig cfg;
        if (args.iters > 0) cfg.iterations = args.iters;
        auto r = bench::cpu::run_latency_per_cluster(cfg, clusters);
        results.emplace_back("latency", std::move(r));
    }
    if (wanted(args.filter, "neon_fma")) {
        bench::cpu::NeonFmaConfig cfg;
        if (args.iters > 0) cfg.iterations = args.iters;
        auto r = bench::cpu::run_neon_fma_per_cluster(cfg, clusters);
        results.emplace_back("neon_fma", std::move(r));
    }
    if (wanted(args.filter, "sustained")) {
        bench::cpu::SustainedConfig cfg;
        // 'sustained' is opt-in only — heavy (30s+ default) and produces a
        // multi-MB JSON blob; user must explicitly --filter=sustained.
        if (args.filter.find("sustained") != std::string::npos) {
            auto r = bench::cpu::run_sustained_on_big_core(cfg, clusters);
            results.emplace_back("sustained", std::move(r));
        }
    }

    bench::Json top;
    top.kv("env", env);
    std::vector<bench::Json> bench_array;
    for (auto& [name, j] : results) {
        bench::Json wrap;
        wrap.kv("name", name).kv("result", j);
        bench_array.push_back(std::move(wrap));
    }
    top.kv("benchmarks", bench_array);

    if (args.json) {
        std::printf("%s\n", top.str().c_str());
    } else {
        // Human-readable: print env + key numbers, then dump JSON.
        std::printf("SoC:    %s (%s / %s)\n",
                    bench::identify_soc(bench::collect_soc_info()).c_str(),
                    bench::collect_soc_info().model.c_str(),
                    bench::collect_soc_info().board.c_str());
        std::printf("CPUs:   %d total, %zu clusters\n",
                    bench::num_cpus(), clusters.size());
        for (size_t i = 0; i < clusters.size(); ++i) {
            const auto& c = clusters[i];
            std::printf("  cluster %zu: cpu%d-%d @ %.2f GHz (%d cores)\n",
                        i, c.min_id, c.max_id,
                        c.max_freq_khz / 1.0e6, c.core_count());
        }
        std::printf("\n--- JSON ---\n%s\n", top.str().c_str());
    }
    return 0;
}
