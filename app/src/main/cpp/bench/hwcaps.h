// HWCAP-based CPU feature detection — the authoritative answer to
// 'can this process actually execute SVE / i8mm / dotprod / FP16 instructions'
// on the current Android kernel.
//
// /proc/cpuinfo's 'Features' line is what most code checks, but it lies in
// both directions on Android:
//   - It can list features the kernel has DISABLED for userspace (e.g.
//     Samsung One UI ships kernels with SVE userspace gated off on some
//     S24 firmwares, even though the silicon supports SVE2 — calling a
//     SVE intrinsic SIGILLs the process).
//   - On 32-bit reads of AArch64 cores it can omit features the kernel
//     does allow.
//
// getauxval(AT_HWCAP / AT_HWCAP2) goes through the kernel's signal context
// path and reflects exactly the instruction set the kernel will permit
// userspace to execute. This is what every bench's compile-time guard
// should additionally check at runtime.
#pragma once

namespace bench {

bool has_neon_fp16();   // HWCAP_ASIMDHP — vfmaq_f16 etc.
bool has_dotprod();     // HWCAP_ASIMDDP — vdotq_s32 / vdotq_u32
bool has_i8mm();        // HWCAP2_I8MM   — vmmlaq_s32 / vmmlaq_u32 (SMMLA)
bool has_sve();         // HWCAP_SVE     — base Scalable Vector Extension
bool has_sve2();        // HWCAP2_SVE2   — SVE2
bool has_bf16();        // HWCAP2_BF16   — bfloat16 arith

unsigned long raw_hwcap();
unsigned long raw_hwcap2();

} // namespace bench
