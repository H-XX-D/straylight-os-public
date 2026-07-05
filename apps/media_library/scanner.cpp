// apps/media_library/scanner.cpp
// Filesystem scanner with MIME detection and GStreamer-based metadata extraction
#include "scanner.h"

#include <straylight/log.h>

// GStreamer
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace straylight::media {

namespace {

inline SLError make_err(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}

// ---------------------------------------------------------------------------
// Magic byte MIME detection
// ---------------------------------------------------------------------------

/// Read up to N bytes from the start of a file into a fixed-size buffer.
template <size_t N>
std::array<uint8_t, N> read_magic(const fs::path& path) {
    std::array<uint8_t, N> buf{};
    std::ifstream f(path, std::ios::binary);
    if (f) f.read(reinterpret_cast<char*>(buf.data()), N);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// MIME detection by magic bytes
// ---------------------------------------------------------------------------

std::string Scanner::detect_mime(const fs::path& path) {
    const auto m = read_magic<16>(path);

    // JPEG
    if (m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) return "image/jpeg";
    // PNG
    if (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') return "image/png";
    // GIF
    if (m[0] == 'G' && m[1] == 'I' && m[2] == 'F') return "image/gif";
    // WebP (RIFF....WEBP)
    if (m[0] == 'R' && m[1] == 'I' && m[2] == 'F' && m[3] == 'F' &&
        m[8] == 'W' && m[9] == 'E' && m[10] == 'B' && m[11] == 'P')
        return "image/webp";
    // AVIF / HEIF (ftyp box: bytes 4-7)
    if (m[4] == 'f' && m[5] == 't' && m[6] == 'y' && m[7] == 'p') {
        if (m[8] == 'a' && m[9] == 'v' && m[10] == 'i' && m[11] == 'f')
            return "image/avif";
        if ((m[8] == 'h' && m[9] == 'e' && m[10] == 'i') ||
            (m[8] == 'h' && m[9] == 'e' && m[10] == 'v'))
            return "image/heif";
        // Generic MP4/M4A/M4V container
        if (m[8] == 'M' && m[9] == '4') {
            if (m[11] == 'A') return "audio/mp4";
            return "video/mp4";
        }
        return "video/mp4"; // generic ftyp box
    }
    // BMP
    if (m[0] == 'B' && m[1] == 'M') return "image/bmp";
    // TIFF (little or big endian)
    if ((m[0] == 'I' && m[1] == 'I' && m[2] == 42 && m[3] == 0) ||
        (m[0] == 'M' && m[1] == 'M' && m[2] == 0  && m[3] == 42))
        return "image/tiff";

    // MP3 (ID3 header or MPEG sync)
    if ((m[0] == 'I' && m[1] == 'D' && m[2] == '3') ||
        (m[0] == 0xFF && (m[1] & 0xE0) == 0xE0))
        return "audio/mpeg";
    // FLAC
    if (m[0] == 'f' && m[1] == 'L' && m[2] == 'a' && m[3] == 'C') return "audio/flac";
    // OGG Vorbis / Opus / Theora
    if (m[0] == 'O' && m[1] == 'g' && m[2] == 'g' && m[3] == 'S') return "audio/ogg";
    // WAV / RIFF PCM
    if (m[0] == 'R' && m[1] == 'I' && m[2] == 'F' && m[3] == 'F' &&
        m[8] == 'W' && m[9] == 'A' && m[10] == 'V' && m[11] == 'E')
        return "audio/wav";
    // AIFF
    if (m[0] == 'F' && m[1] == 'O' && m[2] == 'R' && m[3] == 'M' &&
        m[8] == 'A' && m[9] == 'I' && m[10] == 'F')
        return "audio/aiff";

    // Matroska / WebM
    if (m[0] == 0x1A && m[1] == 0x45 && m[2] == 0xDF && m[3] == 0xA3)
        return "video/x-matroska";
    // AVI (RIFF....AVI )
    if (m[0] == 'R' && m[1] == 'I' && m[2] == 'F' && m[3] == 'F' &&
        m[8] == 'A' && m[9] == 'V' && m[10] == 'I' && m[11] == ' ')
        return "video/x-msvideo";

    // Extension-based fallback
    const std::string ext = path.extension().string();
    if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
    if (ext == ".mkv")                  return "video/x-matroska";
    if (ext == ".webm")                 return "video/webm";
    if (ext == ".avi")                  return "video/x-msvideo";
    if (ext == ".mov")                  return "video/quicktime";
    if (ext == ".m4a")                  return "audio/mp4";
    if (ext == ".opus")                 return "audio/opus";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png")                  return "image/png";
    if (ext == ".gif")                  return "image/gif";

    return "application/octet-stream";
}

MediaType Scanner::mime_to_type(const std::string& mime) {
    if (mime.starts_with("image/")) return MediaType::Image;
    if (mime.starts_with("audio/")) return MediaType::Audio;
    if (mime.starts_with("video/")) return MediaType::Video;
    return MediaType::Unknown;
}

// ---------------------------------------------------------------------------
// GStreamer metadata extraction
// ---------------------------------------------------------------------------

void Scanner::extract_audio_meta(MediaEntry& e) {
    // Use GStreamer Discoverer for duration + tags
    GError* gerr = nullptr;
    GstDiscoverer* disc = gst_discoverer_new(5 * GST_SECOND, &gerr);
    if (!disc) {
        if (gerr) g_error_free(gerr);
        return;
    }

    std::string uri = "file://" + e.path.string();
    GstDiscovererInfo* info = gst_discoverer_discover_uri(disc, uri.c_str(), &gerr);
    if (info) {
        GstClockTime dur = gst_discoverer_info_get_duration(info);
        if (dur != GST_CLOCK_TIME_NONE)
            e.duration_ms = GST_TIME_AS_MSECONDS(dur);

        const GstTagList* tags = gst_discoverer_info_get_tags(info);
        if (tags) {
            gchar* str = nullptr;
            if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &str)) {
                e.title = str; g_free(str); str = nullptr;
            }
            if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &str)) {
                e.artist = str; g_free(str); str = nullptr;
            }
            if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &str)) {
                e.album = str; g_free(str); str = nullptr;
            }
            if (gst_tag_list_get_string(tags, GST_TAG_GENRE, &str)) {
                e.genre = str; g_free(str); str = nullptr;
            }
            guint track = 0;
            if (gst_tag_list_get_uint(tags, GST_TAG_TRACK_NUMBER, &track))
                e.track = static_cast<int>(track);
            GDate* date = nullptr;
            if (gst_tag_list_get_date(tags, GST_TAG_DATE, &date) && date) {
                e.year = static_cast<int>(g_date_get_year(date));
                g_date_free(date);
            }
        }
        gst_discoverer_info_unref(info);
    }
    if (gerr) g_error_free(gerr);
    g_object_unref(disc);
}

void Scanner::extract_video_meta(MediaEntry& e) {
    GError* gerr = nullptr;
    GstDiscoverer* disc = gst_discoverer_new(10 * GST_SECOND, &gerr);
    if (!disc) {
        if (gerr) g_error_free(gerr);
        return;
    }

    std::string uri = "file://" + e.path.string();
    GstDiscovererInfo* info = gst_discoverer_discover_uri(disc, uri.c_str(), &gerr);
    if (info) {
        GstClockTime dur = gst_discoverer_info_get_duration(info);
        if (dur != GST_CLOCK_TIME_NONE)
            e.duration_ms = GST_TIME_AS_MSECONDS(dur);

        const GList* streams = gst_discoverer_info_get_video_streams(info);
        if (streams) {
            auto* vs = static_cast<GstDiscovererVideoInfo*>(streams->data);
            e.width  = gst_discoverer_video_info_get_width(vs);
            e.height = gst_discoverer_video_info_get_height(vs);
        }
        const GstTagList* tags = gst_discoverer_info_get_tags(info);
        if (tags) {
            gchar* str = nullptr;
            if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &str)) {
                e.title = str; g_free(str);
            }
        }
        gst_discoverer_info_unref(info);
    }
    if (gerr) g_error_free(gerr);
    g_object_unref(disc);
}

// ---------------------------------------------------------------------------
// Image dimension extraction (no full decode — header parsing only)
// ---------------------------------------------------------------------------

void Scanner::extract_image_meta(MediaEntry& e) {
    // We read up to 32 bytes of header to determine dimensions cheaply.
    const auto m = read_magic<32>(e.path);

    if (e.mime == "image/jpeg") {
        // Walk JPEG markers to find SOF0/SOF2 (0xFFC0/0xFFC2)
        std::ifstream f(e.path, std::ios::binary);
        if (!f) return;
        uint8_t b0 = 0, b1 = 0;
        f.read(reinterpret_cast<char*>(&b0), 1);
        f.read(reinterpret_cast<char*>(&b1), 1);
        if (b0 != 0xFF || b1 != 0xD8) return;
        while (f) {
            f.read(reinterpret_cast<char*>(&b0), 1);
            if (b0 != 0xFF) break;
            f.read(reinterpret_cast<char*>(&b1), 1);
            uint8_t hi = 0, lo = 0;
            f.read(reinterpret_cast<char*>(&hi), 1);
            f.read(reinterpret_cast<char*>(&lo), 1);
            uint16_t seg_len = (uint16_t(hi) << 8) | lo;
            if (b1 == 0xC0 || b1 == 0xC2) {
                // Precision (1) + height (2) + width (2)
                uint8_t prec = 0;
                f.read(reinterpret_cast<char*>(&prec), 1);
                uint8_t hh = 0, hl = 0, wh = 0, wl = 0;
                f.read(reinterpret_cast<char*>(&hh), 1);
                f.read(reinterpret_cast<char*>(&hl), 1);
                f.read(reinterpret_cast<char*>(&wh), 1);
                f.read(reinterpret_cast<char*>(&wl), 1);
                e.height = (uint32_t(hh) << 8) | hl;
                e.width  = (uint32_t(wh) << 8) | wl;
                return;
            }
            if (seg_len > 2)
                f.seekg(seg_len - 2, std::ios::cur);
        }
    } else if (e.mime == "image/png") {
        // Bytes 16-19 = width, 20-23 = height (big-endian)
        if (m.size() >= 24) {
            e.width  = (uint32_t(m[16]) << 24) | (uint32_t(m[17]) << 16) |
                       (uint32_t(m[18]) <<  8) |  uint32_t(m[19]);
            e.height = (uint32_t(m[20]) << 24) | (uint32_t(m[21]) << 16) |
                       (uint32_t(m[22]) <<  8) |  uint32_t(m[23]);
        }
    } else if (e.mime == "image/gif") {
        // Bytes 6-7 = width, 8-9 = height (little-endian)
        if (m.size() >= 10) {
            e.width  = uint32_t(m[6]) | (uint32_t(m[7]) << 8);
            e.height = uint32_t(m[8]) | (uint32_t(m[9]) << 8);
        }
    } else if (e.mime == "image/bmp") {
        // DIB header: bytes 18-21 = width, 22-25 = height (little-endian, signed)
        if (m.size() >= 26) {
            e.width  = uint32_t(m[18]) | (uint32_t(m[19]) << 8) |
                       (uint32_t(m[20]) << 16) | (uint32_t(m[21]) << 24);
            int32_t h = int32_t(uint32_t(m[22]) | (uint32_t(m[23]) << 8) |
                                (uint32_t(m[24]) << 16) | (uint32_t(m[25]) << 24));
            e.height = static_cast<uint32_t>(h < 0 ? -h : h);
        }
    }
    // WebP, TIFF, AVIF fall back to 0x0 — a full decode would be needed.
}

// ---------------------------------------------------------------------------
// probe() — classify and extract metadata for one file
// ---------------------------------------------------------------------------

Result<MediaEntry, SLError> Scanner::probe(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
        return Result<MediaEntry, SLError>::error(
            make_err(SLErrorCode::NotFound, "Not a regular file: " + path.string()));
    }

    MediaEntry e;
    e.path      = path;
    e.file_size = fs::file_size(path, ec);
    if (ec) e.file_size = 0;

    auto fwt = fs::last_write_time(path, ec);
    if (!ec) {
        // Convert file_time_type to system_clock::time_point
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            fwt - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        e.mtime = sctp;
    }

    e.mime = detect_mime(path);
    e.type = mime_to_type(e.mime);

    if (e.file_size > max_file_bytes) {
        // Don't attempt expensive metadata on huge files
        return Result<MediaEntry, SLError>::ok(std::move(e));
    }

    switch (e.type) {
        case MediaType::Image: extract_image_meta(e); break;
        case MediaType::Audio: extract_audio_meta(e); break;
        case MediaType::Video: extract_video_meta(e); break;
        default: break;
    }

    return Result<MediaEntry, SLError>::ok(std::move(e));
}

// ---------------------------------------------------------------------------
// scan() — recursive directory walk
// ---------------------------------------------------------------------------

Result<size_t, SLError> Scanner::scan(const fs::path& root, ScanCallback cb) {
    if (!fs::exists(root)) {
        return Result<size_t, SLError>::error(
            make_err(SLErrorCode::NotFound, "Directory not found: " + root.string()));
    }

    size_t count = 0;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(root,
                       fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) { ec.clear(); continue; }

        const fs::path& p = it->path();
        auto res = probe(p);
        if (!res.has_value()) continue;
        MediaEntry e = std::move(res.value());
        if (e.type == MediaType::Unknown) continue;

        ++count;
        if (cb) cb(std::move(e));
    }

    return Result<size_t, SLError>::ok(count);
}

} // namespace straylight::media
