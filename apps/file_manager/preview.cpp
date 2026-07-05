// apps/file_manager/preview.cpp
// File preview implementation — text, image (stb_image), directory info
#include "preview.h"

#include <imgui.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

// stb_image for thumbnail loading
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include <stb_image.h>

#include <pwd.h>
#include <sys/stat.h>

namespace straylight::file_manager {

static const std::vector<std::string> kTextExtensions = {
    ".txt", ".md", ".markdown", ".rst", ".log", ".csv", ".tsv",
    ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
    ".xml", ".html", ".htm", ".css", ".js", ".ts", ".jsx", ".tsx",
    ".py", ".rb", ".rs", ".go", ".java", ".kt", ".scala",
    ".c", ".cpp", ".cxx", ".cc", ".h", ".hpp", ".hxx",
    ".sh", ".bash", ".zsh", ".fish",
    ".cmake", ".make", ".makefile",
    ".gitignore", ".gitattributes", ".editorconfig",
    ".env", ".dockerfile",
};

static const std::vector<std::string> kImageExtensions = {
    ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".ico",
    ".tga", ".ppm", ".pgm",
};

Preview::Preview() = default;

bool Preview::is_text_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    for (const auto& te : kTextExtensions) {
        if (ext == te) return true;
    }

    // Check if filename itself is a known text file
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (name == "makefile" || name == "dockerfile" || name == "cmakelists.txt" ||
        name == "readme" || name == "license" || name == "changelog" ||
        name == "authors" || name == "todo" || name == "news") {
        return true;
    }

    return false;
}

bool Preview::is_image_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    for (const auto& ie : kImageExtensions) {
        if (ext == ie) return true;
    }
    return false;
}

void Preview::generate(const fs::path& path) {
    clear();

    data_.path = path;
    data_.filename = path.filename().string();

    std::error_code ec;
    auto status = fs::status(path, ec);
    if (ec) {
        data_.type = PreviewType::Error;
        data_.error_message = "Cannot access: " + ec.message();
        return;
    }

    // File metadata
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        data_.file_size = static_cast<uintmax_t>(st.st_size);
        struct passwd* pw = getpwuid(st.st_uid);
        if (pw) data_.owner = pw->pw_name;

        // Modified time
        struct tm tm_buf {};
        localtime_r(&st.st_mtime, &tm_buf);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        data_.modified_time = buf;
    }

    // Permission string
    auto perms = status.permissions();
    std::string ps(9, '-');
    if ((perms & fs::perms::owner_read) != fs::perms::none)    ps[0] = 'r';
    if ((perms & fs::perms::owner_write) != fs::perms::none)   ps[1] = 'w';
    if ((perms & fs::perms::owner_exec) != fs::perms::none)    ps[2] = 'x';
    if ((perms & fs::perms::group_read) != fs::perms::none)    ps[3] = 'r';
    if ((perms & fs::perms::group_write) != fs::perms::none)   ps[4] = 'w';
    if ((perms & fs::perms::group_exec) != fs::perms::none)    ps[5] = 'x';
    if ((perms & fs::perms::others_read) != fs::perms::none)   ps[6] = 'r';
    if ((perms & fs::perms::others_write) != fs::perms::none)  ps[7] = 'w';
    if ((perms & fs::perms::others_exec) != fs::perms::none)   ps[8] = 'x';
    data_.permissions = ps;

    if (fs::is_directory(status)) {
        generate_directory_preview(path);
    } else if (is_image_file(path)) {
        generate_image_preview(path);
    } else if (is_text_file(path)) {
        generate_text_preview(path);
    } else {
        generate_binary_preview(path);
    }
}

void Preview::clear() {
    free_image_texture();
    data_ = PreviewData{};
}

void Preview::generate_text_preview(const fs::path& path) {
    data_.type = PreviewType::Text;

    std::ifstream file(path);
    if (!file.is_open()) {
        data_.type = PreviewType::Error;
        data_.error_message = "Cannot open file for reading";
        return;
    }

    std::string line;
    int line_count = 0;
    while (std::getline(file, line) && line_count < kMaxPreviewLines) {
        // Truncate very long lines
        if (line.size() > 500) {
            line.resize(500);
            line += "...";
        }
        // Replace tabs with spaces
        size_t pos = 0;
        while ((pos = line.find('\t', pos)) != std::string::npos) {
            line.replace(pos, 1, "    ");
            pos += 4;
        }
        data_.text_lines.push_back(std::move(line));
        line_count++;
    }

    // Determine mime type from extension
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" || ext == ".c" ||
        ext == ".h" || ext == ".hpp") {
        data_.mime_type = "text/x-c++";
    } else if (ext == ".py") {
        data_.mime_type = "text/x-python";
    } else if (ext == ".json") {
        data_.mime_type = "application/json";
    } else if (ext == ".xml" || ext == ".html") {
        data_.mime_type = "text/html";
    } else if (ext == ".sh" || ext == ".bash") {
        data_.mime_type = "text/x-shellscript";
    } else {
        data_.mime_type = "text/plain";
    }
}

void Preview::generate_image_preview(const fs::path& path) {
    data_.type = PreviewType::Image;

    int w, h, channels;
    uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        data_.type = PreviewType::Error;
        data_.error_message = std::string("Failed to load image: ") +
                              stbi_failure_reason();
        return;
    }

    data_.image.width = w;
    data_.image.height = h;
    data_.image.pixels.assign(pixels, pixels + w * h * 4);
    stbi_image_free(pixels);

    // Scale down for preview if too large
    constexpr int kMaxPreviewDim = 512;
    if (w > kMaxPreviewDim || h > kMaxPreviewDim) {
        float scale = std::min(
            static_cast<float>(kMaxPreviewDim) / w,
            static_cast<float>(kMaxPreviewDim) / h);
        int new_w = static_cast<int>(w * scale);
        int new_h = static_cast<int>(h * scale);

        // Simple bilinear downscale
        std::vector<uint8_t> scaled(static_cast<size_t>(new_w * new_h * 4));
        for (int sy = 0; sy < new_h; ++sy) {
            for (int sx = 0; sx < new_w; ++sx) {
                float src_x = sx / scale;
                float src_y = sy / scale;
                int ix = std::clamp(static_cast<int>(src_x), 0, w - 1);
                int iy = std::clamp(static_cast<int>(src_y), 0, h - 1);
                size_t src_idx = static_cast<size_t>((iy * w + ix) * 4);
                size_t dst_idx = static_cast<size_t>((sy * new_w + sx) * 4);
                scaled[dst_idx + 0] = data_.image.pixels[src_idx + 0];
                scaled[dst_idx + 1] = data_.image.pixels[src_idx + 1];
                scaled[dst_idx + 2] = data_.image.pixels[src_idx + 2];
                scaled[dst_idx + 3] = data_.image.pixels[src_idx + 3];
            }
        }

        data_.image.pixels = std::move(scaled);
        data_.image.width = new_w;
        data_.image.height = new_h;
    }

    load_image_texture();
}

void Preview::generate_directory_preview(const fs::path& path) {
    data_.type = PreviewType::Directory;

    std::error_code ec;
    data_.item_count = 0;
    data_.dir_count = 0;
    data_.file_count = 0;
    data_.total_size = 0;

    for (const auto& entry : fs::directory_iterator(path, ec)) {
        data_.item_count++;
        if (entry.is_directory(ec)) {
            data_.dir_count++;
        } else {
            data_.file_count++;
            data_.total_size += entry.file_size(ec);
        }
    }
}

void Preview::generate_binary_preview(const fs::path& path) {
    data_.type = PreviewType::Binary;

    // Detect if it's actually a text file by reading first bytes
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        data_.type = PreviewType::Error;
        data_.error_message = "Cannot open file";
        return;
    }

    // Read first 512 bytes
    char buf[512];
    file.read(buf, sizeof(buf));
    auto bytes_read = file.gcount();

    // Check for null bytes (binary indicator)
    bool has_null = false;
    int printable = 0;
    for (std::streamsize i = 0; i < bytes_read; ++i) {
        uint8_t b = static_cast<uint8_t>(buf[i]);
        if (b == 0) {
            has_null = true;
            break;
        }
        if (std::isprint(b) || b == '\n' || b == '\r' || b == '\t') {
            printable++;
        }
    }

    if (!has_null && bytes_read > 0 &&
        printable > static_cast<int>(bytes_read * 0.8)) {
        // Actually a text file, generate text preview instead
        file.seekg(0);
        data_.type = PreviewType::Text;
        std::string line;
        int line_count = 0;
        while (std::getline(file, line) && line_count < kMaxPreviewLines) {
            if (line.size() > 500) {
                line.resize(500);
                line += "...";
            }
            data_.text_lines.push_back(std::move(line));
            line_count++;
        }
        data_.mime_type = "text/plain";
    }

    // For true binary files, show hex dump of first few bytes
    if (data_.type == PreviewType::Binary && bytes_read > 0) {
        std::ostringstream ss;
        for (std::streamsize i = 0; i < std::min(bytes_read, std::streamsize(256)); i += 16) {
            ss << std::hex << std::setw(8) << std::setfill('0') << i << "  ";
            for (std::streamsize j = 0; j < 16 && (i + j) < bytes_read; ++j) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<uint8_t>(buf[i + j])) << " ";
            }
            ss << " |";
            for (std::streamsize j = 0; j < 16 && (i + j) < bytes_read; ++j) {
                char c = buf[i + j];
                ss << (std::isprint(static_cast<unsigned char>(c)) ? c : '.');
            }
            ss << "|";
            data_.text_lines.push_back(ss.str());
            ss.str("");
            ss.clear();
        }
    }
}

void Preview::load_image_texture() {
    if (data_.image.pixels.empty()) return;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 data_.image.width, data_.image.height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 data_.image.pixels.data());

    data_.image.texture_id = tex;
    data_.image.loaded = true;
}

void Preview::free_image_texture() {
    if (data_.image.texture_id != 0) {
        GLuint tex = data_.image.texture_id;
        glDeleteTextures(1, &tex);
        data_.image.texture_id = 0;
        data_.image.loaded = false;
    }
}

static std::string format_size(uintmax_t bytes) {
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    double s = static_cast<double>(bytes);
    if (s < KB) {
        ss << bytes << " B";
    } else if (s < MB) {
        ss << (s / KB) << " KB";
    } else if (s < GB) {
        ss << (s / MB) << " MB";
    } else {
        ss << (s / GB) << " GB";
    }
    return ss.str();
}

void Preview::render(float width, float height) {
    (void)width;
    (void)height;

    if (data_.type == PreviewType::None) {
        ImGui::TextDisabled("No file selected");
        return;
    }

    // File name and metadata header
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s",
                       data_.filename.c_str());
    ImGui::Separator();

    if (!data_.modified_time.empty()) {
        ImGui::Text("Modified: %s", data_.modified_time.c_str());
    }
    if (!data_.owner.empty()) {
        ImGui::Text("Owner: %s", data_.owner.c_str());
    }
    if (!data_.permissions.empty()) {
        ImGui::Text("Permissions: %s", data_.permissions.c_str());
    }

    ImGui::Spacing();

    switch (data_.type) {
    case PreviewType::Text:
        ImGui::Text("Type: %s", data_.mime_type.c_str());
        ImGui::Text("Size: %s", format_size(data_.file_size).c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("TextPreview", ImVec2(0, 0), true)) {
            for (const auto& line : data_.text_lines) {
                ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
            }
            if (static_cast<int>(data_.text_lines.size()) >= kMaxPreviewLines) {
                ImGui::TextDisabled("... (truncated at %d lines)", kMaxPreviewLines);
            }
        }
        ImGui::EndChild();
        break;

    case PreviewType::Image:
        ImGui::Text("Dimensions: %dx%d", data_.image.width, data_.image.height);
        ImGui::Text("Size: %s", format_size(data_.file_size).c_str());
        ImGui::Separator();

        if (data_.image.loaded && data_.image.texture_id != 0) {
            float aspect = static_cast<float>(data_.image.width) /
                           static_cast<float>(data_.image.height);
            float preview_w = width - 20.0f;
            float preview_h = preview_w / aspect;
            if (preview_h > height - 120.0f) {
                preview_h = height - 120.0f;
                preview_w = preview_h * aspect;
            }
            ImGui::Image(
                reinterpret_cast<ImTextureID>(
                    static_cast<uintptr_t>(data_.image.texture_id)),
                ImVec2(preview_w, preview_h));
        }
        break;

    case PreviewType::Directory:
        ImGui::Text("Items: %d  (%d dirs, %d files)",
                    data_.item_count, data_.dir_count, data_.file_count);
        ImGui::Text("Total size: %s", format_size(data_.total_size).c_str());
        break;

    case PreviewType::Binary:
        ImGui::Text("Type: Binary file");
        ImGui::Text("Size: %s", format_size(data_.file_size).c_str());
        ImGui::Separator();

        if (!data_.text_lines.empty()) {
            ImGui::TextDisabled("Hex dump:");
            if (ImGui::BeginChild("HexPreview", ImVec2(0, 0), true)) {
                for (const auto& line : data_.text_lines) {
                    ImGui::TextUnformatted(line.c_str(),
                                           line.c_str() + line.size());
                }
            }
            ImGui::EndChild();
        }
        break;

    case PreviewType::Error:
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                          "Error: %s", data_.error_message.c_str());
        break;

    case PreviewType::None:
        break;
    }
}

} // namespace straylight::file_manager
