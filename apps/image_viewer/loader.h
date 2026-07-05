// apps/image_viewer/loader.h
// Image loading via stb_image: PNG/JPEG/WebP/BMP/GIF/TGA.
// Uploads decoded pixels to an OpenGL ES texture and handles EXIF orientation.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <GLES3/gl3.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace straylight::viewer {

/// Metadata and GPU handle for a loaded image.
struct ImageAsset {
    GLuint   texture_id = 0;    ///< OpenGL texture name (0 = invalid)
    uint32_t width      = 0;    ///< Pixel width after orientation correction
    uint32_t height     = 0;    ///< Pixel height after orientation correction
    int      channels   = 0;    ///< Channels in the source file (1-4)
    int      exif_orient= 1;    ///< EXIF orientation tag value (1 = normal)
    std::filesystem::path path; ///< Source path

    bool valid() const { return texture_id != 0 && width > 0 && height > 0; }
    float aspect() const {
        return (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    }
};

/// Loads an image from disk and uploads it as an RGBA GL texture.
/// Handles EXIF orientation by rotating/flipping the pixel buffer before upload.
class ImageLoader {
public:
    /// Supported extensions (lowercase).
    static bool is_supported(const std::filesystem::path& path);

    /// Decode `path` and upload to a new GL texture.
    /// The caller must call unload() or destroy the context when done.
    static Result<ImageAsset, SLError> load(const std::filesystem::path& path);

    /// Delete the GL texture associated with an asset.
    static void unload(ImageAsset& asset);

private:
    /// Read EXIF orientation from a JPEG file header (no full decode).
    static int read_exif_orientation(const std::filesystem::path& path);

    /// Apply EXIF orientation transform in-place on an RGBA buffer.
    static void apply_orientation(std::vector<uint8_t>& pixels,
                                  uint32_t& width, uint32_t& height,
                                  int orientation);
};

} // namespace straylight::viewer
