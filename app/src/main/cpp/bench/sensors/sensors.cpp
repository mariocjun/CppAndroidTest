#include "sensors.h"

#include <android/looper.h>
#include <android/sensor.h>
#include <cstring>

namespace bench::sensors {

namespace {

constexpr int LOOPER_ID = 3;  // arbitrary; we only have one queue.

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
    m.reporting_mode = ASensor_getReportingMode(s);
    m.fifo_max_event_count = ASensor_getFifoMaxEventCount(s);
    m.fifo_reserved_event_count = ASensor_getFifoReservedEventCount(s);
    return m;
}

} // namespace

// Impl lives in the bench::sensors namespace (not nested in SensorSampler)
// so anonymous-namespace helpers in this TU can access its members. The .h
// only forward-declares it via the std::unique_ptr<Impl> member.
struct SensorSampler::Impl {
    ASensorManager* mgr = nullptr;
    ALooper* looper = nullptr;
    ASensorEventQueue* queue = nullptr;
    // Parallel arrays: handle <-> ASensor* (so we can resolve enable/disable
    // requests without re-walking the framework's list every call).
    std::vector<const ASensor*> sensors_by_pos;
    std::vector<int> handles_by_pos;

    const ASensor* find_by_handle(int handle) const {
        for (size_t i = 0; i < handles_by_pos.size(); ++i) {
            if (handles_by_pos[i] == handle) return sensors_by_pos[i];
        }
        return nullptr;
    }
};

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

SensorSampler::SensorSampler(const char* package_name) : impl_(std::make_unique<Impl>()) {
    // getInstanceForPackage is the preferred API since API 26; getInstance
    // (deprecated since API 26) still works and is the only viable path when
    // we have no package name (e.g. cppbench running from `adb shell`).
    if (package_name && *package_name) {
        impl_->mgr = ASensorManager_getInstanceForPackage(package_name);
    } else {
        impl_->mgr = ASensorManager_getInstance();
    }
    if (!impl_->mgr) return;

    // ALLOW_NON_CALLBACKS lets us pollOnce without attaching ALooper_addFd
    // callbacks for every event source.
    impl_->looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!impl_->looper) return;

    impl_->queue = ASensorManager_createEventQueue(
        impl_->mgr, impl_->looper, LOOPER_ID, /*callback*/ nullptr, /*data*/ nullptr);
    if (!impl_->queue) return;

    // Build the handle <-> ASensor* map once at construction.
    ASensorList list = nullptr;
    int count = ASensorManager_getSensorList(impl_->mgr, &list);
    if (count > 0 && list) {
        impl_->sensors_by_pos.reserve(static_cast<size_t>(count));
        impl_->handles_by_pos.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (!list[i]) continue;
            impl_->sensors_by_pos.push_back(list[i]);
            impl_->handles_by_pos.push_back(ASensor_getHandle(list[i]));
        }
    }
}

SensorSampler::~SensorSampler() {
    if (!impl_) return;
    if (impl_->queue && impl_->mgr) {
        for (const ASensor* s : impl_->sensors_by_pos) {
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
    const ASensor* s = impl_->find_by_handle(handle);
    if (!s) return false;
    if (ASensorEventQueue_enableSensor(impl_->queue, s) != 0) return false;
    if (ASensorEventQueue_setEventRate(impl_->queue, s, period_us) != 0) return false;
    return true;
}

bool SensorSampler::disable(int handle) {
    if (!valid()) return false;
    const ASensor* s = impl_->find_by_handle(handle);
    if (!s) return false;
    return ASensorEventQueue_disableSensor(impl_->queue, s) == 0;
}

std::vector<Reading> SensorSampler::drain(int timeout_ms) {
    std::vector<Reading> out;
    if (!valid()) return out;

    // ALooper_pollOnce blocks up to timeout_ms; the return ident value would
    // tell us *why* it returned (timeout / our LOOPER_ID / error) but we drain
    // the queue unconditionally on the next line, so the value is discardable.
    // Calling without capture avoids -Wold-style-cast and -Wunused-variable.
    ALooper_pollOnce(timeout_ms, nullptr, nullptr, nullptr);

    ASensorEvent buf[64];
    while (true) {
        int n = ASensorEventQueue_getEvents(impl_->queue, buf, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            Reading r;
            r.handle = buf[i].sensor;
            r.type = buf[i].type;
            r.timestamp_ns = buf[i].timestamp;
            r.value_count = 16;
            std::memcpy(r.values, buf[i].data, sizeof(r.values));
            out.push_back(r);
        }
    }
    return out;
}

} // namespace bench::sensors
