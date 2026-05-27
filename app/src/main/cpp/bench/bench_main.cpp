// cppbench: standalone CLI for benchmarks, sensor enumeration, camera
// enumeration, and live streaming. Pushed via
//   adb push <build>/cppbench /data/local/tmp/ && adb shell /data/local/tmp/cppbench ...
//
// One-shot benchmarks (default action):
//   cppbench --json
//   cppbench --filter=stream --json
//
// Live dynamic stream (NDJSON to stdout, runs until Ctrl-C):
//   cppbench --stream
//   adb shell /data/local/tmp/cppbench --stream | python scripts/dashboard.py
//
// Hardware enumeration (one-shot):
//   cppbench --sensors    # all Android sensors with characteristics
//   cppbench --cameras    # all cameras via NDK Camera2 (no capture, just metadata)
//
// All benchmarks are dispatched through bench::registry::dispatch — adding a
// new benchmark is two lines (wrapper struct + tuple entry), this file does
// not need to change.
#include "affinity.h"
#include "json.h"
#include "registry.h"
#include "soc_info.h"
#include "timer.h"
#include "sensors/sensors.h"
#include "camera/camera.h"
#include "stream/streamer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct CliArgs {
    bool json = false;
    bool list = false;
    bool stream = false;
    bool dump_sensors = false;
    bool dump_cameras = false;
    std::string filter;
    int iters = 0;
    std::size_t elems = 0;
    int interval_ms = 250;
    int sensor_rate_us = 50000;
};

bool arg_match(const char* a, const char* prefix, const char** value_out) {
    std::size_t plen = std::strlen(prefix);
    if (std::strncmp(a, prefix, plen) == 0) {
        if (a[plen] == '=') { *value_out = a + plen + 1; return true; }
        if (a[plen] == '\0') { *value_out = nullptr; return true; }
    }
    return false;
}

void print_help() {
    std::fprintf(stderr,
        "cppbench — Android perf + sensors + camera enumeration / live stream\n\n"
        "Modes:\n"
        "  cppbench [--json] [--filter=NAME] [--iters=N] [--elems=N]\n"
        "      Run benchmarks. Default selection: every registered benchmark\n"
        "      except those marked opt-in (currently: sustained).\n"
        "  cppbench --stream [--interval-ms=N] [--sensor-rate-us=N]\n"
        "      Continuous NDJSON output of sensors+thermal+freqs until SIGINT.\n"
        "      Pipe through python scripts/dashboard.py for a live terminal view.\n"
        "  cppbench --sensors\n"
        "      Dump JSON enumeration of all Android sensors and exit.\n"
        "  cppbench --cameras\n"
        "      Dump JSON enumeration of all Camera2 cameras and exit.\n"
        "  cppbench --list\n"
        "      List available benchmark names.\n"
        "  cppbench --help\n");
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
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = nullptr;
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_help(); return 0;
        }
        if (std::strcmp(a, "--list") == 0) { args.list = true; continue; }
        if (std::strcmp(a, "--json") == 0) { args.json = true; continue; }
        if (std::strcmp(a, "--stream") == 0) { args.stream = true; continue; }
        if (std::strcmp(a, "--sensors") == 0) { args.dump_sensors = true; continue; }
        if (std::strcmp(a, "--cameras") == 0) { args.dump_cameras = true; continue; }
        if (arg_match(a, "--filter", &v) && v) { args.filter = v; continue; }
        if (arg_match(a, "--iters",  &v) && v) { args.iters = std::atoi(v); continue; }
        if (arg_match(a, "--elems",  &v) && v) {
            args.elems = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
            continue;
        }
        if (arg_match(a, "--interval-ms", &v) && v) { args.interval_ms = std::atoi(v); continue; }
        if (arg_match(a, "--sensor-rate-us", &v) && v) { args.sensor_rate_us = std::atoi(v); continue; }
        std::fprintf(stderr, "cppbench: unknown argument: %s\n", a);
        print_help();
        return 2;
    }

    if (args.list) {
        for (const char* n : bench::registry::all_names()) std::printf("%s\n", n);
        return 0;
    }

    if (args.dump_sensors) {
        std::printf("%s\n", bench::sensors::enumerate_json().str().c_str());
        return 0;
    }

    if (args.dump_cameras) {
        std::printf("%s\n", bench::camera::enumerate_json().str().c_str());
        return 0;
    }

    if (args.stream) {
        bench::stream::StreamConfig cfg{
            .interval_ms = args.interval_ms,
            .sensor_rate_us = args.sensor_rate_us,
        };
        bench::stream::run(cfg);
        return 0;
    }

    auto env = build_env_info();
    auto clusters = bench::detect_clusters();

    bench::registry::Args reg_args{
        .filter = args.filter,
        .iters = args.iters,
        .elems = args.elems,
    };
    auto results = bench::registry::dispatch(reg_args, clusters);

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
        auto soc = bench::collect_soc_info();
        std::printf("SoC:    %s (%s / %s)\n",
                    bench::identify_soc(soc).c_str(),
                    soc.model.c_str(), soc.board.c_str());
        std::printf("CPUs:   %d total, %zu clusters\n",
                    bench::num_cpus(), clusters.size());
        for (std::size_t i = 0; i < clusters.size(); ++i) {
            const auto& c = clusters[i];
            std::printf("  cluster %zu: cpu%d-%d @ %.2f GHz (%d cores)\n",
                        i, c.min_id, c.max_id,
                        c.max_freq_khz / 1.0e6, c.core_count());
        }
        std::printf("\n--- JSON ---\n%s\n", top.str().c_str());
    }
    return 0;
}
