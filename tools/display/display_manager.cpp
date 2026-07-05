// tools/display/display_manager.cpp
// Full implementation of display/monitor configuration for StrayLight OS.

#include "display_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

DisplayManager::DisplayManager() {
    fs::create_directories(config_dir());
}

DisplayManager::~DisplayManager() = default;

std::string DisplayManager::config_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight";
}

Result<std::string, std::string> DisplayManager::run_cmd(const std::string& cmd) const {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd +
            "\noutput: " + output);
    }
    return Result<std::string, std::string>::ok(output);
}

DisplayManager::Backend DisplayManager::detect_backend() const {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0') {
        return Backend::Wayland;
    }
    const char* display = std::getenv("DISPLAY");
    if (display && display[0] != '\0') {
        return Backend::X11;
    }
    return Backend::DRM;
}

// ---------------------------------------------------------------------------
// xrandr parsing
// ---------------------------------------------------------------------------

std::vector<OutputInfo> DisplayManager::parse_xrandr(const std::string& raw) const {
    std::vector<OutputInfo> outputs;
    std::istringstream stream(raw);
    std::string line;
    OutputInfo* current = nullptr;

    // Regex for output line: "HDMI-A-1 connected primary 1920x1080+0+0 ..."
    std::regex output_re(R"(^(\S+)\s+(connected|disconnected)\s*(primary)?\s*(\d+x\d+\+\d+\+\d+)?)");
    // Regex for mode line: "   1920x1080     60.00*+  59.94  ..."
    std::regex mode_re(R"(^\s+(\d+)x(\d+)\s+(.*))");

    while (std::getline(stream, line)) {
        std::smatch m;
        if (std::regex_search(line, m, output_re)) {
            outputs.emplace_back();
            current = &outputs.back();
            current->name = m[1].str();
            current->connected = (m[2].str() == "connected");

            if (m[4].matched) {
                current->enabled = true;
                // Parse "1920x1080+0+0"
                std::string geom = m[4].str();
                int w = 0, h = 0, x = 0, y = 0;
                if (sscanf(geom.c_str(), "%dx%d+%d+%d", &w, &h, &x, &y) >= 2) {
                    current->active_mode.width = w;
                    current->active_mode.height = h;
                    current->pos_x = x;
                    current->pos_y = y;
                }
            }
        } else if (current && std::regex_search(line, m, mode_re)) {
            DisplayMode mode;
            mode.width = std::stoi(m[1].str());
            mode.height = std::stoi(m[2].str());

            std::string rates_str = m[3].str();
            // Parse rates like "60.00*+ 59.94 50.00"
            std::regex rate_re(R"((\d+\.\d+)(\*?)(\+?))");
            auto it = std::sregex_iterator(rates_str.begin(), rates_str.end(), rate_re);
            for (; it != std::sregex_iterator(); ++it) {
                DisplayMode rm = mode;
                rm.refresh_hz = std::stod((*it)[1].str());
                rm.current = !(*it)[2].str().empty();
                rm.preferred = !(*it)[3].str().empty();
                current->modes.push_back(rm);

                if (rm.current) {
                    current->active_mode.refresh_hz = rm.refresh_hz;
                    current->active_mode.current = true;
                }
            }
        }
    }
    return outputs;
}

// ---------------------------------------------------------------------------
// wlr-randr parsing
// ---------------------------------------------------------------------------

std::vector<OutputInfo> DisplayManager::parse_wlr_randr(const std::string& raw) const {
    std::vector<OutputInfo> outputs;
    std::istringstream stream(raw);
    std::string line;
    OutputInfo* current = nullptr;

    while (std::getline(stream, line)) {
        // Output header line: "HDMI-A-1 "Samsung ..." (DP-1)"
        if (!line.empty() && line[0] != ' ') {
            outputs.emplace_back();
            current = &outputs.back();
            // First token is the output name
            auto sp = line.find(' ');
            current->name = (sp != std::string::npos) ? line.substr(0, sp) : line;
            current->connected = true;

            // Try to extract make/model from description in quotes
            auto q1 = line.find('"');
            auto q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                current->model = line.substr(q1 + 1, q2 - q1 - 1);
            }
        } else if (current) {
            // Trim leading whitespace
            std::string trimmed = line;
            auto pos = trimmed.find_first_not_of(" \t");
            if (pos != std::string::npos) trimmed = trimmed.substr(pos);

            // "Enabled: yes"
            if (trimmed.rfind("Enabled:", 0) == 0) {
                current->enabled = (trimmed.find("yes") != std::string::npos);
            }
            // "Position: 0,0"
            else if (trimmed.rfind("Position:", 0) == 0) {
                int x = 0, y = 0;
                if (sscanf(trimmed.c_str(), "Position: %d,%d", &x, &y) == 2) {
                    current->pos_x = x;
                    current->pos_y = y;
                }
            }
            // "Transform: normal"
            else if (trimmed.rfind("Transform:", 0) == 0) {
                if (trimmed.find("90") != std::string::npos) current->rotation_deg = 90;
                else if (trimmed.find("180") != std::string::npos) current->rotation_deg = 180;
                else if (trimmed.find("270") != std::string::npos) current->rotation_deg = 270;
                else current->rotation_deg = 0;
            }
            // Mode lines: "1920x1080 px, 60.000000 Hz (preferred, current)"
            else {
                std::regex mode_re(R"((\d+)x(\d+)\s+px,\s+(\d+\.?\d*)\s+Hz\s*(.*)?)");
                std::smatch m;
                if (std::regex_search(trimmed, m, mode_re)) {
                    DisplayMode dm;
                    dm.width = std::stoi(m[1].str());
                    dm.height = std::stoi(m[2].str());
                    dm.refresh_hz = std::stod(m[3].str());
                    std::string flags = m[4].str();
                    dm.current = (flags.find("current") != std::string::npos);
                    dm.preferred = (flags.find("preferred") != std::string::npos);
                    current->modes.push_back(dm);

                    if (dm.current) {
                        current->active_mode = dm;
                    }
                }
            }
        }
    }
    return outputs;
}

// ---------------------------------------------------------------------------
// EDID parsing from /sys/class/drm
// ---------------------------------------------------------------------------

Result<OutputInfo, std::string> DisplayManager::parse_edid(const std::string& drm_path) const {
    OutputInfo info;

    // Read status
    std::ifstream status_file(drm_path + "/status");
    if (status_file.is_open()) {
        std::string status;
        std::getline(status_file, status);
        info.connected = (status == "connected");
    }

    // Extract output name from path: /sys/class/drm/card0-HDMI-A-1 -> HDMI-A-1
    std::string basename = fs::path(drm_path).filename().string();
    auto dash = basename.find('-');
    if (dash != std::string::npos) {
        info.name = basename.substr(dash + 1);
    } else {
        info.name = basename;
    }

    // Read EDID binary for manufacturer / model
    std::ifstream edid_file(drm_path + "/edid", std::ios::binary);
    if (edid_file.is_open()) {
        std::vector<uint8_t> edid((std::istreambuf_iterator<char>(edid_file)),
                                   std::istreambuf_iterator<char>());
        if (edid.size() >= 128) {
            // Manufacturer ID at bytes 8-9 (compressed ASCII)
            uint16_t mfg = (static_cast<uint16_t>(edid[8]) << 8) | edid[9];
            char c1 = static_cast<char>(((mfg >> 10) & 0x1F) + 'A' - 1);
            char c2 = static_cast<char>(((mfg >> 5) & 0x1F) + 'A' - 1);
            char c3 = static_cast<char>((mfg & 0x1F) + 'A' - 1);
            info.make = {c1, c2, c3};

            // Product code at bytes 10-11
            uint16_t product = static_cast<uint16_t>(edid[10]) |
                               (static_cast<uint16_t>(edid[11]) << 8);
            info.model = info.make + "-" + std::to_string(product);

            // Serial from bytes 12-15
            uint32_t serial = static_cast<uint32_t>(edid[12]) |
                              (static_cast<uint32_t>(edid[13]) << 8) |
                              (static_cast<uint32_t>(edid[14]) << 16) |
                              (static_cast<uint32_t>(edid[15]) << 24);
            if (serial != 0) {
                info.serial = std::to_string(serial);
            }

            // Parse descriptor blocks (bytes 54–125) for monitor name
            for (int i = 0; i < 4; ++i) {
                int offset = 54 + i * 18;
                if (offset + 18 > static_cast<int>(edid.size())) break;
                // Monitor name descriptor: tag = 0xFC
                if (edid[offset] == 0 && edid[offset + 1] == 0 &&
                    edid[offset + 2] == 0 && edid[offset + 3] == 0xFC) {
                    std::string name;
                    for (int j = 5; j < 18; ++j) {
                        char ch = static_cast<char>(edid[offset + j]);
                        if (ch == '\n' || ch == '\0') break;
                        name += ch;
                    }
                    if (!name.empty()) info.model = name;
                }
            }

            // Parse preferred timing from first detailed timing block (bytes 54-71)
            if (edid.size() >= 72 && (edid[54] != 0 || edid[55] != 0)) {
                int hactive = static_cast<int>(edid[56]) |
                              ((static_cast<int>(edid[58]) & 0xF0) << 4);
                int vactive = static_cast<int>(edid[59]) |
                              ((static_cast<int>(edid[61]) & 0xF0) << 4);
                int pixel_clock = (static_cast<int>(edid[54]) |
                                   (static_cast<int>(edid[55]) << 8)) * 10000;

                int hblank = static_cast<int>(edid[57]) |
                             ((static_cast<int>(edid[58]) & 0x0F) << 8);
                int vblank = static_cast<int>(edid[60]) |
                             ((static_cast<int>(edid[61]) & 0x0F) << 8);

                int htotal = hactive + hblank;
                int vtotal = vactive + vblank;

                if (htotal > 0 && vtotal > 0 && pixel_clock > 0) {
                    DisplayMode mode;
                    mode.width = hactive;
                    mode.height = vactive;
                    mode.refresh_hz = static_cast<double>(pixel_clock) /
                                      (static_cast<double>(htotal) * vtotal);
                    mode.preferred = true;
                    info.modes.push_back(mode);
                }
            }
        }
    }

    return Result<OutputInfo, std::string>::ok(info);
}

// ---------------------------------------------------------------------------
// list_outputs
// ---------------------------------------------------------------------------

Result<std::vector<OutputInfo>, std::string> DisplayManager::list_outputs() const {
    Backend backend = detect_backend();

    if (backend == Backend::Wayland) {
        auto res = run_cmd("wlr-randr 2>/dev/null");
        if (res.has_value() && !res.value().empty()) {
            return Result<std::vector<OutputInfo>, std::string>::ok(
                parse_wlr_randr(res.value()));
        }
        // Fall through to xrandr under XWayland
    }

    if (backend == Backend::X11 || backend == Backend::Wayland) {
        auto res = run_cmd("xrandr --query 2>/dev/null");
        if (res.has_value()) {
            return Result<std::vector<OutputInfo>, std::string>::ok(
                parse_xrandr(res.value()));
        }
    }

    // DRM fallback: read /sys/class/drm
    std::vector<OutputInfo> outputs;
    std::string drm_base = "/sys/class/drm";
    if (!fs::exists(drm_base)) {
        return Result<std::vector<OutputInfo>, std::string>::error(
            "no display backend available");
    }

    for (const auto& entry : fs::directory_iterator(drm_base)) {
        std::string name = entry.path().filename().string();
        // Skip entries like "card0" (no connector), look for "card0-HDMI-A-1"
        if (name.find("card") != 0 || name.find('-') == std::string::npos) continue;

        auto res = parse_edid(entry.path().string());
        if (res.has_value()) {
            outputs.push_back(res.value());
        }
    }

    return Result<std::vector<OutputInfo>, std::string>::ok(outputs);
}

// ---------------------------------------------------------------------------
// set_mode
// ---------------------------------------------------------------------------

Result<void, std::string> DisplayManager::set_mode(const std::string& output,
                                                     int width, int height,
                                                     double refresh_hz) {
    Backend backend = detect_backend();
    std::string mode_str = std::to_string(width) + "x" + std::to_string(height);

    if (backend == Backend::Wayland) {
        // Try wlr-randr first
        std::ostringstream cmd;
        cmd << "wlr-randr --output " << output
            << " --mode " << mode_str
            << "@" << std::fixed << refresh_hz << "Hz 2>/dev/null";
        auto res = run_cmd(cmd.str());
        if (res.has_value()) return Result<void, std::string>::ok();
    }

    // xrandr fallback
    std::ostringstream cmd;
    cmd << "xrandr --output " << output << " --mode " << mode_str;
    if (refresh_hz > 0) {
        cmd << " --rate " << std::fixed << refresh_hz;
    }
    auto res = run_cmd(cmd.str());
    if (!res.has_value()) {
        return Result<void, std::string>::error(res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// set_position
// ---------------------------------------------------------------------------

Result<void, std::string> DisplayManager::set_position(const std::string& output,
                                                         int x, int y) {
    Backend backend = detect_backend();
    std::string pos_str = std::to_string(x) + "," + std::to_string(y);

    if (backend == Backend::Wayland) {
        std::string cmd = "wlr-randr --output " + output + " --pos " + pos_str + " 2>/dev/null";
        auto res = run_cmd(cmd);
        if (res.has_value()) return Result<void, std::string>::ok();
    }

    std::string cmd = "xrandr --output " + output + " --pos " +
                      std::to_string(x) + "x" + std::to_string(y);
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error(res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// rotate
// ---------------------------------------------------------------------------

Result<void, std::string> DisplayManager::rotate(const std::string& output, int degrees) {
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        return Result<void, std::string>::error(
            "invalid rotation: must be 0, 90, 180, or 270");
    }

    Backend backend = detect_backend();

    if (backend == Backend::Wayland) {
        std::string transform;
        switch (degrees) {
            case 0:   transform = "normal"; break;
            case 90:  transform = "90"; break;
            case 180: transform = "180"; break;
            case 270: transform = "270"; break;
        }
        std::string cmd = "wlr-randr --output " + output +
                          " --transform " + transform + " 2>/dev/null";
        auto res = run_cmd(cmd);
        if (res.has_value()) return Result<void, std::string>::ok();
    }

    // xrandr rotation names
    std::string rot_name;
    switch (degrees) {
        case 0:   rot_name = "normal"; break;
        case 90:  rot_name = "left"; break;
        case 180: rot_name = "inverted"; break;
        case 270: rot_name = "right"; break;
    }
    std::string cmd = "xrandr --output " + output + " --rotate " + rot_name;
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error(res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// mirror
// ---------------------------------------------------------------------------

Result<void, std::string> DisplayManager::mirror(const std::string& source,
                                                   const std::string& dest) {
    Backend backend = detect_backend();

    // Get source's current mode to apply to dest
    auto outputs_res = list_outputs();
    if (!outputs_res.has_value()) {
        return Result<void, std::string>::error(outputs_res.error());
    }

    const OutputInfo* src_info = nullptr;
    for (const auto& o : outputs_res.value()) {
        if (o.name == source) { src_info = &o; break; }
    }
    if (!src_info) {
        return Result<void, std::string>::error("source output '" + source + "' not found");
    }

    std::string mode_str = std::to_string(src_info->active_mode.width) + "x" +
                           std::to_string(src_info->active_mode.height);

    if (backend == Backend::Wayland) {
        std::string cmd = "wlr-randr --output " + dest + " --mode " + mode_str +
                          " --pos 0,0 2>/dev/null";
        auto res = run_cmd(cmd);
        if (res.has_value()) {
            cmd = "wlr-randr --output " + source + " --pos 0,0 2>/dev/null";
            run_cmd(cmd);
            return Result<void, std::string>::ok();
        }
    }

    std::string cmd = "xrandr --output " + dest + " --same-as " + source +
                      " --mode " + mode_str;
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error(res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------

Result<DisplayProfile, std::string> DisplayManager::read_profile(const std::string& name) const {
    std::string path = config_dir() + "/" + kProfilesFile;
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<DisplayProfile, std::string>::error("no profiles file found");
    }

    // Simple JSON parsing for profiles array
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Find the profile by name in the JSON content
    // Format: {"profiles": [{"name": "...", "outputs": [...]}]}
    std::string search = "\"name\":\"" + name + "\"";
    std::string search2 = "\"name\": \"" + name + "\"";
    auto pos = content.find(search);
    if (pos == std::string::npos) pos = content.find(search2);
    if (pos == std::string::npos) {
        return Result<DisplayProfile, std::string>::error(
            "profile '" + name + "' not found");
    }

    // Find the enclosing object braces
    auto obj_start = content.rfind('{', pos);
    int brace_depth = 1;
    auto obj_end = obj_start + 1;
    while (obj_end < content.size() && brace_depth > 0) {
        if (content[obj_end] == '{') ++brace_depth;
        else if (content[obj_end] == '}') --brace_depth;
        ++obj_end;
    }

    std::string obj_str = content.substr(obj_start, obj_end - obj_start);

    DisplayProfile profile;
    profile.name = name;

    // Parse output configs within this profile object
    // Look for "outputs": [...]
    auto outputs_pos = obj_str.find("\"outputs\"");
    if (outputs_pos == std::string::npos) {
        return Result<DisplayProfile, std::string>::ok(profile);
    }

    auto arr_start = obj_str.find('[', outputs_pos);
    auto arr_end = obj_str.find(']', arr_start);
    if (arr_start == std::string::npos || arr_end == std::string::npos) {
        return Result<DisplayProfile, std::string>::ok(profile);
    }

    std::string arr_str = obj_str.substr(arr_start + 1, arr_end - arr_start - 1);

    // Parse each output object in the array
    size_t search_pos = 0;
    while (true) {
        auto entry_start = arr_str.find('{', search_pos);
        if (entry_start == std::string::npos) break;

        int depth = 1;
        auto entry_end = entry_start + 1;
        while (entry_end < arr_str.size() && depth > 0) {
            if (arr_str[entry_end] == '{') ++depth;
            else if (arr_str[entry_end] == '}') --depth;
            ++entry_end;
        }

        std::string entry = arr_str.substr(entry_start, entry_end - entry_start);

        DisplayProfile::OutputConfig cfg;

        // Extract fields with regex
        std::regex str_re(R"("output_name"\s*:\s*"([^"]*)")");
        std::regex w_re(R"("width"\s*:\s*(\d+))");
        std::regex h_re(R"("height"\s*:\s*(\d+))");
        std::regex hz_re(R"("refresh_hz"\s*:\s*(\d+\.?\d*))");
        std::regex px_re(R"("pos_x"\s*:\s*(-?\d+))");
        std::regex py_re(R"("pos_y"\s*:\s*(-?\d+))");
        std::regex rot_re(R"("rotation_deg"\s*:\s*(\d+))");
        std::regex en_re(R"("enabled"\s*:\s*(true|false))");
        std::regex pri_re(R"("primary"\s*:\s*(true|false))");

        std::smatch m;
        if (std::regex_search(entry, m, str_re)) cfg.output_name = m[1].str();
        if (std::regex_search(entry, m, w_re)) cfg.width = std::stoi(m[1].str());
        if (std::regex_search(entry, m, h_re)) cfg.height = std::stoi(m[1].str());
        if (std::regex_search(entry, m, hz_re)) cfg.refresh_hz = std::stod(m[1].str());
        if (std::regex_search(entry, m, px_re)) cfg.pos_x = std::stoi(m[1].str());
        if (std::regex_search(entry, m, py_re)) cfg.pos_y = std::stoi(m[1].str());
        if (std::regex_search(entry, m, rot_re)) cfg.rotation_deg = std::stoi(m[1].str());
        if (std::regex_search(entry, m, en_re)) cfg.enabled = (m[1].str() == "true");
        if (std::regex_search(entry, m, pri_re)) cfg.primary = (m[1].str() == "true");

        profile.outputs.push_back(cfg);
        search_pos = entry_end;
    }

    return Result<DisplayProfile, std::string>::ok(profile);
}

Result<void, std::string> DisplayManager::write_profile(const DisplayProfile& profile) const {
    std::string path = config_dir() + "/" + kProfilesFile;

    // Read existing profiles
    std::string content;
    {
        std::ifstream file(path);
        if (file.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
        }
    }

    // Build the profile JSON object
    std::ostringstream pj;
    pj << "    {\n"
       << "      \"name\": \"" << profile.name << "\",\n"
       << "      \"outputs\": [\n";

    for (size_t i = 0; i < profile.outputs.size(); ++i) {
        const auto& o = profile.outputs[i];
        pj << "        {\n"
           << "          \"output_name\": \"" << o.output_name << "\",\n"
           << "          \"width\": " << o.width << ",\n"
           << "          \"height\": " << o.height << ",\n"
           << "          \"refresh_hz\": " << o.refresh_hz << ",\n"
           << "          \"pos_x\": " << o.pos_x << ",\n"
           << "          \"pos_y\": " << o.pos_y << ",\n"
           << "          \"rotation_deg\": " << o.rotation_deg << ",\n"
           << "          \"enabled\": " << (o.enabled ? "true" : "false") << ",\n"
           << "          \"primary\": " << (o.primary ? "true" : "false") << "\n"
           << "        }";
        if (i + 1 < profile.outputs.size()) pj << ",";
        pj << "\n";
    }
    pj << "      ]\n"
       << "    }";

    // If file doesn't exist or is empty, create new structure
    if (content.empty() || content.find("\"profiles\"") == std::string::npos) {
        std::ofstream out(path);
        if (!out.is_open()) {
            return Result<void, std::string>::error("cannot write to " + path);
        }
        out << "{\n  \"profiles\": [\n" << pj.str() << "\n  ]\n}\n";
        return Result<void, std::string>::ok();
    }

    // Remove existing profile with same name if present
    std::string search = "\"name\":\"" + profile.name + "\"";
    std::string search2 = "\"name\": \"" + profile.name + "\"";
    auto existing = content.find(search);
    if (existing == std::string::npos) existing = content.find(search2);

    if (existing != std::string::npos) {
        // Find and remove the enclosing object
        auto obj_start = content.rfind('{', existing);
        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < content.size() && depth > 0) {
            if (content[obj_end] == '{') ++depth;
            else if (content[obj_end] == '}') --depth;
            ++obj_end;
        }
        // Remove trailing comma if present
        auto after = content.find_first_not_of(" \t\n\r,", obj_end);
        content.erase(obj_start, (after != std::string::npos ? after : obj_end) - obj_start);
    }

    // Insert new profile before the closing bracket of the profiles array
    auto arr_end = content.rfind(']');
    if (arr_end == std::string::npos) {
        return Result<void, std::string>::error("malformed profiles file");
    }

    // Check if there are existing entries (need comma)
    auto arr_start = content.find('[');
    std::string between = content.substr(arr_start + 1, arr_end - arr_start - 1);
    bool has_entries = (between.find('{') != std::string::npos);

    std::string insert = (has_entries ? ",\n" : "\n") + pj.str() + "\n";
    content.insert(arr_end, insert);

    std::ofstream out(path);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write to " + path);
    }
    out << content;
    return Result<void, std::string>::ok();
}

Result<void, std::string> DisplayManager::save_profile(const std::string& name) {
    auto outputs_res = list_outputs();
    if (!outputs_res.has_value()) {
        return Result<void, std::string>::error(outputs_res.error());
    }

    DisplayProfile profile;
    profile.name = name;

    for (const auto& o : outputs_res.value()) {
        if (!o.connected || !o.enabled) continue;
        DisplayProfile::OutputConfig cfg;
        cfg.output_name = o.name;
        cfg.width = o.active_mode.width;
        cfg.height = o.active_mode.height;
        cfg.refresh_hz = o.active_mode.refresh_hz;
        cfg.pos_x = o.pos_x;
        cfg.pos_y = o.pos_y;
        cfg.rotation_deg = o.rotation_deg;
        cfg.enabled = true;
        profile.outputs.push_back(cfg);
    }

    return write_profile(profile);
}

Result<void, std::string> DisplayManager::load_profile(const std::string& name) {
    auto profile_res = read_profile(name);
    if (!profile_res.has_value()) {
        return Result<void, std::string>::error(profile_res.error());
    }

    const auto& profile = profile_res.value();
    for (const auto& cfg : profile.outputs) {
        if (!cfg.enabled) {
            // Disable the output
            Backend backend = detect_backend();
            if (backend == Backend::Wayland) {
                run_cmd("wlr-randr --output " + cfg.output_name + " --off 2>/dev/null");
            } else {
                run_cmd("xrandr --output " + cfg.output_name + " --off");
            }
            continue;
        }

        auto mode_res = set_mode(cfg.output_name, cfg.width, cfg.height, cfg.refresh_hz);
        if (!mode_res.has_value()) {
            return Result<void, std::string>::error(
                "failed to set mode for " + cfg.output_name + ": " + mode_res.error());
        }

        auto pos_res = set_position(cfg.output_name, cfg.pos_x, cfg.pos_y);
        if (!pos_res.has_value()) {
            return Result<void, std::string>::error(
                "failed to set position for " + cfg.output_name + ": " + pos_res.error());
        }

        if (cfg.rotation_deg != 0) {
            auto rot_res = rotate(cfg.output_name, cfg.rotation_deg);
            if (!rot_res.has_value()) {
                return Result<void, std::string>::error(
                    "failed to rotate " + cfg.output_name + ": " + rot_res.error());
            }
        }
    }

    return Result<void, std::string>::ok();
}

std::vector<std::string> DisplayManager::list_profiles() const {
    std::vector<std::string> names;
    std::string path = config_dir() + "/" + kProfilesFile;
    std::ifstream file(path);
    if (!file.is_open()) return names;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    std::regex name_re(R"("name"\s*:\s*"([^"]*)")");
    auto it = std::sregex_iterator(content.begin(), content.end(), name_re);
    for (; it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    return names;
}

Result<void, std::string> DisplayManager::delete_profile(const std::string& name) {
    std::string path = config_dir() + "/" + kProfilesFile;
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<void, std::string>::error("no profiles file found");
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    std::string search = "\"name\": \"" + name + "\"";
    std::string search2 = "\"name\":\"" + name + "\"";
    auto pos = content.find(search);
    if (pos == std::string::npos) pos = content.find(search2);
    if (pos == std::string::npos) {
        return Result<void, std::string>::error("profile '" + name + "' not found");
    }

    auto obj_start = content.rfind('{', pos);
    int depth = 1;
    auto obj_end = obj_start + 1;
    while (obj_end < content.size() && depth > 0) {
        if (content[obj_end] == '{') ++depth;
        else if (content[obj_end] == '}') --depth;
        ++obj_end;
    }

    // Remove trailing comma or leading comma
    auto after = obj_end;
    while (after < content.size() && (content[after] == ' ' || content[after] == '\n' ||
           content[after] == '\r' || content[after] == '\t')) ++after;
    if (after < content.size() && content[after] == ',') ++after;

    content.erase(obj_start, after - obj_start);

    std::ofstream out(path);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write to " + path);
    }
    out << content;
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Night mode / color temperature
// ---------------------------------------------------------------------------

NightModeSettings DisplayManager::read_night_config() const {
    NightModeSettings settings;
    std::string path = config_dir() + "/night-mode.json";
    std::ifstream file(path);
    if (!file.is_open()) return settings;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    std::regex en_re(R"("enabled"\s*:\s*(true|false))");
    std::regex temp_re(R"("temperature_k"\s*:\s*(\d+))");
    std::regex sched_re(R"("schedule"\s*:\s*"([^"]*)")");

    std::smatch m;
    if (std::regex_search(content, m, en_re)) settings.enabled = (m[1].str() == "true");
    if (std::regex_search(content, m, temp_re)) settings.temperature_k = std::stoi(m[1].str());
    if (std::regex_search(content, m, sched_re)) settings.schedule = m[1].str();

    return settings;
}

void DisplayManager::write_night_config(const NightModeSettings& settings) const {
    std::string path = config_dir() + "/night-mode.json";
    std::ofstream out(path);
    if (!out.is_open()) return;
    out << "{\n"
        << "  \"enabled\": " << (settings.enabled ? "true" : "false") << ",\n"
        << "  \"temperature_k\": " << settings.temperature_k << ",\n"
        << "  \"schedule\": \"" << settings.schedule << "\"\n"
        << "}\n";
}

Result<void, std::string> DisplayManager::set_color_temp(int temperature_k) {
    if (temperature_k < 1000 || temperature_k > 6500) {
        return Result<void, std::string>::error(
            "temperature must be between 1000 and 6500 Kelvin");
    }

    // Calculate gamma values from color temperature (Tanner Helland algorithm)
    double temp = static_cast<double>(temperature_k) / 100.0;
    double red = 1.0, green = 1.0, blue = 1.0;

    if (temp <= 66.0) {
        red = 1.0;
        green = 0.39008157876901960784 * std::log(temp) - 0.63184144378862745098;
        if (temp <= 19.0) {
            blue = 0.0;
        } else {
            blue = 0.54320678911019607843 * std::log(temp - 10.0) - 1.19625408914;
        }
    } else {
        red = 1.29293618606274509804 * std::pow(temp - 60.0, -0.1332047592);
        green = 1.12989086089529411765 * std::pow(temp - 60.0, -0.0755148492);
        blue = 1.0;
    }

    // Clamp
    red = std::max(0.0, std::min(1.0, red));
    green = std::max(0.0, std::min(1.0, green));
    blue = std::max(0.0, std::min(1.0, blue));

    // Apply via xrandr gamma (format: R:G:B)
    std::ostringstream gamma;
    gamma << std::fixed << std::setprecision(3) << red << ":" << green << ":" << blue;

    auto outputs_res = list_outputs();
    if (!outputs_res.has_value()) {
        return Result<void, std::string>::error(outputs_res.error());
    }

    for (const auto& output : outputs_res.value()) {
        if (!output.connected || !output.enabled) continue;

        Backend backend = detect_backend();
        if (backend == Backend::Wayland) {
            // gammastep or wlsunset for Wayland
            std::string cmd = "gammastep -O " + std::to_string(temperature_k) + " 2>/dev/null &";
            run_cmd(cmd);
            break; // gammastep applies globally
        } else {
            std::string cmd = "xrandr --output " + output.name +
                              " --gamma " + gamma.str();
            run_cmd(cmd);
        }
    }

    // Save settings
    auto settings = read_night_config();
    settings.temperature_k = temperature_k;
    settings.enabled = true;
    write_night_config(settings);

    return Result<void, std::string>::ok();
}

Result<void, std::string> DisplayManager::set_night_mode(const NightModeSettings& settings) {
    write_night_config(settings);

    if (settings.enabled) {
        return set_color_temp(settings.temperature_k);
    }

    // Disable: reset gamma to 1:1:1
    auto outputs_res = list_outputs();
    if (outputs_res.has_value()) {
        for (const auto& output : outputs_res.value()) {
            if (!output.connected || !output.enabled) continue;
            run_cmd("xrandr --output " + output.name + " --gamma 1:1:1 2>/dev/null");
        }
    }
    // Kill gammastep if running
    run_cmd("killall gammastep 2>/dev/null");

    return Result<void, std::string>::ok();
}

NightModeSettings DisplayManager::get_night_mode() const {
    return read_night_config();
}

// ---------------------------------------------------------------------------
// ICC profiles
// ---------------------------------------------------------------------------

Result<void, std::string> DisplayManager::load_icc_profile(const std::string& output,
                                                             const std::string& icc_path) {
    if (!fs::exists(icc_path)) {
        return Result<void, std::string>::error("ICC profile not found: " + icc_path);
    }

    Backend backend = detect_backend();

    if (backend == Backend::Wayland) {
        // Wayland: use colord or compositor-specific method
        std::string cmd = "colormgr device-add-profile " + output + " " + icc_path + " 2>/dev/null";
        auto res = run_cmd(cmd);
        if (res.has_value()) return Result<void, std::string>::ok();
    }

    // X11: use xcalib or xiccd
    std::string cmd = "xcalib " + icc_path + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        // Fallback to colord
        cmd = "colormgr device-add-profile " + output + " " + icc_path + " 2>/dev/null";
        res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "failed to load ICC profile: " + res.error());
        }
    }

    return Result<void, std::string>::ok();
}

} // namespace straylight
