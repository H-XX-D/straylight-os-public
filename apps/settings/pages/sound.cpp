// apps/settings/pages/sound.cpp
// Sound settings — PulseAudio/PipeWire volume control
#include "sound.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace straylight::settings {

SoundPage::SoundPage() = default;

void SoundPage::load() {
    // Detect audio server
    FILE* pipe = popen("pactl info 2>/dev/null | grep 'Server Name'", "r");
    if (pipe) {
        char buf[256];
        if (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            if (line.find("PipeWire") != std::string::npos) {
                audio_server_ = "pipewire";
            } else {
                audio_server_ = "pulseaudio";
            }
        }
        pclose(pipe);
    }

    read_sinks();
    read_sources();
}

void SoundPage::read_sinks() {
    sinks_.clear();

    // Use pactl (works with both PulseAudio and PipeWire)
    FILE* pipe = popen(
        "pactl list sinks 2>/dev/null", "r");
    if (!pipe) return;

    char buf[512];
    AudioDevice current;
    bool in_sink = false;
    std::string default_sink;

    // First get default sink
    FILE* def_pipe = popen("pactl get-default-sink 2>/dev/null", "r");
    if (def_pipe) {
        char dbuf[256];
        if (fgets(dbuf, sizeof(dbuf), def_pipe)) {
            default_sink = dbuf;
            while (!default_sink.empty() &&
                   (default_sink.back() == '\n' || default_sink.back() == '\r'))
                default_sink.pop_back();
        }
        pclose(def_pipe);
    }

    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        // Remove trailing newline
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();

        if (line.find("Sink #") != std::string::npos) {
            if (in_sink && !current.name.empty()) {
                sinks_.push_back(current);
            }
            current = {};
            in_sink = true;
            sscanf(line.c_str(), " Sink #%d", &current.index);
        } else if (in_sink) {
            // Trim leading whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            std::string trimmed = line.substr(start);

            if (trimmed.compare(0, 5, "Name:") == 0) {
                current.name = trimmed.substr(6);
                current.is_default = (current.name == default_sink);
            } else if (trimmed.compare(0, 12, "Description:") == 0) {
                current.description = trimmed.substr(13);
            } else if (trimmed.compare(0, 5, "Mute:") == 0) {
                current.muted = (trimmed.find("yes") != std::string::npos);
            } else if (trimmed.compare(0, 7, "Volume:") == 0) {
                // Parse volume percentage
                auto pct_pos = trimmed.find('%');
                if (pct_pos != std::string::npos) {
                    // Find the number before %
                    auto space = trimmed.rfind(' ', pct_pos);
                    if (space != std::string::npos) {
                        std::string vol_str = trimmed.substr(space + 1,
                                                              pct_pos - space - 1);
                        try {
                            int pct = std::stoi(vol_str);
                            current.volume = static_cast<float>(pct) / 100.0f;
                        } catch (...) {}
                    }
                }
            } else if (trimmed.compare(0, 13, "Sample Specification:") == 0) {
                // Parse "s16le 2ch 44100Hz"
                int rate = 0, channels = 0;
                if (sscanf(trimmed.c_str() + 22, "%*s %dch %dHz",
                           &channels, &rate) >= 2) {
                    current.channels = channels;
                    current.sample_rate = rate;
                }
            }
        }
    }

    if (in_sink && !current.name.empty()) {
        sinks_.push_back(current);
    }

    pclose(pipe);
}

void SoundPage::read_sources() {
    sources_.clear();

    FILE* pipe = popen("pactl list sources short 2>/dev/null", "r");
    if (!pipe) return;

    std::string default_source;
    FILE* def_pipe = popen("pactl get-default-source 2>/dev/null", "r");
    if (def_pipe) {
        char dbuf[256];
        if (fgets(dbuf, sizeof(dbuf), def_pipe)) {
            default_source = dbuf;
            while (!default_source.empty() &&
                   (default_source.back() == '\n' ||
                    default_source.back() == '\r'))
                default_source.pop_back();
        }
        pclose(def_pipe);
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        AudioSource src;
        char name[256] = {};
        char driver[64] = {};

        if (sscanf(buf, "%d %255s %*s %*s %63s",
                   &src.index, name, driver) >= 2) {
            src.name = name;
            // Skip monitor sources
            if (std::string(name).find(".monitor") != std::string::npos) {
                continue;
            }
            src.description = name;
            src.is_default = (src.name == default_source);
            sources_.push_back(std::move(src));
        }
    }

    pclose(pipe);
}

Result<void, std::string> SoundPage::set_volume(int sink_index, float volume) {
    int pct = static_cast<int>(volume * 100.0f);
    pct = std::clamp(pct, 0, 150);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pactl set-sink-volume %d %d%% 2>&1",
             sink_index, pct);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to set volume");
    }

    char buf[256];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    int status = pclose(pipe);

    if (status != 0) {
        return Result<void, std::string>::error("pactl error: " + output);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> SoundPage::set_mute(int sink_index, bool muted) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pactl set-sink-mute %d %s 2>&1",
             sink_index, muted ? "1" : "0");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to set mute");
    }
    pclose(pipe);

    return Result<void, std::string>::ok();
}

Result<void, std::string> SoundPage::set_default_sink(const std::string& name) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pactl set-default-sink '%s' 2>&1", name.c_str());

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to set default sink");
    }
    pclose(pipe);

    // Refresh
    read_sinks();

    return Result<void, std::string>::ok();
}

void SoundPage::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Sound Settings");
    ImGui::Text("Audio Server: %s",
                audio_server_.empty() ? "Unknown" : audio_server_.c_str());
    ImGui::Separator();

    // === Output Devices ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Output Devices");

    if (sinks_.empty()) {
        ImGui::TextDisabled("No output devices found");
        if (ImGui::Button("Refresh")) {
            load();
        }
    }

    for (auto& sink : sinks_) {
        ImGui::PushID(sink.index);

        // Default indicator
        if (sink.is_default) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "*");
            ImGui::SameLine();
        }

        // Device name
        std::string label = sink.description.empty() ? sink.name : sink.description;
        if (ImGui::CollapsingHeader(label.c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            // Volume slider
            float vol = sink.volume;
            ImGui::Text("Volume:");
            ImGui::SameLine();

            // Mute button
            if (ImGui::Button(sink.muted ? "Unmute" : "Mute")) {
                sink.muted = !sink.muted;
                set_mute(sink.index, sink.muted);
            }
            ImGui::SameLine();

            if (sink.muted) {
                ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                                      ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            }

            char vol_label[32];
            snprintf(vol_label, sizeof(vol_label), "%.0f%%", vol * 100.0f);
            if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.5f, vol_label)) {
                sink.volume = vol;
                set_volume(sink.index, vol);
            }

            if (sink.muted) {
                ImGui::PopStyleColor();
            }

            // Set as default
            if (!sink.is_default) {
                if (ImGui::SmallButton("Set as Default")) {
                    set_default_sink(sink.name);
                }
            }

            // Info
            ImGui::TextDisabled("Channels: %d | Sample Rate: %d Hz",
                               sink.channels, sink.sample_rate);
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Input Devices ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Input Devices");

    if (sources_.empty()) {
        ImGui::TextDisabled("No input devices found");
    }

    for (const auto& src : sources_) {
        ImGui::PushID(src.index + 1000);

        if (src.is_default) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "*");
            ImGui::SameLine();
        }

        ImGui::Text("%s", src.description.c_str());

        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Refresh Audio Devices")) {
        load();
    }
}

} // namespace straylight::settings
