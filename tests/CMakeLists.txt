cmake_minimum_required(VERSION 3.22.1)
project(cppbench_tests LANGUAGES CXX)

# Host build (Linux/Mac/Windows). We do NOT use the NDK toolchain here: the
# tests cover the platform-agnostic pieces of the bench harness — the JSON
# emitter, the Benchmark concept on the registry tuple — which compile
# against the host's standard library without Android headers.
#
# Android-specific code (sensors, camera, sysfs readers, NEON intrinsics) is
# integration-tested by the smoke workflow running the APK on an emulator and
# by the standalone cppbench arm64 build in CI; not unit-tested here.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings on tests too — host build doesn't have to wrestle with NDK
# header noise so we can be tighter than the Android target.
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Wconversion -Wsign-conversion
    -Wdouble-promotion -Wformat=2
    -Wno-error=conversion -Wno-error=sign-conversion
    -Werror=return-type
)

enable_testing()

# doctest — single-header test framework, fetched at configure time so we
# don't have to vendor a 200KB blob into the repo. v2.4.11 is current stable.
include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# Helper: each test_*.cpp becomes its own ctest entry.
function(add_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)
    target_link_libraries(${name} PRIVATE doctest::doctest)
    add_test(NAME ${name} COMMAND ${name} --reporters=console --no-version)
endfunction()

add_unit_test(test_json)
add_unit_test(test_registry)
