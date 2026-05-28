#include "hwcaps.h"

#include <sys/auxv.h>

// Kernel ARM64 HWCAP bit definitions. We hardcode them rather than including
// <asm/hwcap.h> because that header isn't always available cross-platform in
// the NDK sysroot. These values are kernel UAPI and stable across versions.
#ifndef HWCAP_FP
#  define HWCAP_FP        (1UL << 0)
#  define HWCAP_ASIMD     (1UL << 1)
#  define HWCAP_EVTSTRM   (1UL << 2)
#  define HWCAP_AES       (1UL << 3)
#  define HWCAP_PMULL     (1UL << 4)
#  define HWCAP_SHA1      (1UL << 5)
#  define HWCAP_SHA2      (1UL << 6)
#  define HWCAP_CRC32     (1UL << 7)
#  define HWCAP_ATOMICS   (1UL << 8)
#  define HWCAP_FPHP      (1UL << 9)
#  define HWCAP_ASIMDHP   (1UL << 10)
#  define HWCAP_CPUID     (1UL << 11)
#  define HWCAP_ASIMDRDM  (1UL << 12)
#  define HWCAP_JSCVT     (1UL << 13)
#  define HWCAP_FCMA      (1UL << 14)
#  define HWCAP_LRCPC     (1UL << 15)
#  define HWCAP_DCPOP     (1UL << 16)
#  define HWCAP_SHA3      (1UL << 17)
#  define HWCAP_SM3       (1UL << 18)
#  define HWCAP_SM4       (1UL << 19)
#  define HWCAP_ASIMDDP   (1UL << 20)
#  define HWCAP_SHA512    (1UL << 21)
#  define HWCAP_SVE       (1UL << 22)
#  define HWCAP_ASIMDFHM  (1UL << 23)
#  define HWCAP_DIT       (1UL << 24)
#  define HWCAP_USCAT     (1UL << 25)
#  define HWCAP_ILRCPC    (1UL << 26)
#  define HWCAP_FLAGM     (1UL << 27)
#  define HWCAP_SSBS      (1UL << 28)
#  define HWCAP_SB        (1UL << 29)
#  define HWCAP_PACA      (1UL << 30)
#  define HWCAP_PACG      (1UL << 31)
#endif

#ifndef HWCAP2_DCPODP
#  define HWCAP2_DCPODP   (1UL << 0)
#  define HWCAP2_SVE2     (1UL << 1)
#  define HWCAP2_SVEAES   (1UL << 2)
#  define HWCAP2_SVEPMULL (1UL << 3)
#  define HWCAP2_SVEBITPERM (1UL << 4)
#  define HWCAP2_SVESHA3  (1UL << 5)
#  define HWCAP2_SVESM4   (1UL << 6)
#  define HWCAP2_FLAGM2   (1UL << 7)
#  define HWCAP2_FRINT    (1UL << 8)
#  define HWCAP2_SVEI8MM  (1UL << 9)
#  define HWCAP2_SVEF32MM (1UL << 10)
#  define HWCAP2_SVEF64MM (1UL << 11)
#  define HWCAP2_SVEBF16  (1UL << 12)
#  define HWCAP2_I8MM     (1UL << 13)
#  define HWCAP2_BF16     (1UL << 14)
#endif

namespace bench {

namespace {
inline bool has_cap(unsigned long flag) {
    return (getauxval(AT_HWCAP) & flag) != 0;
}
inline bool has_cap2(unsigned long flag) {
    return (getauxval(AT_HWCAP2) & flag) != 0;
}
} // namespace

bool has_neon_fp16() { return has_cap(HWCAP_ASIMDHP); }
bool has_dotprod()   { return has_cap(HWCAP_ASIMDDP); }
bool has_i8mm()      { return has_cap2(HWCAP2_I8MM); }
bool has_sve()       { return has_cap(HWCAP_SVE); }
bool has_sve2()      { return has_cap2(HWCAP2_SVE2) && has_cap(HWCAP_SVE); }
bool has_bf16()      { return has_cap2(HWCAP2_BF16); }

unsigned long raw_hwcap()  { return getauxval(AT_HWCAP); }
unsigned long raw_hwcap2() { return getauxval(AT_HWCAP2); }

} // namespace bench
