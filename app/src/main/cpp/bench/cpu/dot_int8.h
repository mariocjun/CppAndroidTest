#pragma once

#include "../json.h"
#include "../affinity.h"

namespace bench::cpu {

struct DotInt8Config {
    int64_t inner_iters = 1ll << 22;
    int iterations = 7;
    int warmup = 2;
};

// ARMv8.2-A signed dot product (SDOT). Each vdotq_s32 takes 16 int8 from each
// operand, computes 4 four-wide dot products, and accumulates into 4 int32
// lanes. Critical for quantized ML inference (TFLite int8, ONNX QInt8, etc.).
//
// Result JSON:
//   { sdot_gops_per_core, udot_gops_per_core, dotprod_supported, inner_iters }
//
// GOps counts every int8 mul+add as 1 op (so each vdotq = 32 ops). With 8
// independent chains and ARM A76 issuing ~2 SDOT per cycle, expect roughly:
//   A76 Prime @ 2.84 GHz: ~180 GOps/sec
//   A76 Gold @ 2.42 GHz:  ~155 GOps/sec
//   A55 @ 1.8 GHz:        ~58 GOps/sec  (A55 is 1 SDOT/cycle)
//   Mongoose M4:          ~200+ GOps/sec (wider issue)
//
// Gated on ARMv8.2 DotProd. Cortex-A76, Mongoose M4, Cortex-A55 (in Note10+
// configurations) all support it; older A53/A57/A72 do not.
Json run_dot_int8(const DotInt8Config& cfg);

Json run_dot_int8_per_cluster(const DotInt8Config& cfg,
                              const std::vector<CpuCluster>& clusters);

} // namespace bench::cpu
