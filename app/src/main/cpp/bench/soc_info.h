#pragma once

#include <string>
#include <vector>

namespace bench {

struct SocInfo {
    std::string hardware;       // ro.hardware, e.g. "exynos9825" / "qcom"
    std::string board;          // ro.board.platform
    std::string brand;          // ro.product.brand
    std::string model;          // ro.product.model, e.g. "SM-N975F"
    std::string android_release;// ro.build.version.release
    int android_sdk = 0;        // ro.build.version.sdk
    std::string cpu_implementer;// from /proc/cpuinfo (e.g. "ARM", "Qualcomm")
    std::string cpu_part;       // raw part id, varies per cluster
    std::vector<std::string> features; // /proc/cpuinfo "Features" line tokens
    uint64_t mem_total_kb = 0;  // /proc/meminfo MemTotal
    int page_size = 0;
};

SocInfo collect_soc_info();

// Heuristic identification: "Exynos 9825", "Snapdragon 855", or fallback hardware string.
std::string identify_soc(const SocInfo& s);

} // namespace bench
