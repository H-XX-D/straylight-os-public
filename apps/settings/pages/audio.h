// apps/settings/pages/audio.h
// Audio settings: PipeWire volume control and device selection
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::settings {

/// A single PipeWire sink (output device).
struct AudioSink {
    uint32_t    id        = 0;
    std::string name;         // Node name
    std::string description;  // Human-friendly label
    bool        is_default = false;
};

/// A single PipeWire source (input device).
struct AudioSource {
    uint32_t    id        = 0;
    std::string name;
    std::string description;
    bool        is_default = false;
};

/// Audio settings page — PipeWire volume, device selection, and mute.
/// Uses wpctl (WirePlumber CLI) for all control operations.
class AudioPage : public SettingsPage {
public:
    AudioPage()  = default;
    ~AudioPage() = default;

    [[nodiscard]] const char* label() const override { return "Audio"; }

    /// Query PipeWire sinks/sources and current volume via wpctl.
    void load() override;

    /// Render the audio settings panel.
    void render() override;

private:
    /// Enumerate audio sinks via wpctl.
    void load_sinks();

    /// Enumerate audio sources via wpctl.
    void load_sources();

    /// Read current output volume via "wpctl get-volume @DEFAULT_AUDIO_SINK@".
    void load_output_volume();

    /// Read current input volume via "wpctl get-volume @DEFAULT_AUDIO_SOURCE@".
    void load_input_volume();

    /// Set output volume (0.0–1.5). Calls "wpctl set-volume".
    Result<void, std::string> set_output_volume(float vol);

    /// Set input volume (0.0–1.5). Calls "wpctl set-volume".
    Result<void, std::string> set_input_volume(float vol);

    /// Mute/unmute default output.
    void set_output_mute(bool mute);

    /// Mute/unmute default input.
    void set_input_mute(bool mute);

    /// Set default sink by PipeWire node ID.
    Result<void, std::string> set_default_sink(uint32_t id);

    /// Set default source by PipeWire node ID.
    Result<void, std::string> set_default_source(uint32_t id);

    std::vector<AudioSink>   sinks_;
    std::vector<AudioSource> sources_;

    int   selected_sink_   = 0;
    int   selected_source_ = 0;

    float output_volume_   = 0.75f;  // 0.0–1.5 (> 1.0 = amplified)
    float input_volume_    = 0.75f;
    bool  output_muted_    = false;
    bool  input_muted_     = false;

    std::string status_msg_;
};

} // namespace straylight::settings
