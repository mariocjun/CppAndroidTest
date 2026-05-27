#include "soc_info.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>

namespace bench {

static std::string getprop(const char* key) {
    char buf[PROP_VALUE_MAX] = {0};
    __system_property_get(key, buf);
    return std::string(buf);
}

static void parse_cpuinfo(SocInfo& s) {
    std::ifstream f("/proc/cpuinfo");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        // trim trailing whitespace from key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::string val = (colon + 2 <= line.size()) ? line.substr(colon + 2) : "";

        if (key == "CPU implementer" && s.cpu_implementer.empty()) s.cpu_implementer = val;
        else if (key == "CPU part" && s.cpu_part.empty()) s.cpu_part = val;
        else if (key == "Features" && s.features.empty()) {
            std::istringstream iss(val);
            std::string tok;
            while (iss >> tok) s.features.push_back(tok);
        }
    }
}

static uint64_t parse_meminfo_total_kb() {
    std::ifstream f("/proc/meminfo");
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 9, "MemTotal:") == 0) {
            uint64_t kb = 0;
            std::sscanf(line.c_str(), "MemTotal: %llu kB",
                        reinterpret_cast<unsigned long long*>(&kb));
            return kb;
        }
    }
    return 0;
}

SocInfo collect_soc_info() {
    SocInfo s;
    s.hardware = getprop("ro.hardware");
    s.board = getprop("ro.board.platform");
    s.brand = getprop("ro.product.brand");
    s.model = getprop("ro.product.model");
    s.android_release = getprop("ro.build.version.release");
    std::string sdk_s = getprop("ro.build.version.sdk");
    if (!sdk_s.empty()) s.android_sdk = std::atoi(sdk_s.c_str());
    parse_cpuinfo(s);
    s.mem_total_kb = parse_meminfo_total_kb();
    s.page_size = static_cast<int>(sysconf(_SC_PAGESIZE));
    return s;
}

std::string identify_soc(const SocInfo& s) {
    // ro.hardware values we expect on Note10+:
    //   "exynos9825" (SM-N975F)
    //   "qcom"       (SM-N975U) — board.platform is "msmnile" for SD855
    if (s.hardware == "exynos9825") return "Exynos 9825";
    if (s.hardware == "qcom" && s.board == "msmnile") return "Snapdragon 855";
    if (!s.board.empty()) return s.hardware + "/" + s.board;
    return s.hardware.empty() ? std::string("unknown") : s.hardware;
}

} // namespace bench
