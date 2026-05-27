#include "camera.h"

#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace bench::camera {

namespace {

struct MgrDeleter { void operator()(ACameraManager* p) const { if (p) ACameraManager_delete(p); } };
struct IdsDeleter { void operator()(ACameraIdList* p) const { if (p) ACameraManager_deleteCameraIdList(p); } };
struct MetaDeleter { void operator()(ACameraMetadata* p) const { if (p) ACameraMetadata_free(p); } };

using MgrPtr = std::unique_ptr<ACameraManager, MgrDeleter>;
using IdsPtr = std::unique_ptr<ACameraIdList, IdsDeleter>;
using MetaPtr = std::unique_ptr<ACameraMetadata, MetaDeleter>;

bool get_entry(const ACameraMetadata* meta, uint32_t tag, ACameraMetadata_const_entry* out) {
    return ACameraMetadata_getConstEntry(meta, tag, out) == ACAMERA_OK;
}

void fill_facing(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_LENS_FACING, &e) && e.count >= 1) {
        m.facing = e.data.u8[0];
    }
}

void fill_hw_level(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) && e.count >= 1) {
        m.hw_level = e.data.u8[0];
    }
}

void fill_capabilities(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e)) {
        for (uint32_t i = 0; i < e.count; ++i) {
            m.capabilities.push_back(e.data.u8[i]);
        }
    }
}

void fill_focal_lengths(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e)) {
        for (uint32_t i = 0; i < e.count; ++i) {
            m.focal_lengths_mm.push_back(e.data.f[i]);
        }
    }
}

void fill_sensor_geometry(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &e) && e.count >= 2) {
        m.sensor_physical_w_mm = e.data.f[0];
        m.sensor_physical_h_mm = e.data.f[1];
    }
    if (get_entry(meta, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) && e.count >= 2) {
        m.sensor_pixel_array_w = e.data.i32[0];
        m.sensor_pixel_array_h = e.data.i32[1];
    }
    if (get_entry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) && e.count >= 4) {
        // tag returns (left, top, width, height) — we record width+height only.
        m.sensor_active_array_w = e.data.i32[2];
        m.sensor_active_array_h = e.data.i32[3];
    }
}

void fill_iso_exposure(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) && e.count >= 2) {
        m.iso_min = e.data.i32[0];
        m.iso_max = e.data.i32[1];
    }
    if (get_entry(meta, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) && e.count >= 2) {
        m.exposure_min_ns = e.data.i64[0];
        m.exposure_max_ns = e.data.i64[1];
    }
}

void fill_stream_configs(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e)) {
        // Entries come as int32 quadruples: format, width, height, input (0/1).
        for (uint32_t i = 0; i + 3 < e.count; i += 4) {
            StreamConfig sc;
            sc.format = e.data.i32[i + 0];
            sc.width  = e.data.i32[i + 1];
            sc.height = e.data.i32[i + 2];
            sc.input  = e.data.i32[i + 3] != 0;
            m.stream_configs.push_back(sc);
        }
    }
}

void fill_fps_ranges(const ACameraMetadata* meta, CameraMeta& m) {
    ACameraMetadata_const_entry e;
    if (get_entry(meta, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e)) {
        // pairs of (min, max)
        for (uint32_t i = 0; i + 1 < e.count; i += 2) {
            m.ae_fps_ranges.emplace_back(e.data.i32[i + 0], e.data.i32[i + 1]);
        }
    }
}

} // namespace

std::string format_name(int fmt) {
    switch (fmt) {
        case 0x01: return "RGBA_8888";
        case 0x02: return "RGBX_8888";
        case 0x03: return "RGB_888";
        case 0x04: return "RGB_565";
        case 0x10: return "YCbCr_422_SP_NV16";
        case 0x11: return "YCrCb_420_SP_NV21";
        case 0x14: return "YCbCr_422_I_YUY2";
        case 0x16: return "RAW_PRIVATE";
        case 0x20: return "RAW16";
        case 0x21: return "BLOB_JPEG";
        case 0x22: return "IMPLEMENTATION_DEFINED";
        case 0x23: return "YUV_420_888";
        case 0x24: return "RAW_OPAQUE";
        case 0x25: return "RAW10";
        case 0x26: return "RAW12";
        case 0x27: return "YUV_422_888";
        case 0x28: return "YUV_444_888";
        case 0x29: return "FLEX_RGB_888";
        case 0x2A: return "FLEX_RGBA_8888";
        case 0x100: return "DEPTH16";
        case 0x101: return "DEPTH_POINT_CLOUD";
        case 0x103: return "Y8";
        case 0x108: return "DEPTH_JPEG";
        case 0x109: return "HEIC";
        default: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%x", fmt);
            return buf;
        }
    }
}

std::vector<CameraMeta> enumerate() {
    std::vector<CameraMeta> out;
    MgrPtr mgr(ACameraManager_create());
    if (!mgr) return out;

    ACameraIdList* raw_ids = nullptr;
    if (ACameraManager_getCameraIdList(mgr.get(), &raw_ids) != ACAMERA_OK || !raw_ids) return out;
    IdsPtr ids(raw_ids);

    for (int i = 0; i < ids->numCameras; ++i) {
        const char* cid = ids->cameraIds[i];
        if (!cid) continue;

        ACameraMetadata* raw_meta = nullptr;
        if (ACameraManager_getCameraCharacteristics(mgr.get(), cid, &raw_meta) != ACAMERA_OK
            || !raw_meta) continue;
        MetaPtr meta(raw_meta);

        CameraMeta m;
        m.id = cid;
        fill_facing(meta.get(), m);
        fill_hw_level(meta.get(), m);
        fill_capabilities(meta.get(), m);
        fill_focal_lengths(meta.get(), m);
        fill_sensor_geometry(meta.get(), m);
        fill_iso_exposure(meta.get(), m);
        fill_stream_configs(meta.get(), m);
        fill_fps_ranges(meta.get(), m);
        out.push_back(std::move(m));
    }
    return out;
}

Json enumerate_json() {
    std::vector<Json> rows;
    for (const auto& c : enumerate()) {
        std::vector<double> focals(c.focal_lengths_mm.begin(), c.focal_lengths_mm.end());
        std::vector<double> caps_d(c.capabilities.begin(), c.capabilities.end());

        // Find best (largest) JPEG resolution as a quick "this camera's MP" hint.
        int best_jpeg_w = 0, best_jpeg_h = 0;
        for (const auto& s : c.stream_configs) {
            if (s.input) continue;
            if (s.format == 0x21 && (s.width * s.height) > (best_jpeg_w * best_jpeg_h)) {
                best_jpeg_w = s.width;
                best_jpeg_h = s.height;
            }
        }
        double approx_mp = static_cast<double>(best_jpeg_w) * best_jpeg_h / 1.0e6;

        std::vector<Json> configs;
        for (const auto& s : c.stream_configs) {
            Json sj;
            sj.kv("format", s.format)
              .kv("format_name", format_name(s.format))
              .kv("width", s.width)
              .kv("height", s.height)
              .kv("input", s.input);
            configs.push_back(std::move(sj));
        }

        std::vector<Json> fps;
        for (const auto& p : c.ae_fps_ranges) {
            Json fj;
            fj.kv("min", p.first).kv("max", p.second);
            fps.push_back(std::move(fj));
        }

        Json row;
        row.kv("id", c.id)
           .kv("facing", c.facing)
           .kv("hw_level", c.hw_level)
           .kv("capabilities", caps_d)
           .kv("focal_lengths_mm", focals)
           .kv("sensor_physical_w_mm", static_cast<double>(c.sensor_physical_w_mm))
           .kv("sensor_physical_h_mm", static_cast<double>(c.sensor_physical_h_mm))
           .kv("sensor_pixel_array_w", c.sensor_pixel_array_w)
           .kv("sensor_pixel_array_h", c.sensor_pixel_array_h)
           .kv("sensor_active_array_w", c.sensor_active_array_w)
           .kv("sensor_active_array_h", c.sensor_active_array_h)
           .kv("iso_min", c.iso_min)
           .kv("iso_max", c.iso_max)
           .kv("exposure_min_ns", c.exposure_min_ns)
           .kv("exposure_max_ns", c.exposure_max_ns)
           .kv("approx_megapixels", approx_mp)
           .kv("max_jpeg_w", best_jpeg_w)
           .kv("max_jpeg_h", best_jpeg_h)
           .kv("stream_configs", configs)
           .kv("ae_fps_ranges", fps);
        rows.push_back(std::move(row));
    }
    Json out;
    out.kv("count", static_cast<int64_t>(rows.size()))
       .kv("cameras", rows);
    return out;
}

} // namespace bench::camera
