cmake_minimum_required(VERSION 3.22.1)
project(cppandroidtest LANGUAGES CXX C)

# C++23 — Clang 17 (NDK r26b) ships partial-but-substantial C++23 support.
# What we get reliably: 'if consteval', 'static operator()', deducing this on
# member functions, 'auto(x)' decay-copy, multidim 'operator[]', 'std::print'
# is partial. What we *don't* get yet: P2996 reflection, contracts, pattern
# matching (none of those landed in C++26 either, mostly C++29 work now).
# If the standard library hits an unimplemented feature we fall back to a
# polyfill in source.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# WARNING POLICY — this project is a template-grade reference for low-level
# perf work; dead code is a bug magnet, so we surface every unused-* warning,
# every implicit-conversion-loss warning, and harden the truly dangerous ones
# into errors. Keep this tight; users forking the repo benefit from the noise.
# ----------------------------------------------------------------------------
set(STRICT_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wcast-align            # int* p = (int*)char_ptr;  // forbidden
    -Wcast-qual             # const T* -> T* without const_cast
    -Wconversion            # implicit lossy numeric conversion
    -Wsign-conversion       # signed <-> unsigned implicit
    -Wdouble-promotion      # float silently promoted to double
    -Wnull-dereference
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wshadow                # variable shadows outer scope
    -Wstrict-aliasing=2
    -Wundef                 # #if FOO with FOO undefined -> warning
    -Wunused                # umbrella for the family below
    -Wunused-parameter      # was suppressed; now visible
    -Wunused-function       # static helpers nobody calls
    -Wunused-variable
    -Wunused-but-set-variable
    -Wunused-value
    -Wunused-result         # ignored [[nodiscard]] return
    -Wmissing-declarations  # global w/o prior declaration
    -Wmissing-noreturn
    -Wno-error=conversion       # too many false positives in NDK headers
    -Wno-error=sign-conversion
    -Wno-error=shadow
    -Werror=return-type         # forgot a return -> UB; must fail build
    -Werror=switch              # incomplete switch on enum -> bug
    -Werror=uninitialized
    -Werror=implicit-fallthrough
    -Werror=null-dereference
)

# Heavy optimisation. -flto=thin gives most of full-LTO's benefit at a
# fraction of the link time, and Android lld supports it. -ffast-math is
# safe here because no benchmark depends on NaN/Inf semantics; we measure
# throughput, not correctness of edge cases. The -fdata-sections /
# -ffunction-sections pair + --gc-sections in the linker strips anything
# not actually referenced — important for the .so shipped in the APK.
set(HEAVY_OPT
    -O3
    -ffast-math
    -fno-math-errno
    -fno-trapping-math
    -fomit-frame-pointer
    -funroll-loops
    -fvectorize
    -fslp-vectorize
    -fstrict-aliasing
    -fdata-sections
    -ffunction-sections
    # Intentionally NOT setting -fvisibility=hidden: the NativeActivity
    # entry point (ANativeActivity_onCreate, in native_app_glue) and
    # android_main are both required to be exported from the .so. The
    # NDK's native_app_glue marks ANativeActivity_onCreate visibility-
    # default, but hiding everything else would require explicit
    # __attribute__((visibility("default"))) on android_main too — for
    # now we keep symbols default-visible and rely on --gc-sections to
    # strip the unused ones at link time.
    -flto=thin                   # link-time optimisation
)

set(STRICT_LINKER_FLAGS
    -Wl,--gc-sections            # drop unused sections after LTO
    -Wl,--as-needed              # drop unused -lfoo entries
    -Wl,-z,relro                 # immutable GOT after dynamic linking
    -Wl,-z,now                   # eager binding (defence in depth)
    -flto=thin                   # linker also needs the flag
)

# Convenience function: apply our compile + link flags to every target we own.
function(harden_target tgt)
    target_compile_options(${tgt} PRIVATE ${HEAVY_OPT} ${STRICT_WARNINGS})
    target_link_options(${tgt} PRIVATE ${STRICT_LINKER_FLAGS})
endfunction()

# ----------------------------------------------------------------------------
# Shared bench sources — compiled into both the APK's .so and the standalone
# cppbench ELF.
# ----------------------------------------------------------------------------
set(BENCH_SOURCES
    bench/affinity.cpp
    bench/soc_info.cpp
    bench/thermal/sysfs.cpp
    bench/cpu/stream.cpp
    bench/cpu/latency.cpp
    bench/cpu/neon_fma.cpp
    bench/cpu/dot_int8.cpp
    bench/cpu/i8mm.cpp
    bench/cpu/sve2.cpp
    bench/cpu/sustained.cpp
    bench/cpu/perf_counters.cpp
    bench/sensors/sensors.cpp
    bench/camera/camera.cpp
    bench/stream/streamer.cpp)

set(BENCH_LIBS android camera2ndk log)

# dot_int8.cpp uses ARMv8.2-A SDOT/UDOT intrinsics. We compile ONLY this TU
# with +dotprod (rather than the whole library) because:
#   - On Note10+ Exynos 9825, the mid cluster is Cortex-A75 which does NOT
#     support DotProd. If autovectorization in other TUs emitted DOTPROD
#     instructions, they would SIGILL when run on those cores.
#   - The dot_int8 measurement functions runtime-gate on /proc/cpuinfo's
#     'asimddp' feature, so they're never invoked on a non-DotProd core.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    # dot_int8: SDOT/UDOT need ARMv8.2-A + DotProd (Cortex-A55 / A75 / M4 /
    # Kryo 485 baseline). Note10+ has it on all clusters except A75 (Exynos
    # 9825 mid cluster).
    set_source_files_properties(bench/cpu/dot_int8.cpp PROPERTIES
        COMPILE_OPTIONS "-march=armv8.2-a+dotprod")

    # i8mm: SMMLA/UMMLA/USMMLA — ARMv8.6-A extension. Cortex-X4 / A720 / A520
    # (S24 Ultra) all support it. Note10+ generation does NOT. The runtime
    # gate (cpuinfo 'i8mm') prevents execution on unsupported cores.
    set_source_files_properties(bench/cpu/i8mm.cpp PROPERTIES
        COMPILE_OPTIONS "-march=armv8.6-a+i8mm")

    # sve2: Scalable Vector Extension 2 — ARMv9-A baseline. S24 Ultra (8 Gen 3)
    # has 128-bit SVE2. Note10+ has no SVE at all.
    set_source_files_properties(bench/cpu/sve2.cpp PROPERTIES
        COMPILE_OPTIONS "-march=armv9-a+sve2")
endif()

# ----------------------------------------------------------------------------
# native_app_glue
# ----------------------------------------------------------------------------
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)
# Don't harden the NDK-shipped glue — it's not our code and won't pass our
# strict warning set. Just optimise it.
target_compile_options(native_app_glue PRIVATE -O3 -fdata-sections -ffunction-sections)

# Force-keep ANativeActivity_onCreate or the dynamic linker can't find android_main.
set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

# ----------------------------------------------------------------------------
# libcppandroidtest.so — loaded by android.app.NativeActivity.
# ----------------------------------------------------------------------------
add_library(cppandroidtest SHARED main.cpp ${BENCH_SOURCES})
target_link_libraries(cppandroidtest native_app_glue ${BENCH_LIBS})
harden_target(cppandroidtest)

# ----------------------------------------------------------------------------
# cppbench — standalone ARM64 ELF executable.
# ----------------------------------------------------------------------------
option(BUILD_BENCH_EXECUTABLE "Build the standalone cppbench ELF" OFF)
if(BUILD_BENCH_EXECUTABLE)
    add_executable(cppbench bench/bench_main.cpp ${BENCH_SOURCES})
    target_link_libraries(cppbench ${BENCH_LIBS})
    harden_target(cppbench)
endif()
