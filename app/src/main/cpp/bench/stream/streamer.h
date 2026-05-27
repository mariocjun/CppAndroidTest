#pragma once

#include "../json.h"

namespace bench::stream {

struct StreamConfig {
    int interval_ms = 250;          // emit one NDJSON frame per interval
    int sensor_rate_us = 50000;     // 20 Hz default sensor update rate
    bool enable_sensors = true;
    bool enable_thermal = true;
    bool enable_freqs = true;
    bool enable_continuous_only = true;  // skip ON_CHANGE/ONE_SHOT/SPECIAL by default
};

// Continuously emit NDJSON to stdout (one JSON object per line) until SIGINT.
// First line is a "header" with metadata: SoC info, sensor list, thermal zones.
// Subsequent lines are "frames" with current readings.
//
// Run via:
//   adb shell /data/local/tmp/cppbench --stream
// or piped into the dashboard for live ANSI rendering:
//   python scripts/dashboard.py
void run(const StreamConfig& cfg);

} // namespace bench::stream
