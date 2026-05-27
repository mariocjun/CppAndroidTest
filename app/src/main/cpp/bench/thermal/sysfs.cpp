#include "sysfs.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

namespace bench::thermal {

namespace {

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    // Some sysfs nodes end in \n; getline already strips it. Trim any
    // remaining \r from CR/LF environments (defensive — unlikely on Android).
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

std::vector<ThermalZone> list_thermal_zones() {
    std::vector<ThermalZone> out;
    DIR* d = ::opendir("/sys/class/thermal");
    if (!d) return out;
    while (auto* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        // Parse the integer after "thermal_zone".
        int idx = -1;
        if (std::sscanf(name.c_str(), "thermal_zone%d", &idx) != 1) continue;

        ThermalZone z;
        z.idx = idx;
        z.type = read_first_line("/sys/class/thermal/" + name + "/type");
        z.path_temp = "/sys/class/thermal/" + name + "/temp";
        if (file_exists(z.path_temp)) out.push_back(std::move(z));
    }
    ::closedir(d);

    // Sort by index for deterministic order across reboots.
    std::sort(out.begin(), out.end(),
              [](const ThermalZone& a, const ThermalZone& b) { return a.idx < b.idx; });
    return out;
}

std::vector<int32_t> sample_temps_mc(const std::vector<ThermalZone>& zones) {
    std::vector<int32_t> out;
    out.reserve(zones.size());
    for (const auto& z : zones) {
        std::ifstream f(z.path_temp);
        int32_t v = INT32_MIN;
        if (f) f >> v;
        out.push_back(v);
    }
    return out;
}

std::vector<uint64_t> sample_cpu_freqs_khz(int num_cpus) {
    std::vector<uint64_t> out(static_cast<size_t>(num_cpus), 0);
    for (int i = 0; i < num_cpus; ++i) {
        char path[160];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        std::ifstream f(path);
        if (!f) continue;          // cpu offline / hotplugged
        uint64_t khz = 0;
        f >> khz;
        out[static_cast<size_t>(i)] = khz;
    }
    return out;
}

} // namespace bench::thermal
