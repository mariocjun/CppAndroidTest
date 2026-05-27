// STREAM benchmark (McCalpin, 1995). Measures sustained memory bandwidth using
// four kernels. Adapted for ARM64 mobile:
//   - float (4B) elements instead of double; mobile DRAM controllers are
//     bandwidth-limited well before fp64 throughput matters, and using 32-bit
//     halves the array size needed to escape the SLC.
//   - 64-byte-aligned allocations so each cache line is owned by one stream.
//   - best-of-N timing (STREAM-standard) — we want peak sustained, not average.
//   - __restrict__ pointers + -O3 lets clang/gcc autovectorize to NEON.
//
// Bytes/iteration counted per the canonical STREAM rules:
//   copy:  2 * N * sizeof(float)   (read B, write A)
//   scale: 2 * N * sizeof(float)   (read B, write A)
//   add:   3 * N * sizeof(float)   (read B+C, write A)
//   triad: 3 * N * sizeof(float)   (read B+C, write A)
#include "stream.h"

#include "../timer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace bench::cpu {

namespace {

float* aligned_alloc_floats(size_t n, size_t alignment = 64) {
    void* p = nullptr;
    if (posix_memalign(&p, alignment, n * sizeof(float)) != 0) return nullptr;
    return static_cast<float*>(p);
}

// Mark the result so the compiler can't DCE the loop. A volatile sink read of
// one element is enough.
volatile float g_sink;
inline void touch(float* a, size_t n) { g_sink = a[n - 1]; }

inline double bytes_to_gbps(size_t bytes, double secs) {
    return static_cast<double>(bytes) / secs / (1024.0 * 1024.0 * 1024.0);
}

void kernel_copy(float* __restrict__ a, const float* __restrict__ b, size_t n) {
    for (size_t i = 0; i < n; ++i) a[i] = b[i];
}
void kernel_scale(float* __restrict__ a, const float* __restrict__ b, float s, size_t n) {
    for (size_t i = 0; i < n; ++i) a[i] = s * b[i];
}
void kernel_add(float* __restrict__ a, const float* __restrict__ b,
                const float* __restrict__ c, size_t n) {
    for (size_t i = 0; i < n; ++i) a[i] = b[i] + c[i];
}
void kernel_triad(float* __restrict__ a, const float* __restrict__ b,
                  const float* __restrict__ c, float s, size_t n) {
    for (size_t i = 0; i < n; ++i) a[i] = b[i] + s * c[i];
}

template <typename Fn>
double best_of(int iters, int warmup, Fn&& fn) {
    double best = 1e30;
    for (int i = 0; i < warmup; ++i) fn();
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = now_ns();
        fn();
        uint64_t t1 = now_ns();
        double t = static_cast<double>(t1 - t0) * 1e-9;
        best = std::min(best, t);
    }
    return best;
}

} // namespace

Json run_stream(const StreamConfig& cfg) {
    const size_t N = cfg.elems;
    const size_t bytes_per_array = N * sizeof(float);
    float* A = aligned_alloc_floats(N);
    float* B = aligned_alloc_floats(N);
    float* C = aligned_alloc_floats(N);

    if (!A || !B || !C) {
        std::free(A); std::free(B); std::free(C);
        Json err;
        err.kv("error", "allocation_failed").kv("elems", static_cast<int64_t>(N));
        return err;
    }

    // Touch every page to force physical backing and warm TLB before we start.
    const float seed_a = 1.0f, seed_b = 2.0f, seed_c = 0.5f;
    for (size_t i = 0; i < N; ++i) { A[i] = seed_a; B[i] = seed_b; C[i] = seed_c; }

    const float scalar = 3.0f;

    double t_copy = best_of(cfg.iterations, cfg.warmup,
        [&]{ kernel_copy(A, B, N); touch(A, N); });
    double t_scale = best_of(cfg.iterations, cfg.warmup,
        [&]{ kernel_scale(A, B, scalar, N); touch(A, N); });
    double t_add = best_of(cfg.iterations, cfg.warmup,
        [&]{ kernel_add(A, B, C, N); touch(A, N); });
    double t_triad = best_of(cfg.iterations, cfg.warmup,
        [&]{ kernel_triad(A, B, C, scalar, N); touch(A, N); });

    std::free(A); std::free(B); std::free(C);

    Json out;
    out.kv("elems", static_cast<int64_t>(N))
       .kv("bytes_per_array", static_cast<int64_t>(bytes_per_array))
       .kv("iterations", cfg.iterations)
       .kv("copy_gbps",  bytes_to_gbps(2 * bytes_per_array, t_copy))
       .kv("scale_gbps", bytes_to_gbps(2 * bytes_per_array, t_scale))
       .kv("add_gbps",   bytes_to_gbps(3 * bytes_per_array, t_add))
       .kv("triad_gbps", bytes_to_gbps(3 * bytes_per_array, t_triad))
       .kv("copy_sec",  t_copy)
       .kv("scale_sec", t_scale)
       .kv("add_sec",   t_add)
       .kv("triad_sec", t_triad);
    return out;
}

Json run_stream_per_cluster(const StreamConfig& cfg,
                            const std::vector<CpuCluster>& clusters) {
    std::vector<Json> rows;
    rows.reserve(clusters.size());
    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        bool pinned = pin_to_cluster(c);
        Json row = run_stream(cfg);
        Json wrap;
        wrap.kv("cluster_idx", static_cast<int64_t>(i))
            .kv("cpu_min", c.min_id)
            .kv("cpu_max", c.max_id)
            .kv("max_freq_khz", c.max_freq_khz)
            .kv("pinned", pinned)
            .kv("stream", row);
        rows.push_back(std::move(wrap));
    }
    Json out;
    out.kv("per_cluster", rows);
    return out;
}

} // namespace bench::cpu
