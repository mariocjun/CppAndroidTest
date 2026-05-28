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

// async-signal-safe-ish: avoid malloc / iostreams / locale stuff. We only
// use POSIX write() + snprintf with a fixed-size stack buffer. snprintf is
// "implementation-defined" wrt signal safety but glibc/bionic's is fine in
// practice; the alternative (dprintf) has the same limitation.
[[noreturn]] void on_fatal_signal(int sig) {
    if (g_crash_dump_path[0]) {
        int fd = ::open(g_crash_dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char buf[256];
            const char* name = "?";
            switch (sig) {
                case SIGSEGV: name = "SIGSEGV"; break;
                case SIGABRT: name = "SIGABRT"; break;
                case SIGILL:  name = "SIGILL";  break;
                case SIGBUS:  name = "SIGBUS";  break;
                case SIGFPE:  name = "SIGFPE";  break;
                default: break;
            }
            int n = std::snprintf(buf, sizeof(buf),
                "{\"crash\":true,\"signal\":%d,\"name\":\"%s\",\"unix_time\":%lld}\n",
                sig, name, static_cast<long long>(std::time(nullptr)));
            if (n > 0) {
                ssize_t written = ::write(fd, buf, static_cast<size_t>(n));
                (void)written;  // best-effort, we're about to die anyway
            }
            ::close(fd);
        }
    }
    // Restore default handler and re-raise so the OS still gets a proper
    // tombstone in /data/tombstones.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
    _exit(128 + sig);  // unreachable, but keeps [[noreturn]] honest
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
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
    JNIEnv* env, jobject /*this*/, jstring jExternalDir) {
    LOGI("nativeRunBenchmarks: start");
    const std::string out_dir = jstring_to_std(env, jExternalDir);

    try {
        auto clusters = bench::detect_clusters();
        bench::registry::Args args{};
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
