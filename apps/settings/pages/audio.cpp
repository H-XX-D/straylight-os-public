// apps/settings/pages/audio.cpp
// Audio settings via PipeWire / WirePlumber (wpctl)
#include "audio.h"

#include <imgui.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace straylight::settings {

// ---------------------------------------------------------------------------
// Helpers: run a command and return stdout
// ---------------------------------------------------------------------------

namespace {

std::string run_command(const char* cmd) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return {};

    std::string output;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
    pclose(pipe);
    return output;
}

bool is_audio_tree_line(const std::string& line) {
    if (line.empty()) return false;
    const auto first = static_cast<unsigned char>(line[0]);
    return first == ' ' || first == '\t' || first == '|' || first == 0xe2;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// load helpers
// ---------------------------------------------------------------------------

void AudioPage::load_output_volume() {
    std::string out =
        run_command("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    if (out.empty()) return;

    float v = 0.0f;
    if (sscanf(out.c_str(), "Volume: %f", &v) == 1) {
        output_volume_ = v;
    }
    output_muted_ = (out.find("[MUTED]") != std::string::npos);
}

void AudioPage::load_input_volume() {
    std::string out =
        run_command("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null");
    if (out.empty()) return;

    float v = 0.0f;
    if (sscanf(out.c_str(), "Volume: %f", &v) == 1) {
        input_volume_ = v;
    }
    input_muted_ = (out.find("[MUTED]") != std::string::npos);
}

void AudioPage::load_sinks() {
    sinks_.clear();

    // Use wpctl status to list audio sinks.
    // wpctl status output (simplified):
    //   Audio
    //    ├─ Sinks:
    //   │   ├─ * 47. Speakers [vol: 0.75]
    //   │   └─   48. HDMI Output [vol: 0.40]
    std::string raw = run_command("wpctl status 2>/dev/null");
    if (raw.empty()) {
        // Fallback: pw-cli list-objects to get node names
        raw = run_command(
            "pw-cli list-objects Node 2>/dev/null | "
            "grep -A5 'media.class.*Audio/Sink'");
    }

    if (raw.empty()) {
        // No PipeWire available — provide a placeholder
        AudioSink s;
        s.id          = 0;
        s.name        = "default";
        s.description = "Default Audio Output";
        s.is_default  = true;
        sinks_.push_back(std::move(s));
        return;
    }

    // Parse the "Sinks:" section from wpctl status
    bool in_sinks = false;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("Sinks:") != std::string::npos) {
            in_sinks = true;
            continue;
        }
        if (in_sinks) {
            // Stop at next section header (line with no leading whitespace or
            // a different section keyword)
            if (!is_audio_tree_line(line)) {
                break;
            }

            // Look for lines containing a node ID number
            // Format: "   * <id>. <description>" or "     <id>. <description>"
            uint32_t id = 0;
            char desc[256] = {};
            bool is_default = (line.find('*') != std::string::npos);

            // Find the numeric ID after optional '*' and whitespace
            const char* p = line.c_str();
            while (*p && (*p == ' ' || *p == '\t' || *p == '*' ||
                          *p == '|' || *p == (char)0xe2)) {
                ++p;
            }

            if (sscanf(p, "%u. %255[^\n]", &id, desc) >= 2) {
                // Trim trailing volume annotation like " [vol: 0.75]"
                std::string d = desc;
                auto bracket = d.find(" [vol:");
                if (bracket != std::string::npos) {
                    d = d.substr(0, bracket);
                }

                AudioSink sink;
                sink.id          = id;
                sink.name        = d;
                sink.description = d;
                sink.is_default  = is_default;
                sinks_.push_back(std::move(sink));
            }
        }
        // Reset section flag when we hit "Sources:" heading
        if (in_sinks && line.find("Sources:") != std::string::npos) {
            break;
        }
    }

    // If nothing parsed, add placeholder
    if (sinks_.empty()) {
        AudioSink s;
        s.id          = 0;
        s.name        = "default";
        s.description = "Default Audio Output";
        s.is_default  = true;
        sinks_.push_back(std::move(s));
    }

    // Set selected_sink_ to the default
    for (int i = 0; i < static_cast<int>(sinks_.size()); ++i) {
        if (sinks_[static_cast<size_t>(i)].is_default) {
            selected_sink_ = i;
            break;
        }
    }
}

void AudioPage::load_sources() {
    sources_.clear();

    std::string raw = run_command("wpctl status 2>/dev/null");
    if (raw.empty()) {
        AudioSource src;
        src.id          = 0;
        src.name        = "default";
        src.description = "Default Audio Input";
        src.is_default  = true;
        sources_.push_back(std::move(src));
        return;
    }

    bool in_sources = false;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("Sources:") != std::string::npos) {
            in_sources = true;
            continue;
        }
        if (in_sources) {
            if (!is_audio_tree_line(line)) {
                break;
            }

            uint32_t id = 0;
            char desc[256] = {};
            bool is_default = (line.find('*') != std::string::npos);

            const char* p = line.c_str();
            while (*p && (*p == ' ' || *p == '\t' || *p == '*' ||
                          *p == '|' || *p == (char)0xe2)) {
                ++p;
            }

            if (sscanf(p, "%u. %255[^\n]", &id, desc) >= 2) {
                std::string d = desc;
                auto bracket = d.find(" [vol:");
                if (bracket != std::string::npos) d = d.substr(0, bracket);

                AudioSource src;
                src.id          = id;
                src.name        = d;
                src.description = d;
                src.is_default  = is_default;
                sources_.push_back(std::move(src));
            }
        }
    }

    if (sources_.empty()) {
        AudioSource src;
        src.id = 0; src.name = "default";
        src.description = "Default Audio Input"; src.is_default = true;
        sources_.push_back(std::move(src));
    }

    for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
        if (sources_[static_cast<size_t>(i)].is_default) {
            selected_source_ = i;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------

void AudioPage::load() {
    load_sinks();
    load_sources();
    load_output_volume();
    load_input_volume();
    status_msg_.clear();
}

// ---------------------------------------------------------------------------
// Control operations
// ---------------------------------------------------------------------------

Result<void, std::string> AudioPage::set_output_volume(float vol) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f 2>/dev/null", vol);
    if (std::system(cmd) != 0) {
        return Result<void, std::string>::error("wpctl set-volume failed");
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioPage::set_input_volume(float vol) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "wpctl set-volume @DEFAULT_AUDIO_SOURCE@ %.2f 2>/dev/null", vol);
    if (std::system(cmd) != 0) {
        return Result<void, std::string>::error("wpctl set-volume (input) failed");
    }
    return Result<void, std::string>::ok();
}

void AudioPage::set_output_mute(bool mute) {
    const char* cmd = mute
        ? "wpctl set-mute @DEFAULT_AUDIO_SINK@ 1 2>/dev/null"
        : "wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 2>/dev/null";
    if (std::system(cmd) != 0) {
        return;
    }
}

void AudioPage::set_input_mute(bool mute) {
    const char* cmd = mute
        ? "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 1 2>/dev/null"
        : "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0 2>/dev/null";
    if (std::system(cmd) != 0) {
        return;
    }
}

Result<void, std::string> AudioPage::set_default_sink(uint32_t id) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %u 2>/dev/null", id);
    if (std::system(cmd) != 0) {
        return Result<void, std::string>::error("wpctl set-default failed");
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioPage::set_default_source(uint32_t id) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %u 2>/dev/null", id);
    if (std::system(cmd) != 0) {
        return Result<void, std::string>::error("wpctl set-default (source) failed");
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// render()
// ---------------------------------------------------------------------------

void AudioPage::render() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("Audio");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // --- Output Volume --------------------------------------------------
    ImGui::SeparatorText("Output Volume");

    // Volume slider 0..150%
    float display_vol = output_volume_ * 100.0f;
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::SliderFloat("##outvol", &display_vol, 0.0f, 150.0f, "%.0f%%")) {
        output_volume_ = display_vol / 100.0f;
        auto r = set_output_volume(output_volume_);
        if (!r.has_value()) {
            status_msg_ = r.error();
        }
    }
    ImGui::SameLine();

    // Mute toggle
    bool prev_muted = output_muted_;
    if (ImGui::Checkbox("Mute Output", &output_muted_)) {
        set_output_mute(output_muted_);
    }

    // Volume bar indicator
    ImVec4 vol_color = output_muted_
        ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
        : (output_volume_ > 1.0f
               ? ImVec4(1.0f, 0.5f, 0.0f, 1.0f)
               : ImVec4(0.0f, 0.8f, 0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vol_color);
    char vol_overlay[32];
    snprintf(vol_overlay, sizeof(vol_overlay), "%.0f%%", display_vol);
    ImGui::ProgressBar(output_volume_ / 1.5f, ImVec2(300.0f, 12.0f), vol_overlay);
    ImGui::PopStyleColor();

    (void)prev_muted;

    ImGui::Spacing();

    // --- Output Device Selection ----------------------------------------
    ImGui::SeparatorText("Output Device");

    if (!sinks_.empty()) {
        const auto& current_sink = sinks_[static_cast<size_t>(selected_sink_)];
        ImGui::SetNextItemWidth(400.0f);
        if (ImGui::BeginCombo("##outsink", current_sink.description.c_str())) {
            for (int i = 0; i < static_cast<int>(sinks_.size()); ++i) {
                const auto& s = sinks_[static_cast<size_t>(i)];
                bool selected = (i == selected_sink_);
                if (ImGui::Selectable(s.description.c_str(), selected)) {
                    selected_sink_ = i;
                    auto r = set_default_sink(s.id);
                    if (r.has_value()) {
                        status_msg_ = "Output device changed to: " + s.description;
                    } else {
                        status_msg_ = r.error();
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##sinks")) {
            load();
        }
    } else {
        ImGui::TextDisabled("No audio output devices found");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) load();
    }

    ImGui::Spacing();

    // --- Input Volume ---------------------------------------------------
    ImGui::SeparatorText("Input Volume (Microphone)");

    float display_in_vol = input_volume_ * 100.0f;
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::SliderFloat("##invol", &display_in_vol, 0.0f, 150.0f, "%.0f%%")) {
        input_volume_ = display_in_vol / 100.0f;
        auto r = set_input_volume(input_volume_);
        if (!r.has_value()) {
            status_msg_ = r.error();
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Mute Input", &input_muted_)) {
        set_input_mute(input_muted_);
    }

    ImGui::Spacing();

    // --- Input Device Selection -----------------------------------------
    ImGui::SeparatorText("Input Device");

    if (!sources_.empty()) {
        const auto& current_src = sources_[static_cast<size_t>(selected_source_)];
        ImGui::SetNextItemWidth(400.0f);
        if (ImGui::BeginCombo("##insource", current_src.description.c_str())) {
            for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
                const auto& s = sources_[static_cast<size_t>(i)];
                bool selected = (i == selected_source_);
                if (ImGui::Selectable(s.description.c_str(), selected)) {
                    selected_source_ = i;
                    auto r = set_default_source(s.id);
                    if (r.has_value()) {
                        status_msg_ = "Input device changed to: " + s.description;
                    } else {
                        status_msg_ = r.error();
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("No audio input devices found");
    }

    ImGui::Spacing();

    // --- PipeWire Server Info -------------------------------------------
    ImGui::SeparatorText("PipeWire Status");
    {
        std::string pw_info = run_command("pw-cli info 0 2>/dev/null | head -5");
        if (!pw_info.empty()) {
            ImGui::TextDisabled("%s", pw_info.c_str());
        } else {
            ImGui::TextDisabled("PipeWire: not running or not installed");
        }
    }

    // --- Status ---------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    if (!status_msg_.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f),
                           "%s", status_msg_.c_str());
    }
}

} // namespace straylight::settings
