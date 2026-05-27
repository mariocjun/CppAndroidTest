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
    // ro.hardware is the SoC family on Samsung Exynos and most MediaTek;
    // ro.board.platform is the codename (msmnile, pineapple, lahaina, ...)
    // on Qualcomm. We match against both to disambiguate.
    //
    // The list below is "known good" — easy to extend; for unknown SoCs we
    // emit a generic "<hardware>/<board>" fallback so the JSON consumer
    // always has *something* to key on.
    struct Entry { const char* hw; const char* board; const char* name; };
    static const Entry table[] = {
        // Samsung Exynos
        {"exynos9825",  "",          "Exynos 9825"},        // Note10+ Exynos (SM-N975F)
        {"exynos2100",  "",          "Exynos 2100"},        // S21 Exynos
        {"exynos2200",  "",          "Exynos 2200"},        // S22 Exynos
        {"s5e9925",     "",          "Exynos 2200"},        // alt hw string
        {"s5e9945",     "",          "Exynos 2400"},        // S24 Exynos
        // Qualcomm Snapdragon (matched by board.platform — Qualcomm's hw is always "qcom")
        {"qcom",        "msmnile",   "Snapdragon 855"},      // Note10+ US (SM-N975U)
        {"qcom",        "kona",      "Snapdragon 865"},      // S20
        {"qcom",        "lahaina",   "Snapdragon 888"},      // S21 US
        {"qcom",        "taro",      "Snapdragon 8 Gen 1"},  // S22 US
        {"qcom",        "kalama",    "Snapdragon 8 Gen 2"},  // S23
        {"qcom",        "pineapple", "Snapdragon 8 Gen 3"},  // S24 — incl. "for Galaxy" variant
        {"qcom",        "sun",       "Snapdragon 8 Elite"},  // S25 (codename "sun")
        // MediaTek Dimensity
        {"mt6983",      "",          "Dimensity 9000"},
        {"mt6985",      "",          "Dimensity 9200"},
        {"mt6989",      "",          "Dimensity 9300"},
        // Google Tensor
        {"gs101",       "",          "Tensor G1"},           // Pixel 6
        {"gs201",       "",          "Tensor G2"},           // Pixel 7
        {"zuma",        "",          "Tensor G3"},           // Pixel 8
        {"zumapro",     "",          "Tensor G4"},           // Pixel 9
    };

    for (const auto& e : table) {
        const bool hw_ok = (e.hw[0] == '\0') || (s.hardware == e.hw);
        const bool board_ok = (e.board[0] == '\0') || (s.board == e.board);
        if (hw_ok && board_ok) {
            // Require at least one of the two to be non-wildcard and match,
            // otherwise we'd accept any device.
            if (e.hw[0] != '\0' && s.hardware == e.hw) return e.name;
            if (e.board[0] != '\0' && s.board == e.board) return e.name;
        }
    }
    if (!s.board.empty()) return s.hardware + "/" + s.board;
    return s.hardware.empty() ? std::string("unknown") : s.hardware;
}

} // namespace bench
