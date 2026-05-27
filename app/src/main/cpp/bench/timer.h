// Monotonic timing helpers. CLOCK_MONOTONIC_RAW because we don't want the kernel
// to slew it during NTP adjustments — benchmarks need an unmodified rate.
#pragma once

#include <cstdint>
#include <time.h>

namespace bench {

inline uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}

inline double now_sec() {
    return static_cast<double>(now_ns()) * 1e-9;
}

// RAII timer: ScopedTimer t(out); ... ; t goes out of scope -> out updated with elapsed seconds.
struct ScopedTimer {
    double* out;
    uint64_t t0;
    explicit ScopedTimer(double* o) : out(o), t0(now_ns()) {}
    ~ScopedTimer() { *out = static_cast<double>(now_ns() - t0) * 1e-9; }
};

} // namespace bench
