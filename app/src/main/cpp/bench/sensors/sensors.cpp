#include "sensors.h"

#include <android/looper.h>
#include <android/sensor.h>
#include <cstring>

namespace bench::sensors {

namespace {

constexpr int LOOPER_ID = 3;  // arbitrary identifier; we only have one queue here.

const char* nullable(const char* p) { return p ? p : ""; }

SensorMeta meta_from(const ASensor* s) {
    SensorMeta m;
    m.handle = ASensor_getHandle(s);
    m.type = ASensor_getType(s);
    m.name = nullable(ASensor_getName(s));
    m.vendor = nullable(ASensor_getVendor(s));
    m.string_type = nullable(ASensor_getStringType(s));
    m.resolution = ASensor_getResolution(s);
    m.min_delay_us = ASensor_getMinDelay(s);
    m.max_range = 0.0f;  // no getter in the public NDK ASensor; left at 0
    m.power_mA = 0.0f;   // no getter in the public NDK ASensor; left at 0
    m.reporting_mode = ASensor_getReportingMode(s);
    m.fifo_max_event_count = ASensor_getFifoMaxEventCount(s);
    m.fifo_reserved_event_count = ASensor_getFifoReservedEventCount(s);
    return m;
}

} // namespace

std::vector<SensorMeta> enumerate() {
    std::vector<SensorMeta> out;
    ASensorManager* mgr = ASensorManager_getInstance();
    if (!mgr) return out;
    ASensorList list = nullptr;
    int count = ASensorManager_getSensorList(mgr, &list);
    if (count <= 0 || !list) return out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (!list[i]) continue;
        out.push_back(meta_from(list[i]));
    }
    return out;
}

Json enumerate_json() {
    std::vector<Json> rows;
    for (const auto& m : enumerate()) {
        Json j;
        j.kv("handle", m.handle)
         .kv("type", m.type)
         .kv("name", m.name)
         .kv("vendor", m.vendor)
         .kv("string_type", m.string_type)
         .kv("resolution", static_cast<double>(m.resolution))
         .kv("min_delay_us", m.min_delay_us)
         .kv("reporting_mode", m.reporting_mode)
         .kv("fifo_max", m.fifo_max_event_count)
         .kv("fifo_reserved", m.fifo_reserved_event_count);
        rows.push_back(std::move(j));
    }
    Json out;
    out.kv("count", static_cast<int64_t>(rows.size()))
       .kv("sensors", rows);
    return out;
}

// -- SensorSampler -----------------------------------------------------------

struct SensorSampler::Impl {
    ASensorManager* mgr = nullptr;
    ALooper* looper = nullptr;
    ASensorEventQueue* queue = nullptr;
    // Cache: handle -> ASensor*. ASensorManager_getDefaultSensorEx isn't in
    // the NDK; we use the enumeration to find a sensor by handle for enable().
    std::vector<const ASensor*> sensors_by_handle;
    std::vector<int> handles_known;  // parallel to sensors_by_handle
};

namespace {

void populate_sensor_map(SensorSampler::Impl& impl) {
    ASensorList list = nullptr;
    int count = ASensorManager_getSensorList(impl.mgr, &list);
    if (count <= 0 || !list) return;
    impl.sensors_by_handle.reserve(static_cast<size_t>(count));
    impl.handles_known.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (!list[i]) continue;
        impl.sensors_by_handle.push_back(list[i]);
        impl.handles_known.push_back(ASensor_getHandle(list[i]));
    }
}

const ASensor* find_by_handle(const SensorSampler::Impl& impl, int handle) {
    for (size_t i = 0; i < impl.handles_known.size(); ++i) {
        if (impl.handles_known[i] == handle) return impl.sensors_by_handle[i];
    }
    return nullptr;
}

} // namespace

SensorSampler::SensorSampler(const char* package_name) : impl_(std::make_unique<Impl>()) {
    if (package_name && *package_name) {
        impl_->mgr = ASensorManager_getInstanceForPackage(package_name);
    } else {
        // Deprecated since API 26 but still functional; the only path that
        // works from a non-app context (e.g. `adb shell`) without a package.
        impl_->mgr = ASensorManager_getInstance();
    }
    if (!impl_->mgr) return;

    // ALooper_prepare returns the looper already associated with the thread,
    // or creates a fresh one. ALLOW_NON_CALLBACKS lets us pollOnce without
    // attaching ALooper_addFd callbacks for every event source.
    impl_->looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!impl_->looper) return;

    impl_->queue = ASensorManager_createEventQueue(
        impl_->mgr, impl_->looper, LOOPER_ID, /*ALooper_callbackFunc*/ nullptr, /*data*/ nullptr);
    if (!impl_->queue) return;

    populate_sensor_map(*impl_);
}

SensorSampler::~SensorSampler() {
    if (!impl_) return;
    if (impl_->queue && impl_->mgr) {
        for (int handle : impl_->handles_known) {
            const ASensor* s = find_by_handle(*impl_, handle);
            if (s) ASensorEventQueue_disableSensor(impl_->queue, s);
        }
        ASensorManager_destroyEventQueue(impl_->mgr, impl_->queue);
    }
}

bool SensorSampler::valid() const {
    return impl_ && impl_->mgr && impl_->looper && impl_->queue;
}

bool SensorSampler::enable(int handle, int32_t period_us) {
    if (!valid()) return false;
    const ASensor* s = find_by_handle(*impl_, handle);
    if (!s) return false;
    if (ASensorEventQueue_enableSensor(impl_->queue, s) != 0) return false;
    if (ASensorEventQueue_setEventRate(impl_->queue, s, period_us) != 0) return false;
    return true;
}

bool SensorSampler::disable(int handle) {
    if (!valid()) return false;
    const ASensor* s = find_by_handle(*impl_, handle);
    if (!s) return false;
    return ASensorEventQueue_disableSensor(impl_->queue, s) == 0;
}

std::vector<Reading> SensorSampler::drain(int timeout_ms) {
    std::vector<Reading> out;
    if (!valid()) return out;

    // ALooper_pollOnce blocks until either (a) timeout, (b) an event source
    // fires (our queue is a registered source), or (c) a wake. We then drain
    // the queue fully.
    int ident = ALooper_pollOnce(timeout_ms, nullptr, nullptr, nullptr);
    (void)ident;  // could be ALOOPER_POLL_TIMEOUT/ERROR/CALLBACK or our LOOPER_ID

    ASensorEvent buf[64];
    while (true) {
        int n = ASensorEventQueue_getEvents(impl_->queue, buf, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            Reading r;
            r.handle = buf[i].sensor;
            r.type = buf[i].type;
            r.timestamp_ns = buf[i].timestamp;
            // Copy up to 16 floats from the union — covers vectors (3),
            // rotation vectors (5), and any per-type scalars.
            r.value_count = 16;
            std::memcpy(r.values, buf[i].data, sizeof(r.values));
            out.push_back(r);
        }
    }
    return out;
}

} // namespace bench::sensors
