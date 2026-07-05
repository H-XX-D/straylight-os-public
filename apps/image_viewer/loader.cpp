// apps/image_viewer/loader.cpp
// stb_image-based image loader with EXIF orientation and GL texture upload.
#include "loader.h"

#include <straylight/log.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace straylight::viewer {

namespace {
constexpr std::string_view kSupportedExts[] = {
    ".png", ".jpg", ".jpeg", ".bmp", ".tga",
    ".gif", ".hdr", ".psd", ".pic",
};
} // namespace

bool ImageLoader::is_supported(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& e : kSupportedExts)
        if (ext == e) return true;
    return false;
}

// ---------------------------------------------------------------------------
// EXIF orientation reader (minimal — reads only APP1/EXIF in JPEG)
// ---------------------------------------------------------------------------

int ImageLoader::read_exif_orientation(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 1;

    auto read_u16_be = [&](uint16_t& out) -> bool {
        unsigned char b[2];
        if (std::fread(b, 1, 2, f) != 2) return false;
        out = static_cast<uint16_t>((b[0] << 8) | b[1]);
        return true;
    };
    auto read_u16 = [&](bool big_endian, uint16_t& out) -> bool {
        unsigned char b[2];
        if (std::fread(b, 1, 2, f) != 2) return false;
        out = big_endian ? static_cast<uint16_t>((b[0] << 8) | b[1])
                         : static_cast<uint16_t>((b[1] << 8) | b[0]);
        return true;
    };
    auto read_u32 = [&](bool big_endian, uint32_t& out) -> bool {
        unsigned char b[4];
        if (std::fread(b, 1, 4, f) != 4) return false;
        if (big_endian)
            out = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
                  (uint32_t(b[2]) << 8)  | b[3];
        else
            out = (uint32_t(b[3]) << 24) | (uint32_t(b[2]) << 16) |
                  (uint32_t(b[1]) << 8)  | b[0];
        return true;
    };

    int orientation = 1;

    // Check JPEG SOI marker
    uint16_t marker = 0;
    if (!read_u16_be(marker) || marker != 0xFFD8) { std::fclose(f); return 1; }

    // Scan JPEG segments
    while (true) {
        if (!read_u16_be(marker)) break;
        if ((marker & 0xFF00) != 0xFF00) break;

        uint16_t length = 0;
        if (!read_u16_be(length) || length < 2) break;
        uint32_t data_len = length - 2;

        if (marker == 0xFFE1) { // APP1 — possible EXIF
            char exif_hdr[6] = {};
            if (data_len < 6) { std::fseek(f, static_cast<long>(data_len), SEEK_CUR); continue; }
            if (std::fread(exif_hdr, 1, 6, f) != 6) break;
            data_len -= 6;

            if (std::memcmp(exif_hdr, "Exif\0\0", 6) == 0) {
                long tiff_start = std::ftell(f);
                // TIFF byte order
                char bom[2] = {};
                if (std::fread(bom, 1, 2, f) != 2) break;
                bool big_endian = (bom[0] == 'M');
                // TIFF magic (0x002A)
                uint16_t tiff_magic = 0;
                if (!read_u16(big_endian, tiff_magic) || tiff_magic != 0x002A) break;
                // Offset to first IFD
                uint32_t ifd_offset = 0;
                if (!read_u32(big_endian, ifd_offset)) break;
                std::fseek(f, tiff_start + static_cast<long>(ifd_offset), SEEK_SET);
                // Number of entries
                uint16_t n_entries = 0;
                if (!read_u16(big_endian, n_entries)) break;
                for (uint16_t e = 0; e < n_entries; ++e) {
                    uint16_t tag = 0, type = 0;
                    uint32_t count = 0, value_or_offset = 0;
                    if (!read_u16(big_endian, tag))           break;
                    if (!read_u16(big_endian, type))          break;
                    if (!read_u32(big_endian, count))         break;
                    if (!read_u32(big_endian, value_or_offset)) break;
                    if (tag == 0x0112 && count == 1) {
                        // Orientation: value is in low 16 bits (SHORT)
                        orientation = big_endian
                            ? static_cast<int>(value_or_offset >> 16)
                            : static_cast<int>(value_or_offset & 0xFFFF);
                        break;
                    }
                }
                break;
            } else {
                // Not EXIF, skip remaining data
                std::fseek(f, static_cast<long>(data_len), SEEK_CUR);
            }
        } else {
            std::fseek(f, static_cast<long>(data_len), SEEK_CUR);
        }
    }

    std::fclose(f);
    return orientation;
}

// ---------------------------------------------------------------------------
// apply_orientation — rotate/flip pixel buffer in-place
// EXIF tag meanings:
//   1 = normal, 2 = flip-H, 3 = 180°, 4 = flip-V,
//   5 = transpose, 6 = 90° CW, 7 = transverse, 8 = 90° CCW
// ---------------------------------------------------------------------------

void ImageLoader::apply_orientation(std::vector<uint8_t>& pixels,
                                    uint32_t& width, uint32_t& height,
                                    int orientation) {
    if (orientation <= 1 || orientation > 8) return;

    const uint32_t W = width, H = height;
    std::vector<uint8_t> tmp(pixels.size());

    auto src = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (y * W + x) * 4;
    };
    auto dst = [&](uint32_t x, uint32_t y) -> uint8_t* {
        return tmp.data() + (y * width + x) * 4;
    };

    // Determine output dimensions
    if (orientation >= 5) { width = H; height = W; } // transpose-like

    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint32_t dx = x, dy = y;
            switch (orientation) {
                case 2: dx = W - 1 - x; dy = y;         break; // flip H
                case 3: dx = W - 1 - x; dy = H - 1 - y; break; // 180
                case 4: dx = x;         dy = H - 1 - y;  break; // flip V
                case 5: dx = y;         dy = x;           break; // transpose
                case 6: dx = H - 1 - y; dy = x;           break; // 90 CW
                case 7: dx = y;         dy = W - 1 - x;   break; // transverse
                case 8: dx = y;         dy = W - 1 - x;
                        // remap for CCW
                        dx = y; dy = W - 1 - x;
                        break;
                default: break;
            }
            std::memcpy(dst(dx, dy), src(x, y), 4);
        }
    }
    pixels = std::move(tmp);
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

Result<ImageAsset, SLError> ImageLoader::load(const std::filesystem::path& path) {
    if (!is_supported(path)) {
        return Result<ImageAsset, SLError>::error(
            SLError{SLErrorCode::InvalidArgument,
                    "Unsupported image format: " + path.string()});
    }

    // Read EXIF orientation for JPEGs before decoding
    int orient = 1;
    {
        std::string ext = path.extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".jpg" || ext == ".jpeg") orient = read_exif_orientation(path);
    }

    int w = 0, h = 0, comp = 0;
    uint8_t* data = stbi_load(path.c_str(), &w, &h, &comp, STBI_rgb_alpha);
    if (!data) {
        return Result<ImageAsset, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("stbi_load failed: ") + stbi_failure_reason()});
    }

    std::vector<uint8_t> pixels(data, data + size_t(w) * size_t(h) * 4);
    stbi_image_free(data);

    uint32_t img_w = static_cast<uint32_t>(w);
    uint32_t img_h = static_cast<uint32_t>(h);

    apply_orientation(pixels, img_w, img_h, orient);

    // Upload to GL texture
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(img_w), static_cast<GLsizei>(img_h),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    ImageAsset asset;
    asset.texture_id  = tex;
    asset.width       = img_w;
    asset.height      = img_h;
    asset.channels    = comp;
    asset.exif_orient = orient;
    asset.path        = path;

    return Result<ImageAsset, SLError>::ok(std::move(asset));
}

void ImageLoader::unload(ImageAsset& asset) {
    if (asset.texture_id != 0) {
        glDeleteTextures(1, &asset.texture_id);
        asset.texture_id = 0;
    }
    asset.width = asset.height = 0;
}

} // namespace straylight::viewer
