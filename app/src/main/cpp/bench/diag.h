// Lightweight diagnostic state — populated by the registry dispatch loop
// and consumed by jni.cpp's fatal signal handler. We only need a stable
// pointer to a C string, written/read by different threads but with single-
// writer semantics (the dispatcher) and signal-safety from the reader side.
// std::atomic on a pointer-sized type is wait-free and async-signal-safe.
#pragma once

#include <atomic>

namespace bench::diag {

// Current bench name (or "(idle)" / "(starting)"). Updated by the registry
// dispatch loop before invoking each wrapper. The fatal signal handler in
// jni.cpp reads this to identify which bench triggered the signal.
extern std::atomic<const char*> current_bench;

inline void set_current(const char* name) {
    current_bench.store(name, std::memory_order_relaxed);
}

inline const char* get_current() {
    return current_bench.load(std::memory_order_relaxed);
}

} // namespace bench::diag
