// apps/settings/pages/sound.h
// Sound settings — PulseAudio/PipeWire volume control, device selection
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::settings {

/// Audio output device.
struct AudioDevice {
    int index = 0;
    std::string name;
    std::string description;
    float volume = 1.0f;  // 0.0..1.5 (allows boost)
    bool muted = false;
    bool is_default = false;
    int channels = 2;
    int sample_rate = 44100;
    std::string driver; // "pulseaudio" or "pipewire"
};

/// Audio input (source) device.
struct AudioSource {
    int index = 0;
    std::string name;
    std::string description;
    float volume = 1.0f;
    bool muted = false;
    bool is_default = false;
};

/// Sound settings page.
class SoundPage : public SettingsPage {
public:
    SoundPage();

    [[nodiscard]] const char* label() const override { return "Sound"; }

    /// Load current audio state.
    void load() override;

    /// Render the sound settings page in ImGui.
    void render() override;

    /// Set volume for a sink.
    Result<void, std::string> set_volume(int sink_index, float volume);

    /// Set mute state.
    Result<void, std::string> set_mute(int sink_index, bool muted);

    /// Set default output device.
    Result<void, std::string> set_default_sink(const std::string& name);

private:
    void read_sinks();
    void read_sources();

    std::vector<AudioDevice> sinks_;
    std::vector<AudioSource> sources_;
    std::string audio_server_; // "pulseaudio" or "pipewire"
};

} // namespace straylight::settings
