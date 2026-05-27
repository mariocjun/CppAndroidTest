cmake_minimum_required(VERSION 3.22.1)
project(cppandroidtest LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# Shared bench sources — compiled into both the APK's .so (for in-process use)
# and the standalone cppbench ELF executable.
# ----------------------------------------------------------------------------
set(BENCH_SOURCES
    bench/affinity.cpp
    bench/soc_info.cpp
    bench/thermal/sysfs.cpp
    bench/cpu/stream.cpp
    bench/cpu/latency.cpp
    bench/cpu/neon_fma.cpp
    bench/cpu/dot_int8.cpp
    bench/cpu/sustained.cpp)

# dot_int8.cpp uses ARMv8.2-A SDOT/UDOT intrinsics. We compile ONLY this TU
# with +dotprod (rather than the whole library) because:
#   - On Note10+ Exynos 9825, the mid cluster is Cortex-A75 which does NOT
#     support DotProd. If autovectorization in other TUs emitted DOTPROD
#     instructions, they would SIGILL when run on those cores.
#   - The dot_int8 measurement functions runtime-gate on /proc/cpuinfo's
#     'asimddp' feature, so they're never invoked on a non-DotProd core.
# x86_64 / armv7 builds get no flag; the file falls back to its stub path.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set_source_files_properties(bench/cpu/dot_int8.cpp PROPERTIES
        COMPILE_OPTIONS "-march=armv8.2-a+dotprod")
endif()

# ----------------------------------------------------------------------------
# native_app_glue (NDK-shipped Activity bridge for NativeActivity apps)
# ----------------------------------------------------------------------------
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Force-keep the entry symbol or the dynamic linker can't find android_main.
set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

# ----------------------------------------------------------------------------
# libcppandroidtest.so — loaded by android.app.NativeActivity (see Manifest's
# android.app.lib_name meta-data, which must match the library basename).
# ----------------------------------------------------------------------------
add_library(cppandroidtest SHARED main.cpp ${BENCH_SOURCES})
target_link_libraries(cppandroidtest native_app_glue android log)
target_compile_options(cppandroidtest PRIVATE
    -O3 -ffast-math -fno-math-errno
    -Wall -Wextra -Wno-unused-parameter)

# ----------------------------------------------------------------------------
# cppbench — standalone ARM64 ELF executable for bench runs via adb shell.
# Build via scripts/build-bench.sh (drives CMake with the NDK toolchain and
# -DBUILD_BENCH_EXECUTABLE=ON). The Gradle/AGP path leaves this OFF because
# AGP doesn't package executables into the APK.
# ----------------------------------------------------------------------------
option(BUILD_BENCH_EXECUTABLE "Build the standalone cppbench ELF" OFF)
if(BUILD_BENCH_EXECUTABLE)
    add_executable(cppbench bench/bench_main.cpp ${BENCH_SOURCES})
    target_compile_options(cppbench PRIVATE
        -O3 -ffast-math -fno-math-errno
        -Wall -Wextra -Wno-unused-parameter
        # No -mfpu/-mcpu — let -march come from the NDK toolchain (arm64-v8a
        # with armv8.2-a baseline on Android 7+ devices).
    )
    target_link_libraries(cppbench log)
    # PIE is the default for armeabi-v7a/arm64-v8a in NDK r19+; no explicit flag needed.
endif()
