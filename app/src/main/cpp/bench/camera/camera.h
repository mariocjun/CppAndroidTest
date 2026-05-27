// Enumerate cameras and their characteristics via the NDK Camera2 API
// (ACameraManager + ACameraMetadata). Read-only metadata path — does NOT
// open a capture session or require the CAMERA permission. Works from a
// standalone ELF.
//
// On S24 Ultra typical cameras: 200 MP main, 50 MP periscope (5x), 10 MP
// telephoto (3x), 12 MP ultrawide, 12 MP front. Each appears as a separate
// camera ID with distinct resolution/focal-length/sensor-size sets.
#pragma once

#include "../json.h"

#include <string>
#include <vector>

namespace bench::camera {

struct StreamConfig {
    int format = 0;     // HAL_PIXEL_FORMAT_* (0x21=JPEG, 0x23=YUV_420_888, etc.)
    int width = 0;
    int height = 0;
    bool input = false; // true means this format can be a reprocessing input
};

struct CameraMeta {
    std::string id;                     // "0", "1", or vendor-defined like "10"
    int facing = -1;                    // 0=front, 1=back, 2=external
    int hw_level = -1;                  // 0=limited, 1=full, 2=legacy, 3=level_3, 4=external
    std::vector<int> capabilities;      // ACAMERA_REQUEST_AVAILABLE_CAPABILITIES enum values
    std::vector<float> focal_lengths_mm;
    float sensor_physical_w_mm = 0.0f;
    float sensor_physical_h_mm = 0.0f;
    int sensor_pixel_array_w = 0;
    int sensor_pixel_array_h = 0;
    int sensor_active_array_w = 0;
    int sensor_active_array_h = 0;
    int iso_min = 0;
    int iso_max = 0;
    int64_t exposure_min_ns = 0;
    int64_t exposure_max_ns = 0;
    std::vector<StreamConfig> stream_configs;       // available output configurations
    std::vector<std::pair<int, int>> ae_fps_ranges; // (min_fps, max_fps)
};

std::vector<CameraMeta> enumerate();
Json enumerate_json();

// Human-readable format name for HAL_PIXEL_FORMAT_*. Returns "0x..." for unknowns.
std::string format_name(int hal_pixel_format);

} // namespace bench::camera
