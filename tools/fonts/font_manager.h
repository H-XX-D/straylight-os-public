// tools/fonts/font_manager.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace straylight {

/// Metadata parsed from a TTF/OTF font file's name table.
struct FontInfo {
    std::string family;
    std::string style;          // "Regular", "Bold", "Italic", etc.
    uint16_t weight = 400;      // CSS-style numeric weight
    std::string path;           // Absolute filesystem path
    std::string format;         // "ttf" or "otf"
    std::string version;
    std::string license;
    bool is_variable = false;   // Variable font flag
};

/// A lightweight entry returned by Google Fonts search.
struct GoogleFontEntry {
    std::string family;
    std::string category;       // "sans-serif", "serif", etc.
    std::vector<std::string> variants;
};

/// Font manager: scan, install, remove, preview, search, and configure fonts.
class FontManager {
public:
    /// Scan all system and user font directories, populating the internal list.
    Result<void, SLError> scan();

    /// Return all known fonts, optionally filtered by family name substring.
    std::vector<FontInfo> list(const std::string& family_filter = "") const;

    /// Return detailed info for a single family.
    Result<std::vector<FontInfo>, SLError> info(const std::string& family) const;

    /// Install a font file (local path or "google:<family>") into ~/.local/share/fonts/.
    Result<void, SLError> install(const std::string& source);

    /// Remove all font files for the given family from the user font directory.
    Result<void, SLError> remove(const std::string& family);

    /// Generate a terminal preview of the given family using Unicode block chars.
    Result<std::string, SLError> preview(const std::string& family,
                                         const std::string& text = "The quick brown fox") const;

    /// Search Google Fonts for families matching the query (offline heuristic + API).
    Result<std::vector<GoogleFontEntry>, SLError> search(const std::string& query) const;

    /// Set the default font for a fontconfig category (sans-serif, serif, monospace).
    Result<void, SLError> set_default(const std::string& family,
                                      const std::string& category);

    /// Export the current font list as a JSON string.
    std::string export_json() const;

private:
    /// Scan a single directory for TTF/OTF files.
    void scan_directory(const std::filesystem::path& dir);

    /// Parse the name table from a TTF/OTF file to extract metadata.
    Result<FontInfo, SLError> parse_font_file(const std::filesystem::path& path) const;

    /// Download a font family from Google Fonts into the user font dir.
    Result<void, SLError> install_google_font(const std::string& family);

    /// Install a local font file by copying to the user font dir.
    Result<void, SLError> install_local_file(const std::filesystem::path& path);

    /// Run fc-cache to rebuild the fontconfig cache.
    Result<void, SLError> refresh_cache();

    /// Get the user font directory (~/.local/share/fonts/).
    std::filesystem::path user_font_dir() const;

    /// Read a big-endian uint16 from a buffer.
    static uint16_t read_u16(const uint8_t* data);

    /// Read a big-endian uint32 from a buffer.
    static uint32_t read_u32(const uint8_t* data);

    std::vector<FontInfo> fonts_;
};

} // namespace straylight
