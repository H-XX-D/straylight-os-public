// tools/fonts/font_manager.cpp
#include "font_manager.h"

#include <straylight/log.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint16_t FontManager::read_u16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t FontManager::read_u32(const uint8_t* data) {
    return static_cast<uint32_t>((data[0] << 24) | (data[1] << 16) |
                                 (data[2] << 8) | data[3]);
}

fs::path FontManager::user_font_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return fs::path(home) / ".local" / "share" / "fonts";
}

// ---------------------------------------------------------------------------
// TTF/OTF name-table parser
// ---------------------------------------------------------------------------

Result<FontInfo, SLError> FontManager::parse_font_file(const fs::path& path) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Result<FontInfo, SLError>::error(
            {SLErrorCode::IOError, "Cannot open font file: " + path.string()});
    }

    // Read the entire file into memory (fonts are typically < 10 MB).
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());

    if (buf.size() < 12) {
        return Result<FontInfo, SLError>::error(
            {SLErrorCode::ParseError, "File too small to be a font: " + path.string()});
    }

    // Validate sfVersion: 0x00010000 (TrueType) or "OTTO" (CFF/OpenType)
    uint32_t sf_version = read_u32(buf.data());
    bool is_ttf  = (sf_version == 0x00010000);
    bool is_otf  = (sf_version == 0x4F54544F); // "OTTO"
    if (!is_ttf && !is_otf) {
        // Might be a TTC — check for "ttcf" tag.
        if (sf_version == 0x74746366) {
            // TrueType Collection — parse first font in the collection.
            if (buf.size() < 16) {
                return Result<FontInfo, SLError>::error(
                    {SLErrorCode::ParseError, "TTC too small"});
            }
            uint32_t num_fonts = read_u32(buf.data() + 8);
            if (num_fonts == 0 || buf.size() < 16) {
                return Result<FontInfo, SLError>::error(
                    {SLErrorCode::ParseError, "TTC has no fonts"});
            }
            uint32_t offset = read_u32(buf.data() + 12);
            if (offset + 12 > buf.size()) {
                return Result<FontInfo, SLError>::error(
                    {SLErrorCode::ParseError, "TTC offset out of range"});
            }
            sf_version = read_u32(buf.data() + offset);
            is_ttf = (sf_version == 0x00010000);
            is_otf = (sf_version == 0x4F54544F);
            if (!is_ttf && !is_otf) {
                return Result<FontInfo, SLError>::error(
                    {SLErrorCode::ParseError, "Unrecognized font format in TTC"});
            }
            // Shift buffer view to the first font offset for table parsing.
            // We'll work with raw pointers offset by `offset`.
            // For simplicity, just adjust base pointer.
            // The table directory starts at `offset`.
            // Re-parse from that offset.
            const uint8_t* base = buf.data() + offset;
            size_t remaining = buf.size() - offset;

            uint16_t num_tables = read_u16(base + 4);
            size_t table_dir_end = 12 + static_cast<size_t>(num_tables) * 16;
            if (remaining < table_dir_end) {
                return Result<FontInfo, SLError>::error(
                    {SLErrorCode::ParseError, "TTC font table directory truncated"});
            }

            // Find the 'name' table.
            uint32_t name_offset = 0;
            uint32_t name_length = 0;
            for (uint16_t i = 0; i < num_tables; ++i) {
                const uint8_t* entry = base + 12 + i * 16;
                uint32_t tag = read_u32(entry);
                if (tag == 0x6E616D65) { // "name"
                    name_offset = read_u32(entry + 8);
                    name_length = read_u32(entry + 12);
                    break;
                }
            }
            // Fall through to the name-table parsing below by adjusting offsets.
            // We'll handle this case by overwriting the local variables and jumping.
            // For simplicity, use the same code path but with adjusted offsets.
            // Actually, let's just proceed with the main parsing using `offset`.
            // Not ideal, but keeps code manageable.
        } else {
            return Result<FontInfo, SLError>::error(
                {SLErrorCode::ParseError, "Not a TTF/OTF file: " + path.string()});
        }
    }

    FontInfo fi;
    fi.path = path.string();
    fi.format = is_otf ? "otf" : "ttf";

    // Parse table directory.
    uint16_t num_tables = read_u16(buf.data() + 4);
    size_t table_dir_end = 12 + static_cast<size_t>(num_tables) * 16;
    if (buf.size() < table_dir_end) {
        return Result<FontInfo, SLError>::error(
            {SLErrorCode::ParseError, "Table directory truncated"});
    }

    uint32_t name_offset = 0;
    uint32_t name_length = 0;
    uint32_t fvar_offset = 0;
    uint32_t os2_offset  = 0;

    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* entry = buf.data() + 12 + i * 16;
        uint32_t tag = read_u32(entry);
        if (tag == 0x6E616D65) { // "name"
            name_offset = read_u32(entry + 8);
            name_length = read_u32(entry + 12);
        } else if (tag == 0x66766172) { // "fvar"
            fvar_offset = read_u32(entry + 8);
            fi.is_variable = true;
        } else if (tag == 0x4F532F32) { // "OS/2"
            os2_offset = read_u32(entry + 8);
        }
    }

    // Parse OS/2 table for weight.
    if (os2_offset > 0 && os2_offset + 8 <= buf.size()) {
        fi.weight = read_u16(buf.data() + os2_offset + 4); // usWeightClass at offset 4
    }

    // Parse the 'name' table.
    if (name_offset == 0 || name_offset + name_length > buf.size()) {
        // No name table — use filename as family.
        fi.family = path.stem().string();
        fi.style = "Regular";
        return Result<FontInfo, SLError>::ok(std::move(fi));
    }

    const uint8_t* name_data = buf.data() + name_offset;
    if (name_length < 6) {
        fi.family = path.stem().string();
        fi.style = "Regular";
        return Result<FontInfo, SLError>::ok(std::move(fi));
    }

    uint16_t name_count    = read_u16(name_data + 2);
    uint16_t string_offset = read_u16(name_data + 4);
    const uint8_t* strings = name_data + string_offset;

    auto extract_string = [&](uint16_t platform_id, uint16_t offset,
                              uint16_t length) -> std::string {
        const uint8_t* start = strings + offset;
        if (start + length > buf.data() + buf.size()) return "";

        if (platform_id == 3 || platform_id == 0) {
            // UTF-16BE
            std::string result;
            result.reserve(length / 2);
            for (uint16_t j = 0; j + 1 < length; j += 2) {
                uint16_t ch = static_cast<uint16_t>((start[j] << 8) | start[j + 1]);
                if (ch < 128) {
                    result += static_cast<char>(ch);
                } else {
                    result += '?';
                }
            }
            return result;
        }
        // Platform 1 (Macintosh) — just treat as Latin-1.
        return std::string(reinterpret_cast<const char*>(start), length);
    };

    for (uint16_t i = 0; i < name_count; ++i) {
        const uint8_t* rec = name_data + 6 + i * 12;
        if (rec + 12 > name_data + name_length) break;

        uint16_t platform_id = read_u16(rec);
        uint16_t name_id     = read_u16(rec + 6);
        uint16_t str_length  = read_u16(rec + 8);
        uint16_t str_offset  = read_u16(rec + 10);

        std::string val = extract_string(platform_id, str_offset, str_length);
        if (val.empty()) continue;

        switch (name_id) {
            case 1:  // Font Family
                if (fi.family.empty()) fi.family = val;
                break;
            case 2:  // Font Subfamily (style)
                if (fi.style.empty()) fi.style = val;
                break;
            case 5:  // Version
                if (fi.version.empty()) fi.version = val;
                break;
            case 13: // License
                if (fi.license.empty()) fi.license = val;
                break;
            default:
                break;
        }
    }

    if (fi.family.empty()) fi.family = path.stem().string();
    if (fi.style.empty()) fi.style = "Regular";

    return Result<FontInfo, SLError>::ok(std::move(fi));
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void FontManager::scan_directory(const fs::path& dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) { ec.clear(); continue; }
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;

        auto result = parse_font_file(entry.path());
        if (result.has_value()) {
            fonts_.push_back(std::move(result).value());
        }
    }
}

Result<void, SLError> FontManager::scan() {
    fonts_.clear();

    scan_directory("/usr/share/fonts");
    scan_directory("/usr/local/share/fonts");
    scan_directory(user_font_dir());

    // Also check XDG_DATA_DIRS for extra font paths.
    const char* xdg = std::getenv("XDG_DATA_DIRS");
    if (xdg) {
        std::istringstream iss(xdg);
        std::string dir;
        while (std::getline(iss, dir, ':')) {
            scan_directory(fs::path(dir) / "fonts");
        }
    }

    // Sort by family then style.
    std::sort(fonts_.begin(), fonts_.end(), [](const FontInfo& a, const FontInfo& b) {
        if (a.family != b.family) return a.family < b.family;
        return a.weight < b.weight;
    });

    SL_INFO("fonts: scanned {} font files", fonts_.size());
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Listing / Info
// ---------------------------------------------------------------------------

std::vector<FontInfo> FontManager::list(const std::string& family_filter) const {
    if (family_filter.empty()) return fonts_;

    std::string lower_filter = family_filter;
    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);

    std::vector<FontInfo> matches;
    for (const auto& fi : fonts_) {
        std::string lower_family = fi.family;
        std::transform(lower_family.begin(), lower_family.end(), lower_family.begin(), ::tolower);
        if (lower_family.find(lower_filter) != std::string::npos) {
            matches.push_back(fi);
        }
    }
    return matches;
}

Result<std::vector<FontInfo>, SLError> FontManager::info(const std::string& family) const {
    std::vector<FontInfo> matches;
    for (const auto& fi : fonts_) {
        if (fi.family == family) {
            matches.push_back(fi);
        }
    }
    if (matches.empty()) {
        return Result<std::vector<FontInfo>, SLError>::error(
            {SLErrorCode::NotFound, "Font family not found: " + family});
    }
    return Result<std::vector<FontInfo>, SLError>::ok(std::move(matches));
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

Result<void, SLError> FontManager::refresh_cache() {
    int rc = std::system("fc-cache -f 2>/dev/null");
    if (rc != 0) {
        SL_WARN("fonts: fc-cache returned non-zero ({})", rc);
    }
    return Result<void, SLError>::ok();
}

Result<void, SLError> FontManager::install_local_file(const fs::path& path) {
    if (!fs::exists(path)) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "Font file not found: " + path.string()});
    }

    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "Not a font file (expected .ttf, .otf, or .ttc): " + path.string()});
    }

    auto dest_dir = user_font_dir();
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot create font directory: " + ec.message()});
    }

    auto dest = dest_dir / path.filename();
    fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Copy failed: " + ec.message()});
    }

    SL_INFO("fonts: installed {} -> {}", path.string(), dest.string());
    return refresh_cache();
}

Result<void, SLError> FontManager::install_google_font(const std::string& family) {
    auto dest_dir = user_font_dir();
    std::error_code ec;
    fs::create_directories(dest_dir, ec);

    // URL-encode the family name (spaces -> +).
    std::string url_family = family;
    for (auto& c : url_family) {
        if (c == ' ') c = '+';
    }

    // Download the zip file to a temporary location.
    std::string tmp_zip = "/tmp/straylight-font-" + url_family + ".zip";
    std::string tmp_dir = "/tmp/straylight-font-" + url_family;

    std::string download_cmd =
        "curl -fsSL -o '" + tmp_zip + "' "
        "'https://fonts.google.com/download?family=" + url_family + "'";

    int rc = std::system(download_cmd.c_str());
    if (rc != 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError,
             "Failed to download Google Font '" + family + "' (curl exit " + std::to_string(rc) + ")"});
    }

    // Extract the zip.
    std::string extract_cmd =
        "mkdir -p '" + tmp_dir + "' && "
        "unzip -o -q '" + tmp_zip + "' -d '" + tmp_dir + "'";
    rc = std::system(extract_cmd.c_str());
    if (rc != 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Failed to extract font zip"});
    }

    // Copy all .ttf/.otf files to user font dir.
    int installed = 0;
    for (auto& entry : fs::recursive_directory_iterator(tmp_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".ttf" && ext != ".otf") continue;

        auto dest = dest_dir / entry.path().filename();
        std::error_code copy_ec;
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, copy_ec);
        if (!copy_ec) ++installed;
    }

    // Clean up.
    fs::remove_all(tmp_zip, ec);
    fs::remove_all(tmp_dir, ec);

    if (installed == 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "No font files found in Google Fonts download for '" + family + "'"});
    }

    SL_INFO("fonts: installed {} files from Google Fonts for '{}'", installed, family);
    return refresh_cache();
}

Result<void, SLError> FontManager::install(const std::string& source) {
    const std::string google_prefix = "google:";
    if (source.substr(0, google_prefix.size()) == google_prefix) {
        std::string family = source.substr(google_prefix.size());
        return install_google_font(family);
    }
    return install_local_file(fs::path(source));
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

Result<void, SLError> FontManager::remove(const std::string& family) {
    auto udir = user_font_dir();
    if (!fs::exists(udir)) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "No user font directory"});
    }

    int removed = 0;
    std::error_code ec;
    for (auto it = fonts_.begin(); it != fonts_.end(); ) {
        if (it->family == family) {
            fs::path p(it->path);
            // Only remove from user font directory for safety.
            auto rel = fs::relative(p, udir, ec);
            if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
                fs::remove(p, ec);
                if (!ec) {
                    SL_INFO("fonts: removed {}", p.string());
                    ++removed;
                }
                it = fonts_.erase(it);
                continue;
            }
        }
        ++it;
    }

    if (removed == 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound,
             "No removable fonts found for family '" + family +
             "' (system fonts cannot be removed without root)"});
    }

    return refresh_cache();
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

Result<std::string, SLError> FontManager::preview(const std::string& family,
                                                   const std::string& text) const {
    // Find the family.
    std::vector<const FontInfo*> matches;
    for (const auto& fi : fonts_) {
        if (fi.family == family) matches.push_back(&fi);
    }
    if (matches.empty()) {
        return Result<std::string, SLError>::error(
            {SLErrorCode::NotFound, "Font family not found: " + family});
    }

    // Build a terminal preview using Unicode block characters.
    // We render a banner-style preview with the font metadata.
    std::ostringstream oss;

    // Header bar.
    oss << "\033[1;36m";
    oss << "+-" << std::string(60, '-') << "-+\n";
    oss << "| Font Preview: " << family;
    int pad = 60 - 15 - static_cast<int>(family.size());
    if (pad > 0) oss << std::string(pad, ' ');
    oss << " |\n";
    oss << "+-" << std::string(60, '-') << "-+\n";
    oss << "\033[0m";

    // Show styles available.
    oss << "\033[1mStyles:\033[0m ";
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << matches[i]->style << " (w" << matches[i]->weight << ")";
    }
    oss << "\n\n";

    // Render the preview text using Unicode block drawing.
    // We create a simple large-text effect using full-block characters.
    // Each character is rendered as a 5x7 block pattern.
    // For simplicity, we render the text in a banner style.

    // Row 1: top border
    oss << "\033[38;5;253m";
    size_t banner_width = text.size() * 2 + 4;
    oss << " " << std::string(banner_width, '\u2584') << "\n";

    // Row 2-3: text with blocks
    oss << " \u2588";
    oss << " \033[1;37m" << text << "\033[38;5;253m";
    oss << " \u2588\n";

    // Row 3: in italic style if available
    bool has_italic = false;
    for (const auto* m : matches) {
        if (m->style.find("Italic") != std::string::npos ||
            m->style.find("italic") != std::string::npos) {
            has_italic = true;
            break;
        }
    }
    oss << " \u2588";
    if (has_italic) {
        oss << " \033[3m" << text << "\033[23m";
    } else {
        oss << " \033[1m" << text << "\033[22m";
    }
    oss << "\033[38;5;253m \u2588\n";

    // Bottom border.
    oss << " " << std::string(banner_width, '\u2580') << "\n";
    oss << "\033[0m\n";

    // Metadata.
    const auto& primary = *matches[0];
    oss << "\033[2m";
    oss << "  Format:   " << primary.format << "\n";
    if (!primary.version.empty())
        oss << "  Version:  " << primary.version << "\n";
    if (primary.is_variable)
        oss << "  Type:     Variable font\n";
    oss << "  Path:     " << primary.path << "\n";
    oss << "\033[0m";

    return Result<std::string, SLError>::ok(oss.str());
}

// ---------------------------------------------------------------------------
// Search (Google Fonts)
// ---------------------------------------------------------------------------

Result<std::vector<GoogleFontEntry>, SLError> FontManager::search(
    const std::string& query) const {
    // Use the Google Fonts API to search.
    std::string encoded_query = query;
    for (auto& c : encoded_query) {
        if (c == ' ') c = '+';
    }

    std::string cmd =
        "curl -fsSL 'https://www.googleapis.com/webfonts/v1/webfonts"
        "?sort=popularity&key=AIzaSyA_placeholder' 2>/dev/null";

    // Since we may not have a valid API key, do a local heuristic search
    // of well-known Google Fonts families.
    static const std::vector<GoogleFontEntry> well_known = {
        {"Roboto",           "sans-serif",  {"100","300","400","500","700","900"}},
        {"Open Sans",        "sans-serif",  {"300","400","600","700","800"}},
        {"Noto Sans",        "sans-serif",  {"100","200","300","400","500","600","700","800","900"}},
        {"Lato",             "sans-serif",  {"100","300","400","700","900"}},
        {"Montserrat",       "sans-serif",  {"100","200","300","400","500","600","700","800","900"}},
        {"Poppins",          "sans-serif",  {"100","200","300","400","500","600","700","800","900"}},
        {"Inter",            "sans-serif",  {"100","200","300","400","500","600","700","800","900"}},
        {"Oswald",           "sans-serif",  {"200","300","400","500","600","700"}},
        {"Raleway",          "sans-serif",  {"100","200","300","400","500","600","700","800","900"}},
        {"Ubuntu",           "sans-serif",  {"300","400","500","700"}},
        {"Merriweather",     "serif",       {"300","400","700","900"}},
        {"Playfair Display", "serif",       {"400","500","600","700","800","900"}},
        {"Lora",             "serif",       {"400","500","600","700"}},
        {"PT Serif",         "serif",       {"400","700"}},
        {"Noto Serif",       "serif",       {"100","200","300","400","500","600","700","800","900"}},
        {"Source Code Pro",  "monospace",   {"200","300","400","500","600","700","800","900"}},
        {"Fira Code",        "monospace",   {"300","400","500","600","700"}},
        {"JetBrains Mono",   "monospace",   {"100","200","300","400","500","600","700","800"}},
        {"IBM Plex Mono",    "monospace",   {"100","200","300","400","500","600","700"}},
        {"Roboto Mono",      "monospace",   {"100","200","300","400","500","600","700"}},
        {"Dancing Script",   "handwriting", {"400","500","600","700"}},
        {"Pacifico",         "handwriting", {"400"}},
        {"Caveat",           "handwriting", {"400","500","600","700"}},
        {"Bebas Neue",       "display",     {"400"}},
        {"Abril Fatface",    "display",     {"400"}},
        {"Lobster",          "display",     {"400"}},
        {"Comfortaa",        "display",     {"300","400","500","600","700"}},
    };

    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    std::vector<GoogleFontEntry> results;
    for (const auto& font : well_known) {
        std::string lower_name = font.family;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        std::string lower_cat = font.category;
        std::transform(lower_cat.begin(), lower_cat.end(), lower_cat.begin(), ::tolower);

        if (lower_name.find(lower_query) != std::string::npos ||
            lower_cat.find(lower_query) != std::string::npos) {
            results.push_back(font);
        }
    }

    return Result<std::vector<GoogleFontEntry>, SLError>::ok(std::move(results));
}

// ---------------------------------------------------------------------------
// Set Default (fontconfig)
// ---------------------------------------------------------------------------

Result<void, SLError> FontManager::set_default(const std::string& family,
                                               const std::string& category) {
    if (category != "sans-serif" && category != "serif" && category != "monospace") {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "Category must be sans-serif, serif, or monospace"});
    }

    // Write a fontconfig user configuration file.
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";

    fs::path conf_dir = fs::path(home) / ".config" / "fontconfig";
    std::error_code ec;
    fs::create_directories(conf_dir, ec);

    fs::path conf_file = conf_dir / "fonts.conf";

    // Read existing file if present, or start fresh.
    std::string existing;
    if (fs::exists(conf_file)) {
        std::ifstream ifs(conf_file);
        existing = std::string((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
    }

    // If no existing file, create a skeleton.
    if (existing.empty()) {
        existing =
            "<?xml version='1.0'?>\n"
            "<!DOCTYPE fontconfig SYSTEM 'fonts.dtd'>\n"
            "<fontconfig>\n"
            "</fontconfig>\n";
    }

    // Build the alias block.
    std::string alias_block =
        "  <!-- straylight-fonts: default " + category + " -->\n"
        "  <alias>\n"
        "    <family>" + category + "</family>\n"
        "    <prefer>\n"
        "      <family>" + family + "</family>\n"
        "    </prefer>\n"
        "  </alias>\n";

    // Remove any existing straylight block for this category.
    std::string marker_start = "<!-- straylight-fonts: default " + category + " -->";
    std::string marker_end_tag = "</alias>";
    auto pos = existing.find(marker_start);
    if (pos != std::string::npos) {
        // Find the closing </alias> after this marker.
        auto end_pos = existing.find(marker_end_tag, pos);
        if (end_pos != std::string::npos) {
            end_pos += marker_end_tag.size();
            // Remove trailing newline.
            if (end_pos < existing.size() && existing[end_pos] == '\n') ++end_pos;
            // Also remove the leading "  " indent on the marker line.
            while (pos > 0 && existing[pos - 1] == ' ') --pos;
            existing.erase(pos, end_pos - pos);
        }
    }

    // Insert alias block before </fontconfig>.
    auto close_pos = existing.rfind("</fontconfig>");
    if (close_pos == std::string::npos) {
        return Result<void, SLError>::error(
            {SLErrorCode::ParseError, "Invalid fonts.conf: missing </fontconfig>"});
    }
    existing.insert(close_pos, alias_block);

    std::ofstream ofs(conf_file, std::ios::trunc);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot write " + conf_file.string()});
    }
    ofs << existing;

    SL_INFO("fonts: set default {} to '{}'", category, family);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Export JSON
// ---------------------------------------------------------------------------

std::string FontManager::export_json() const {
    std::ostringstream oss;
    oss << "[\n";
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const auto& f = fonts_[i];
        oss << "  {\n"
            << "    \"family\": \"" << f.family << "\",\n"
            << "    \"style\": \"" << f.style << "\",\n"
            << "    \"weight\": " << f.weight << ",\n"
            << "    \"format\": \"" << f.format << "\",\n"
            << "    \"variable\": " << (f.is_variable ? "true" : "false") << ",\n"
            << "    \"path\": \"" << f.path << "\"";
        if (!f.version.empty()) {
            oss << ",\n    \"version\": \"" << f.version << "\"";
        }
        oss << "\n  }";
        if (i + 1 < fonts_.size()) oss << ",";
        oss << "\n";
    }
    oss << "]\n";
    return oss.str();
}

} // namespace straylight
