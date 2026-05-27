// Read every Android sensor via the NDK ASensorManager API. Works equally
// well from a NativeActivity-hosted .so and from a standalone ELF launched
// via `adb shell` — neither path needs root, neither needs an app permission
// (sensor data is unrestricted; only HIGH_SAMPLING_RATE_SENSORS on Android 12+
// requires a manifest declaration, which we don't trigger by default).
#pragma once

#include "../json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bench::sensors {

// Reporting modes mirror the SDK constants:
//   0 = CONTINUOUS (e.g. accelerometer, gyro)
//   1 = ON_CHANGE  (e.g. proximity, light)
//   2 = ONE_SHOT   (e.g. significant motion)
//   3 = SPECIAL    (step counter, etc.)
struct SensorMeta {
    int handle = 0;
    int type = 0;                   // ASENSOR_TYPE_* (see android/sensor.h)
    std::string name;
    std::string vendor;
    std::string string_type;        // e.g. "android.sensor.accelerometer"
    float resolution = 0.0f;        // smallest detectable delta in sensor units
    float max_range = 0.0f;         // sensor units
    int32_t min_delay_us = 0;       // minimum event period; 0 == on-change/one-shot
    float power_mA = 0.0f;          // current draw when enabled
    int reporting_mode = 0;
    int fifo_max_event_count = 0;   // hardware FIFO depth (0 = no batching)
    int fifo_reserved_event_count = 0;
};

std::vector<SensorMeta> enumerate();

// Dump enumeration as JSON.
Json enumerate_json();

// Single live reading from one sensor.
struct Reading {
    int handle = 0;
    int type = 0;
    int64_t timestamp_ns = 0;       // sensor-side timestamp (CLOCK_BOOTTIME on most devices)
    int value_count = 0;            // number of meaningful floats in 'values'
    float values[16] = {0};
};

// SensorSampler owns an ASensorManager, an ALooper, and an event queue.
// Standalone mode: pass null package; we'll call ASensorManager_getInstance().
// APK mode: pass a real package name to use the per-package instance, which
// the framework prefers since API 26.
class SensorSampler {
public:
    explicit SensorSampler(const char* package_name = nullptr);
    ~SensorSampler();
    SensorSampler(const SensorSampler&) = delete;
    SensorSampler& operator=(const SensorSampler&) = delete;

    bool valid() const;             // true if manager + queue + looper acquired

    // Enable a sensor at the requested period (microseconds). Returns false if
    // the sensor handle isn't recognized or the queue rejects the rate.
    bool enable(int handle, int32_t period_us);
    bool disable(int handle);

    // Block up to timeout_ms waiting for any events, then drain everything
    // currently queued. Returns the readings. timeout_ms == 0 means non-blocking.
    std::vector<Reading> drain(int timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bench::sensors
