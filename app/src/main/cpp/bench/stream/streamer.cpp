#include "streamer.h"

#include "../affinity.h"
#include "../soc_info.h"
#include "../timer.h"
#include "../sensors/sensors.h"
#include "../thermal/sysfs.h"
#include "../camera/camera.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <map>
#include <unistd.h>

namespace bench::stream {

namespace {

// Signal-safe stop flag — set by SIGINT/SIGTERM handler.
std::atomic<int> g_stop{0};

void on_signal(int /*sig*/) { g_stop.store(1); }

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Reading -> JSON, with named axes per common sensor type for human readability.
Json reading_to_json(const sensors::Reading& r) {
    Json j;
    j.kv("handle", r.handle)
     .kv("type", r.type)
     .kv("ts_ns", r.timestamp_ns);
    // Common labellings (Android sensor docs):
    //   1 ACCEL / 4 GYRO / 2 MAG / 9 GRAVITY / 10 LINEAR_ACCEL  -> x/y/z
    //   11 ROTATION_VECTOR / 15 GAME_ROTATION_VECTOR            -> x/y/z/cos/accuracy
    //   5 LIGHT / 6 PRESSURE / 8 PROXIMITY / 7 AMBIENT_TEMP / 13 AMBIENT_TEMP_v2 / 12 RELATIVE_HUMIDITY / 21 HEART_RATE -> scalar 'v'
    //   19 STEP_COUNTER -> uint64 'count' (we cast from float lossy; framework
    //                     also packs full precision into the first float)
    switch (r.type) {
        case 1: case 4: case 2: case 9: case 10:
            j.kv("x", static_cast<double>(r.values[0]))
             .kv("y", static_cast<double>(r.values[1]))
             .kv("z", static_cast<double>(r.values[2]));
            break;
        case 11: case 15: case 20: // ROTATION_VECTOR, GAME_RV, GEOMAG_RV
            j.kv("x", static_cast<double>(r.values[0]))
             .kv("y", static_cast<double>(r.values[1]))
             .kv("z", static_cast<double>(r.values[2]))
             .kv("cos", static_cast<double>(r.values[3]))
             .kv("accuracy", static_cast<double>(r.values[4]));
            break;
        case 5: case 6: case 8: case 7: case 12: case 13: case 21:
            j.kv("v", static_cast<double>(r.values[0]));
            break;
        case 19:
            j.kv("count", static_cast<double>(r.values[0]));
            break;
        default:
            std::vector<double> raw(r.values, r.values + 8); // first 8 floats
            j.kv("raw", raw);
            break;
    }
    return j;
}

void emit(const Json& obj) {
    std::printf("%s\n", obj.str().c_str());
    std::fflush(stdout);  // critical for live consumption via adb shell pipe
}

} // namespace

void run(const StreamConfig& cfg) {
    install_signal_handlers();

    // -- Header line: env + sensor enumeration + camera enumeration --------
    sensors::SensorSampler sampler;
    auto sensor_list = sensors::enumerate();
    int num_cpus = bench::num_cpus();
    auto thermal_zones = thermal::list_thermal_zones();

    Json env;
    auto soc = collect_soc_info();
    env.kv("soc", identify_soc(soc))
       .kv("model", soc.model)
       .kv("board", soc.board)
       .kv("android_release", soc.android_release)
       .kv("android_sdk", soc.android_sdk)
       .kv("num_cpus", num_cpus);

    std::vector<Json> zone_json;
    for (const auto& z : thermal_zones) {
        Json zj; zj.kv("idx", z.idx).kv("type", z.type);
        zone_json.push_back(std::move(zj));
    }

    Json header;
    header.kv("kind", "header")
          .kv("env", env)
          .kv("thermal_zones", zone_json)
          .kv("sensors_meta", sensors::enumerate_json())
          .kv("cameras", camera::enumerate_json())
          .kv("config_interval_ms", cfg.interval_ms)
          .kv("config_sensor_rate_us", cfg.sensor_rate_us)
          .kv("started_at_ns", now_ns());
    emit(header);

    // -- Enable sensors -----------------------------------------------------
    // Map handle -> latest reading so we always emit the most recent value.
    std::map<int, sensors::Reading> latest;

    if (cfg.enable_sensors && sampler.valid()) {
        for (const auto& m : sensor_list) {
            // Reporting mode: 0=continuous, 1=on-change, 2=one-shot, 3=special.
            // We accept on-change too (proximity, light, step-detector etc.)
            // because those still update meaningfully. We skip one-shot to
            // avoid trip-and-disable cycles, and skip "special" (step-counter
            // batching, etc.) unless explicitly enabled.
            if (cfg.enable_continuous_only && m.reporting_mode != 0 && m.reporting_mode != 1) continue;
            // setEventRate honors min_delay_us as the floor; we ask for our
            // configured rate or the floor, whichever is larger.
            int32_t rate = cfg.sensor_rate_us;
            if (m.min_delay_us > 0 && rate < m.min_delay_us) rate = m.min_delay_us;
            sampler.enable(m.handle, rate);
        }
    }

    // -- Main loop ---------------------------------------------------------
    while (!g_stop.load()) {
        uint64_t t_frame_start = now_ns();

        // Drain sensor events with a short blocking wait, so we don't spin.
        if (sampler.valid()) {
            auto readings = sampler.drain(cfg.interval_ms);
            for (const auto& r : readings) latest[r.handle] = r;
        } else {
            // No sensor manager — just sleep for the interval.
            usleep(static_cast<useconds_t>(cfg.interval_ms) * 1000);
        }

        // Snapshot thermal + freqs.
        Json frame;
        frame.kv("kind", "frame")
             .kv("t_ns", now_ns())
             .kv("dt_ms", static_cast<int64_t>((now_ns() - t_frame_start) / 1'000'000ull));

        if (cfg.enable_thermal && !thermal_zones.empty()) {
            auto temps = thermal::sample_temps_mc(thermal_zones);
            std::vector<double> temps_d(temps.begin(), temps.end());
            frame.kv("temps_mc", temps_d);
        }
        if (cfg.enable_freqs) {
            auto freqs = thermal::sample_cpu_freqs_khz(num_cpus);
            std::vector<double> freqs_d(freqs.begin(), freqs.end());
            frame.kv("freqs_khz", freqs_d);
        }

        // Sensor snapshot — emit only sensors we have data for.
        std::vector<Json> sensor_frame;
        sensor_frame.reserve(latest.size());
        for (const auto& kv : latest) {
            sensor_frame.push_back(reading_to_json(kv.second));
        }
        frame.kv("sensors", sensor_frame);

        emit(frame);
    }

    // Footer so a consumer can detect a clean exit.
    Json footer;
    footer.kv("kind", "footer").kv("stopped_at_ns", now_ns());
    emit(footer);
}

} // namespace bench::stream
