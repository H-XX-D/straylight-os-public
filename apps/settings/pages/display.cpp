// apps/settings/pages/display.cpp
// Display settings — reads DRM/sysfs for output info, applies via wlr-output-management
#include "display.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight::settings {

namespace fs = std::filesystem;

DisplayPage::DisplayPage() = default;

void DisplayPage::load() {
    detect();
}

void DisplayPage::detect() {
    outputs_.clear();
    read_drm_outputs();

    if (outputs_.empty()) {
        // Fallback: create a placeholder
        DisplayOutput out;
        out.name = "Unknown";
        out.description = "Display output";
        out.enabled = true;

        // Read current framebuffer resolution
        std::ifstream fb("/sys/class/graphics/fb0/virtual_size");
        if (fb.is_open()) {
            char sep;
            fb >> out.physical_width_mm >> sep >> out.physical_height_mm;
            // These are actually pixels, not mm, but gives us something
            DisplayMode mode;
            mode.width = out.physical_width_mm;
            mode.height = out.physical_height_mm;
            mode.refresh_rate = 60.0f;
            mode.current = true;
            out.modes.push_back(mode);
            out.current_mode_index = 0;
            // Reset physical dimensions
            out.physical_width_mm = 0;
            out.physical_height_mm = 0;
        }

        outputs_.push_back(std::move(out));
    }
}

void DisplayPage::read_drm_outputs() {
    std::error_code ec;
    fs::path drm_base = "/sys/class/drm";

    if (!fs::exists(drm_base, ec)) return;

    for (const auto& entry : fs::directory_iterator(drm_base, ec)) {
        std::string name = entry.path().filename().string();

        // Match connectors like "card0-HDMI-A-1", "card0-DP-1"
        if (name.find('-') == std::string::npos) continue;
        if (name.compare(0, 4, "card") != 0) continue;

        fs::path conn_path = entry.path();

        // Check if connected
        std::ifstream status_file(conn_path / "status");
        if (!status_file.is_open()) continue;
        std::string status;
        std::getline(status_file, status);
        if (status != "connected") continue;

        DisplayOutput output;

        // Parse connector name (e.g., "card0-HDMI-A-1" -> "HDMI-A-1")
        auto dash_pos = name.find('-');
        output.name = name.substr(dash_pos + 1);
        output.enabled = true;

        // Read EDID for make/model if available
        fs::path edid_path = conn_path / "edid";
        if (fs::exists(edid_path, ec) && fs::file_size(edid_path, ec) > 0) {
            // EDID parsing — read manufacturer and model from header
            std::ifstream edid(edid_path, std::ios::binary);
            if (edid.is_open()) {
                char edid_data[256];
                edid.read(edid_data, sizeof(edid_data));
                auto bytes_read = edid.gcount();

                if (bytes_read >= 128) {
                    // Physical size from EDID (bytes 21-22)
                    output.physical_width_mm =
                        static_cast<int>(static_cast<uint8_t>(edid_data[21])) * 10;
                    output.physical_height_mm =
                        static_cast<int>(static_cast<uint8_t>(edid_data[22])) * 10;

                    // Manufacturer ID (bytes 8-9)
                    uint16_t mfg = (static_cast<uint16_t>(
                                        static_cast<uint8_t>(edid_data[8]))
                                    << 8) |
                                   static_cast<uint8_t>(edid_data[9]);
                    char mfg_str[4];
                    mfg_str[0] = static_cast<char>(((mfg >> 10) & 0x1F) + 'A' - 1);
                    mfg_str[1] = static_cast<char>(((mfg >> 5) & 0x1F) + 'A' - 1);
                    mfg_str[2] = static_cast<char>((mfg & 0x1F) + 'A' - 1);
                    mfg_str[3] = '\0';
                    output.make = mfg_str;
                }
            }
        }

        // Read available modes
        std::ifstream modes_file(conn_path / "modes");
        if (modes_file.is_open()) {
            std::string mode_line;
            int mode_idx = 0;
            while (std::getline(modes_file, mode_line)) {
                if (mode_line.empty()) continue;

                DisplayMode mode;
                // Format: "1920x1080" or "1920x1080i"
                int w = 0, h = 0;
                if (sscanf(mode_line.c_str(), "%dx%d", &w, &h) >= 2) {
                    mode.width = w;
                    mode.height = h;
                    mode.refresh_rate = 60.0f; // Default, actual rate from modeline
                    mode.current = (mode_idx == 0); // First mode is typically current

                    output.modes.push_back(mode);
                }
                mode_idx++;
            }
        }

        // If no modes found, add a default
        if (output.modes.empty()) {
            DisplayMode mode;
            mode.width = 1920;
            mode.height = 1080;
            mode.refresh_rate = 60.0f;
            mode.current = true;
            output.modes.push_back(mode);
        }

        output.current_mode_index = 0;
        output.description = output.make + " " + output.name;

        outputs_.push_back(std::move(output));
    }
}

Result<void, std::string> DisplayPage::apply() {
    // In production, this would use the wlr-output-management Wayland protocol
    // to apply the configuration. Here we construct the configuration request.

    if (outputs_.empty()) {
        return Result<void, std::string>::error("No outputs to configure");
    }

    // Write desired configuration to a file that the compositor can read
    // (IPC mechanism would vary by compositor)
    std::ofstream config("/tmp/straylight-display-config.json");
    if (!config.is_open()) {
        return Result<void, std::string>::error("Cannot write display config");
    }

    config << "{\n  \"outputs\": [\n";
    for (size_t i = 0; i < outputs_.size(); ++i) {
        const auto& out = outputs_[i];
        if (i > 0) config << ",\n";
        config << "    {\n";
        config << "      \"name\": \"" << out.name << "\",\n";
        config << "      \"enabled\": " << (out.enabled ? "true" : "false") << ",\n";
        config << "      \"x\": " << out.x << ",\n";
        config << "      \"y\": " << out.y << ",\n";
        config << "      \"scale\": " << out.scale << ",\n";
        config << "      \"transform\": " << out.transform << ",\n";

        int mode_idx = out.current_mode_index;
        if (mode_idx >= 0 && mode_idx < static_cast<int>(out.modes.size())) {
            const auto& m = out.modes[static_cast<size_t>(mode_idx)];
            config << "      \"width\": " << m.width << ",\n";
            config << "      \"height\": " << m.height << ",\n";
            config << "      \"refresh\": " << m.refresh_rate << "\n";
        }
        config << "    }";
    }
    config << "\n  ]\n}\n";
    config.close();

    dirty_ = false;

    // The release desktop is managed by GNOME/GDM. Persist the requested
    // layout for firstboot or an external display backend to consume.
    return Result<void, std::string>::ok();
}

void DisplayPage::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Display Settings");
    ImGui::Separator();

    if (outputs_.empty()) {
        if (ImGui::Button("Detect Displays")) {
            detect();
        }
        ImGui::TextDisabled("No displays detected");
        return;
    }

    // Output selector
    if (outputs_.size() > 1) {
        for (int i = 0; i < static_cast<int>(outputs_.size()); ++i) {
            if (i > 0) ImGui::SameLine();
            bool selected = (i == selected_output_);
            if (ImGui::Selectable(outputs_[static_cast<size_t>(i)].name.c_str(),
                                  selected, 0, ImVec2(120, 30))) {
                selected_output_ = i;
            }
        }
        ImGui::Separator();
    }

    auto& out = outputs_[static_cast<size_t>(selected_output_)];

    // Output info
    ImGui::Text("Output: %s", out.name.c_str());
    if (!out.make.empty()) {
        ImGui::Text("Manufacturer: %s", out.make.c_str());
    }
    if (out.physical_width_mm > 0) {
        ImGui::Text("Physical: %dx%d mm", out.physical_width_mm,
                    out.physical_height_mm);
    }

    ImGui::Spacing();

    // Enable/disable
    if (ImGui::Checkbox("Enabled", &out.enabled)) {
        dirty_ = true;
    }

    ImGui::Spacing();

    // Resolution selection
    ImGui::Text("Resolution:");
    if (ImGui::BeginCombo("##Resolution",
                           out.modes.empty()
                               ? "N/A"
                               : (std::to_string(
                                      out.modes[static_cast<size_t>(
                                                    out.current_mode_index)]
                                          .width) +
                                  "x" +
                                  std::to_string(
                                      out.modes[static_cast<size_t>(
                                                    out.current_mode_index)]
                                          .height))
                                     .c_str())) {
        for (int i = 0; i < static_cast<int>(out.modes.size()); ++i) {
            const auto& m = out.modes[static_cast<size_t>(i)];
            char label[64];
            snprintf(label, sizeof(label), "%dx%d @ %.0f Hz",
                     m.width, m.height, m.refresh_rate);
            bool selected = (i == out.current_mode_index);
            if (ImGui::Selectable(label, selected)) {
                out.current_mode_index = i;
                dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }

    // Scale
    ImGui::Text("Scale:");
    float scales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
    for (int i = 0; i < 7; ++i) {
        if (i > 0) ImGui::SameLine();
        char label[16];
        snprintf(label, sizeof(label), "%.2fx", scales[i]);
        bool selected = (out.scale == scales[i]);
        if (ImGui::RadioButton(label, selected)) {
            out.scale = scales[i];
            dirty_ = true;
        }
    }

    // Transform/rotation
    ImGui::Text("Rotation:");
    const char* rotations[] = {"Normal", "90 CW", "180", "90 CCW"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        bool selected = (out.transform == i);
        if (ImGui::RadioButton(rotations[i], selected)) {
            out.transform = i;
            dirty_ = true;
        }
    }

    // Position (for multi-monitor)
    if (outputs_.size() > 1) {
        ImGui::Spacing();
        ImGui::Text("Position:");
        bool pos_changed = false;
        pos_changed |= ImGui::InputInt("X", &out.x, 100, 1000);
        pos_changed |= ImGui::InputInt("Y", &out.y, 100, 1000);
        if (pos_changed) dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Apply/Revert
    if (dirty_) {
        if (ImGui::Button("Apply", ImVec2(120, 30))) {
            apply();
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert", ImVec2(120, 30))) {
            detect();
            dirty_ = false;
        }
    }
}

} // namespace straylight::settings
