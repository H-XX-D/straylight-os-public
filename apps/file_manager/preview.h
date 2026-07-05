// apps/file_manager/preview.h
// File preview panel — text, images, directory info
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace straylight::file_manager {

namespace fs = std::filesystem;

/// Type of preview content.
enum class PreviewType {
    None,
    Text,
    Image,
    Directory,
    Binary,
    Error,
};

/// Image data for preview thumbnails.
struct ImagePreview {
    std::vector<uint8_t> pixels; // RGBA
    int width = 0;
    int height = 0;
    unsigned int texture_id = 0; // OpenGL texture handle
    bool loaded = false;
};

/// Preview data for a file.
struct PreviewData {
    PreviewType type = PreviewType::None;
    fs::path path;
    std::string filename;

    // Text preview
    std::vector<std::string> text_lines;
    std::string mime_type;

    // Image preview
    ImagePreview image;

    // Directory preview
    int item_count = 0;
    int dir_count = 0;
    int file_count = 0;
    uintmax_t total_size = 0;

    // File metadata
    uintmax_t file_size = 0;
    std::string permissions;
    std::string owner;
    std::string modified_time;

    // Error
    std::string error_message;
};

/// File preview generator.
class Preview {
public:
    Preview();

    /// Generate preview for the given path.
    void generate(const fs::path& path);

    /// Get the current preview data.
    [[nodiscard]] const PreviewData& data() const { return data_; }

    /// Clear the preview.
    void clear();

    /// Render the preview in ImGui.
    void render(float width, float height);

    /// Check if a file is a text file based on extension.
    static bool is_text_file(const fs::path& path);

    /// Check if a file is an image file based on extension.
    static bool is_image_file(const fs::path& path);

    /// Maximum lines to show for text preview.
    static constexpr int kMaxPreviewLines = 100;

private:
    void generate_text_preview(const fs::path& path);
    void generate_image_preview(const fs::path& path);
    void generate_directory_preview(const fs::path& path);
    void generate_binary_preview(const fs::path& path);

    void load_image_texture();
    void free_image_texture();

    PreviewData data_;
};

} // namespace straylight::file_manager
