// apps/media_library/thumbnailer.cpp
// Thumbnail generation: images via stb_image / stb_image_resize,
// video frames via GStreamer appsink pipeline.
#include "thumbnailer.h"

#include <straylight/log.h>

// stb_image (single-header, implementation defined here)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// GStreamer
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

// SHA-256 via openssl for deterministic cache keys
#include <openssl/sha.h>

namespace straylight::media {

namespace fs = std::filesystem;

namespace {

inline SLError make_err(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}

/// Hex-encode a SHA-256 digest.
std::string sha256_hex(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[SHA256_DIGEST_LENGTH * 2] = '\0';
    return hex;
}

} // namespace

// ---------------------------------------------------------------------------
// Cache paths
// ---------------------------------------------------------------------------

fs::path Thumbnailer::cache_dir() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".cache" / "straylight" / "media-thumbs"
                         : fs::path("/tmp/straylight-thumbs");
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

fs::path Thumbnailer::cache_path(const MediaEntry& e) {
    return cache_dir() / (sha256_hex(e.path.string()) + ".png");
}

// ---------------------------------------------------------------------------
// PNG cache I/O
// ---------------------------------------------------------------------------

Result<Thumbnail, SLError> Thumbnailer::load_cached(const fs::path& cache_file) {
    int w = 0, h = 0, comp = 0;
    uint8_t* data = stbi_load(cache_file.c_str(), &w, &h, &comp, 4);
    if (!data) {
        return Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::NotFound, "Cache load failed: " + cache_file.string()));
    }
    Thumbnail t;
    t.width  = static_cast<uint32_t>(w);
    t.height = static_cast<uint32_t>(h);
    t.rgba.assign(data, data + size_t(w) * size_t(h) * 4);
    stbi_image_free(data);
    return Result<Thumbnail, SLError>::ok(std::move(t));
}

Result<void, SLError> Thumbnailer::save_cached(const fs::path& cache_file,
                                                const Thumbnail& t) {
    int rc = stbi_write_png(cache_file.c_str(),
                             static_cast<int>(t.width),
                             static_cast<int>(t.height),
                             4, t.rgba.data(),
                             static_cast<int>(t.width) * 4);
    if (!rc) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "PNG write failed: " + cache_file.string()));
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Image thumbnail via stb_image + stb_image_resize2
// ---------------------------------------------------------------------------

Result<Thumbnail, SLError> Thumbnailer::thumbnail_image(const MediaEntry& e) {
    int w = 0, h = 0, comp = 0;
    uint8_t* data = stbi_load(e.path.c_str(), &w, &h, &comp, 4);
    if (!data) {
        return Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::IOError,
                     std::string("stbi_load: ") + stbi_failure_reason()));
    }

    // Calculate scaled dimensions preserving aspect ratio
    uint32_t out_w = thumb_size;
    uint32_t out_h = thumb_size;
    if (w > 0 && h > 0) {
        if (w >= h) {
            out_w = thumb_size;
            out_h = std::max(1u, uint32_t(uint64_t(thumb_size) * h / w));
        } else {
            out_h = thumb_size;
            out_w = std::max(1u, uint32_t(uint64_t(thumb_size) * w / h));
        }
    }

    Thumbnail t;
    t.width  = out_w;
    t.height = out_h;
    t.rgba.resize(size_t(out_w) * out_h * 4);

    stbir_resize_uint8_srgb(data, w, h, w * 4,
                             t.rgba.data(), static_cast<int>(out_w),
                             static_cast<int>(out_h), static_cast<int>(out_w) * 4,
                             STBIR_RGBA);
    stbi_image_free(data);
    return Result<Thumbnail, SLError>::ok(std::move(t));
}

// ---------------------------------------------------------------------------
// Video thumbnail via GStreamer (seek to 10% duration, grab one frame)
// ---------------------------------------------------------------------------

Result<Thumbnail, SLError> Thumbnailer::thumbnail_video(const MediaEntry& e) {
    // Build a playbin3/uridecodebin + videoconvert + appsink pipeline to
    // extract a single RGBA frame at ~10% of the video duration.

    std::string uri = "file://" + e.path.string();

    // Pipeline string: decode video, convert to RGBA, emit one sample
    std::string pipe_str =
        "uridecodebin uri=\"" + uri + "\" ! "
        "videoconvert ! "
        "video/x-raw,format=RGBA ! "
        "appsink name=sink max-buffers=1 drop=true emit-signals=false";

    GError* gerr = nullptr;
    GstElement* pipeline = gst_parse_launch(pipe_str.c_str(), &gerr);
    if (!pipeline) {
        std::string msg = gerr ? gerr->message : "gst_parse_launch failed";
        if (gerr) g_error_free(gerr);
        return Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::Internal, "GStreamer pipeline: " + msg));
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_element_set_state(pipeline, GST_STATE_PAUSED);

    // Wait for PAUSED state so we can seek
    GstStateChangeReturn sr = gst_element_get_state(pipeline, nullptr, nullptr,
                                                     10 * GST_SECOND);
    if (sr == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::Internal, "Pipeline failed to reach PAUSED"));
    }

    // Query duration and seek to ~10%
    gint64 duration = 0;
    if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration) &&
        duration > 0) {
        gint64 seek_pos = duration / 10;
        gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                                static_cast<GstSeekFlags>(
                                    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                seek_pos);
        gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
    }

    // Pull one sample from appsink
    GstSample* sample = gst_app_sink_pull_preroll(GST_APP_SINK(sink));
    if (!sample) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::NotFound, "No video frame available"));
    }

    GstCaps* caps = gst_sample_get_caps(sample);
    GstStructure* st = gst_caps_get_structure(caps, 0);
    int frame_w = 0, frame_h = 0;
    gst_structure_get_int(st, "width",  &frame_w);
    gst_structure_get_int(st, "height", &frame_h);

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_READ);

    // Copy raw RGBA frame data
    std::vector<uint8_t> raw_rgba(map.data, map.data + map.size);
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (frame_w <= 0 || frame_h <= 0) {
        return Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::Internal, "Invalid frame dimensions"));
    }

    // Scale to thumb_size
    uint32_t out_w = thumb_size;
    uint32_t out_h = thumb_size;
    if (frame_w >= frame_h) {
        out_h = std::max(1u, uint32_t(uint64_t(thumb_size) * frame_h / frame_w));
    } else {
        out_w = std::max(1u, uint32_t(uint64_t(thumb_size) * frame_w / frame_h));
    }

    Thumbnail t;
    t.width  = out_w;
    t.height = out_h;
    t.rgba.resize(size_t(out_w) * out_h * 4);

    stbir_resize_uint8_srgb(raw_rgba.data(), frame_w, frame_h, frame_w * 4,
                             t.rgba.data(), static_cast<int>(out_w),
                             static_cast<int>(out_h), static_cast<int>(out_w) * 4,
                             STBIR_RGBA);
    return Result<Thumbnail, SLError>::ok(std::move(t));
}

// ---------------------------------------------------------------------------
// Public get() — check cache, generate if needed, persist
// ---------------------------------------------------------------------------

Result<Thumbnail, SLError> Thumbnailer::get(const MediaEntry& e) {
    const fs::path cp = cache_path(e);

    // Return cached thumbnail if it's newer than the media file
    std::error_code ec;
    if (fs::exists(cp, ec)) {
        auto cache_mtime = fs::last_write_time(cp, ec);
        auto media_mtime = fs::last_write_time(e.path, ec);
        if (!ec && cache_mtime >= media_mtime) {
            auto cached = load_cached(cp);
            if (cached.has_value()) return cached;
            // Cache corrupted — fall through to regenerate
        }
    }

    Result<Thumbnail, SLError> result =
        Result<Thumbnail, SLError>::error(
            make_err(SLErrorCode::Internal, "unsupported media type for thumbnail"));

    switch (e.type) {
        case MediaType::Image:
            result = thumbnail_image(e);
            break;
        case MediaType::Video:
            result = thumbnail_video(e);
            break;
        default:
            break;
    }

    if (result.has_value()) {
        (void)save_cached(cp, result.value());
    }
    return result;
}

void Thumbnailer::evict(const MediaEntry& e) {
    std::error_code ec;
    fs::remove(cache_path(e), ec);
}

} // namespace straylight::media
