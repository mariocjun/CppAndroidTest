// Lightweight sysfs readers for thermal zones and per-cpu frequency scaling.
// All paths are kernel-standard on Android, no permissions needed (the sysfs
// nodes are world-readable). Cached lookups so the per-sample cost is one
// open()/read()/close() per zone — fast enough to sample at 10 Hz on a phone.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench::thermal {

struct ThermalZone {
    int idx;                 // /sys/class/thermal/thermal_zoneN
    std::string type;        // contents of .../type — e.g. "cpu-0-0", "battery"
    std::string path_temp;   // cached full path to .../temp
};

// Enumerate all available thermal zones once at startup. Some Android devices
// expose 10-30 zones; we record all of them.
std::vector<ThermalZone> list_thermal_zones();

// Read the current temperature for every zone. Output indices match the zones
// vector. Returns milicelsius (kernel's native unit). On read failure, the
// corresponding entry is INT32_MIN.
std::vector<int32_t> sample_temps_mc(const std::vector<ThermalZone>& zones);

// Per-cpu current frequency in kHz. Index = cpu id. On read failure (offline
// cpu, hotplug), the entry is 0.
std::vector<uint64_t> sample_cpu_freqs_khz(int num_cpus);

} // namespace bench::thermal
