// JNI bridge — exposes the bench harness to the Kotlin MainActivity. Each
// function returns a JSON string that the caller renders in the UI and
// also persists to the external-files directory. The library is loaded by
// MainActivity's static initialiser (System.loadLibrary("cppandroidtest"))
// so these symbols are looked up at first JNI call.
//
// Note on linkage: the .so already exports ANativeActivity_onCreate via
// the `-u` linker flag in CMakeLists.txt. The NativeActivity itself is no
// longer used at runtime (Manifest now points at MainActivity), but keeping
// the symbol exported is harmless and avoids needing to refactor the
// CMakeLists's linker flags now.
#include "bench/affinity.h"
#include "bench/json.h"
#include "bench/registry.h"
#include "bench/soc_info.h"
#include "bench/timer.h"
#include "bench/sensors/sensors.h"
#include "bench/camera/camera.h"

#include <android/log.h>
#include <jni.h>
#include <fstream>
#include <string>

#define LOG_TAG "CppAndroidTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

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

// JNI helper: convert nullable jstring -> std::string. Returns empty string
// if the input is null (Kotlin can pass null to indicate "don't persist").
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
        LOGI("WARN: could not open %s for writing", path.c_str());
    }
}

} // namespace

extern "C" {

// JNI_OnLoad is invoked exactly once by the runtime when
// System.loadLibrary("cppandroidtest") completes. We use it as a probe for
// the smoke workflow: if this line appears in logcat, the .so loaded
// successfully and the JNI surface is reachable.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
    LOGI("JNI_OnLoad: libcppandroidtest.so ready for benchmarks");
    return JNI_VERSION_1_6;
}


JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeRunBenchmarks(
    JNIEnv* env, jobject /*this*/, jstring jExternalDir) {
    LOGI("nativeRunBenchmarks: start");
    const std::string out_dir = jstring_to_std(env, jExternalDir);

    auto clusters = bench::detect_clusters();
    bench::registry::Args args{};  // defaults: empty filter, default iters
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
}

JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeEnumerateSensors(
    JNIEnv* env, jobject /*this*/) {
    LOGI("nativeEnumerateSensors: start");
    const auto json = bench::sensors::enumerate_json().str();
    LOGI("nativeEnumerateSensors: done, %zu bytes", json.size());
    return env->NewStringUTF(json.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_example_cppandroidtest_MainActivity_nativeEnumerateCameras(
    JNIEnv* env, jobject /*this*/) {
    LOGI("nativeEnumerateCameras: start");
    const auto json = bench::camera::enumerate_json().str();
    LOGI("nativeEnumerateCameras: done, %zu bytes", json.size());
    return env->NewStringUTF(json.c_str());
}

} // extern "C"
