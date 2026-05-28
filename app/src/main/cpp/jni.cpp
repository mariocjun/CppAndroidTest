// JNI bridge — exposes the bench harness to the Kotlin MainActivity. Each
// function returns a JSON string that the caller renders in the UI and
// also persists to the external-files directory.
//
// Hardening (added after a field-reported crash on S24 Ultra):
//   1. Every native function wraps its body in try/catch(std::exception);
//      on exception we return a structured error JSON instead of letting
//      it propagate to the JVM (which would kill the app).
//   2. JNI_OnLoad installs signal handlers (SIGSEGV/SIGABRT/SIGBUS/SIGILL/
//      SIGFPE) that write a one-shot crash dump to the app's internal
//      files dir, then re-raise so the OS still gets a proper tombstone.
//      MainActivity checks for the dump on next startup and offers to
//      upload it.

#include "bench/affinity.h"
#include "bench/diag.h"
#include "bench/hwcaps.h"
#include "bench/json.h"
#include "bench/registry.h"
#include "bench/soc_info.h"
#include "bench/timer.h"
#include "bench/sensors/sensors.h"
#include "bench/camera/camera.h"

#include <android/log.h>
#include <jni.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/ucontext.h>
#include <unistd.h>

#define LOG_TAG "CppAndroidTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Path where the native crash dump is written. Filled in by JNI_OnLoad once
// the activity hands us its internal files dir on first benchmark call.
// Until then it stays empty and the signal handler is a no-op writer.
char g_crash_dump_path[512] = {0};

bench::Json build_env_info() {
    bench::Json env;
    auto soc = bench::collect_soc_info();
    env.kv("soc_identified", bench::identify_soc(soc))
       .kv("ro_hardware", soc.hardware)
       .kv("ro_board_platform", soc.board)
       .kv("ro_product_brand", soc.brand)
       .kv("ro_product_model", soc.model)
       .kv("android_release", soc.android_release)
       .kv("android_sdk", soc.android_sdk)
       .kv("cpu_implementer", soc.cpu_implementer)
       .kv("cpu_features", soc.features)
       .kv("mem_total_kb", soc.mem_total_kb)
       .kv("page_size", soc.page_size);

    std::vector<bench::Json> clusters_json;
    for (const auto& c : bench::detect_clusters()) {
        bench::Json cj;
        cj.kv("cpu_min", c.min_id)
          .kv("cpu_max", c.max_id)
          .kv("max_freq_khz", c.max_freq_khz);
        clusters_json.push_back(std::move(cj));
    }
    env.kv("cpu_clusters", clusters_json);
    env.kv("num_cpus", bench::num_cpus());
    env.kv("timestamp_ns", bench::now_ns());
    return env;
}

std::string jstring_to_std(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    if (!c) return {};
    std::string out(c);
    env->ReleaseStringUTFChars(js, c);
    return out;
}

void maybe_persist(const std::string& dir, const std::string& filename, const std::string& content) {
    if (dir.empty()) return;
    const std::string path = dir + "/" + filename;
    std::ofstream f(path);
    if (f) {
        f << content;
        LOGI("Persisted %zu bytes to %s", content.size(), path.c_str());
    } else {
        LOGE("WARN: could not open %s for writing", path.c_str());
    }
}

// Build a JSON error envelope that callers return when an exception escapes
// the bench code. Same shape every time so the Kotlin side can detect it.
std::string make_error_json(const char* where, const char* what) {
    bench::Json j;
    j.kv("error", true).kv("where", where).kv("what", what ? what : "(no message)");
    return j.str();
}

// -- Native crash handler ---------------------------------------------------
//
// SA_SIGINFO + 3-arg handler gives us siginfo_t (faulting address) and
// ucontext_t (full register state at the moment of the trap). On arm64 the
// program counter at the time of SIGILL is at ucontext->uc_mcontext.pc;
// dumping that PLUS the libcppandroidtest.so base from /proc/self/maps lets
// us turn the absolute PC into an offset we can resolve back to a symbol
// offline via `llvm-addr2line -e libcppandroidtest.so <offset>`.
//
// Async-signal-safe constraints: no malloc, no iostreams, no locale, no
// fprintf. We use open/write/close (POSIX async-signal-safe per POSIX.1) and
// snprintf into a stack buffer (glibc/bionic's snprintf is signal-safe in
// practice; the alternative `dprintf` is too).

const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        default:      return "?";
    }
}

// Read libcppandroidtest.so's load base from /proc/self/maps. Best-effort —
// returns 0 on any failure. async-signal-safe (open/read/close only).
uintptr_t find_so_base() {
    int fd = ::open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n;
    uintptr_t base = 0;
    // Scan the first chunk for our library name. /proc/self/maps lines are
    // typically <= 200 bytes and the .so usually appears in the first ~16 KB.
    while ((n = ::read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        const char* needle = "libcppandroidtest.so";
        const char* p = std::strstr(buf, needle);
        if (p) {
            // Walk back to the start of the line, then parse the hex address.
            while (p > buf && p[-1] != '\n') --p;
            uintptr_t v = 0;
            for (; *p && *p != '-'; ++p) {
                char c = *p;
                uintptr_t d;
                if (c >= '0' && c <= '9') d = static_cast<uintptr_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<uintptr_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<uintptr_t>(c - 'A' + 10);
                else break;
                v = (v << 4) | d;
            }
            base = v;
            break;
        }
        if (n < static_cast<ssize_t>(sizeof(buf) - 1)) break;
    }
    ::close(fd);
    return base;
}

[[noreturn]] void on_fatal_signal(int sig, siginfo_t* info, void* ucontext_v) {
    if (g_crash_dump_path[0]) {
        int fd = ::open(g_crash_dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            // Extract PC. On arm64 ucontext_t::uc_mcontext is a sigcontext;
            // its .pc field is the saved program counter.
            uintptr_t pc = 0;
            uintptr_t lr = 0;
            if (ucontext_v) {
                auto* uc = static_cast<ucontext_t*>(ucontext_v);
#if defined(__aarch64__)
                pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
                // x30 is the link register on arm64. uc_mcontext.regs[30].
                lr = static_cast<uintptr_t>(uc->uc_mcontext.regs[30]);
#else
                (void)uc;
#endif
            }
            uintptr_t fault_addr = info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
            uintptr_t so_base = find_so_base();
            uintptr_t pc_offset = (so_base && pc > so_base) ? (pc - so_base) : 0;

            const char* bench_name = bench::diag::get_current();
            if (!bench_name) bench_name = "?";

            char buf[768];
            int n = std::snprintf(buf, sizeof(buf),
                "{\"crash\":true,"
                "\"signal\":%d,"
                "\"name\":\"%s\","
                "\"unix_time\":%lld,"
                "\"current_bench\":\"%s\","
                "\"pc\":\"0x%lx\","
                "\"lr\":\"0x%lx\","
                "\"faulting_addr\":\"0x%lx\","
                "\"so_base\":\"0x%lx\","
                "\"pc_offset\":\"0x%lx\","
                "\"si_code\":%d}\n",
                sig, sig_name(sig),
                static_cast<long long>(std::time(nullptr)),
                bench_name,
                static_cast<unsigned long>(pc),
                static_cast<unsigned long>(lr),
                static_cast<unsigned long>(fault_addr),
                static_cast<unsigned long>(so_base),
                static_cast<unsigned long>(pc_offset),
                info ? info->si_code : 0);
            if (n > 0) {
                ssize_t written = ::write(fd, buf, static_cast<size_t>(n));
                (void)written;
            }
            ::close(fd);
        }
    }
    // Restore default handler and re-raise so the OS still produces a
    // tombstone in /data/tombstones (visible via 'adb bugreport').
    std::signal(sig, SIG_DFL);
    std::raise(sig);
    _exit(128 + sig);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_sigaction = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (int sig : {SIGSEGV, SIGABRT, SIGILL, SIGBUS, SIGFPE}) {
        sigaction(sig, &sa, nullptr);
    }
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    install_signal_handlers();
    LOGI("JNI_OnLoad: libcppandroidtest.so ready for benchmarks");
    return JNI_VERSION_1_6;
}

// Lets MainActivity tell us where to drop crash dumps. Called once on
// app startup with getFilesDir() (the app's internal-data dir, always
// writable, always exists).
JNIEXPORT void JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeSetCrashDir(
    JNIEnv* env, jobject /*this*/, jstring jInternalDir) {
    const std::string d = jstring_to_std(env, jInternalDir);
    if (d.empty()) return;
    const std::string p = d + "/last-native-crash.json";
    std::strncpy(g_crash_dump_path, p.c_str(), sizeof(g_crash_dump_path) - 1);
    g_crash_dump_path[sizeof(g_crash_dump_path) - 1] = '\0';
    LOGI("Crash dump path set: %s", g_crash_dump_path);
}

JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeRunBenchmarks(
    JNIEnv* env, jobject /*this*/, jstring jExternalDir, jstring jFilter) {
    const std::string out_dir = jstring_to_std(env, jExternalDir);
    const std::string filter  = jstring_to_std(env, jFilter);
    LOGI("nativeRunBenchmarks: start, filter='%s'", filter.c_str());

    try {
        auto clusters = bench::detect_clusters();
        bench::registry::Args args{};
        args.filter = filter;
        auto results = bench::registry::dispatch(args, clusters);

        bench::Json top;
        top.kv("env", build_env_info());
        std::vector<bench::Json> bench_array;
        for (auto& [name, j] : results) {
            bench::Json wrap;
            wrap.kv("name", name).kv("result", j);
            bench_array.push_back(std::move(wrap));
        }
        top.kv("benchmarks", bench_array);

        const auto json = top.str();
        maybe_persist(out_dir, "benchmarks-latest.json", json);
        LOGI("nativeRunBenchmarks: done, %zu bytes of JSON", json.size());
        return env->NewStringUTF(json.c_str());
    } catch (const std::exception& e) {
        LOGE("nativeRunBenchmarks: std::exception: %s", e.what());
        return env->NewStringUTF(
            make_error_json("nativeRunBenchmarks", e.what()).c_str());
    } catch (...) {
        LOGE("nativeRunBenchmarks: unknown exception");
        return env->NewStringUTF(
            make_error_json("nativeRunBenchmarks", "unknown C++ exception").c_str());
    }
}

JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeEnumerateSensors(
    JNIEnv* env, jobject /*this*/) {
    LOGI("nativeEnumerateSensors: start");
    try {
        const auto json = bench::sensors::enumerate_json().str();
        LOGI("nativeEnumerateSensors: done, %zu bytes", json.size());
        return env->NewStringUTF(json.c_str());
    } catch (const std::exception& e) {
        LOGE("nativeEnumerateSensors: %s", e.what());
        return env->NewStringUTF(
            make_error_json("nativeEnumerateSensors", e.what()).c_str());
    } catch (...) {
        return env->NewStringUTF(
            make_error_json("nativeEnumerateSensors", "unknown C++ exception").c_str());
    }
}

JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeHwcaps(
    JNIEnv* env, jobject /*this*/) {
    // Authoritative kernel view of which extensions userspace can use.
    // Surfaced to the UI so the user can see *before* running a bench
    // whether (for instance) SVE2 is going to SIGILL or work.
    try {
        bench::Json j;
        j.kv("hwcap_raw",  static_cast<int64_t>(bench::raw_hwcap()))
         .kv("hwcap2_raw", static_cast<int64_t>(bench::raw_hwcap2()))
         .kv("neon_fp16", bench::has_neon_fp16())
         .kv("dotprod",   bench::has_dotprod())
         .kv("i8mm",      bench::has_i8mm())
         .kv("sve",       bench::has_sve())
         .kv("sve2",      bench::has_sve2())
         .kv("bf16",      bench::has_bf16());
        return env->NewStringUTF(j.str().c_str());
    } catch (const std::exception& e) {
        return env->NewStringUTF(make_error_json("nativeHwcaps", e.what()).c_str());
    }
}

JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeEnumerateCameras(
    JNIEnv* env, jobject /*this*/) {
    LOGI("nativeEnumerateCameras: start");
    try {
        const auto json = bench::camera::enumerate_json().str();
        LOGI("nativeEnumerateCameras: done, %zu bytes", json.size());
        return env->NewStringUTF(json.c_str());
    } catch (const std::exception& e) {
        LOGE("nativeEnumerateCameras: %s", e.what());
        return env->NewStringUTF(
            make_error_json("nativeEnumerateCameras", e.what()).c_str());
    } catch (...) {
        return env->NewStringUTF(
            make_error_json("nativeEnumerateCameras", "unknown C++ exception").c_str());
    }
}

} // extern "C"
