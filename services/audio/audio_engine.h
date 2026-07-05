// services/audio/audio_engine.h
// PipeWire-based audio routing engine for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Represents an audio device (sink or source).
struct AudioDevice {
    uint32_t id = 0;
    std::string name;
    std::string description;
    std::string driver;       // "pipewire", "alsa", "bluetooth"
    enum class Type { Sink, Source } type = Type::Sink;
    bool is_default = false;
    bool is_monitor = false;
    int channels = 2;
    int sample_rate = 48000;
    float volume = 1.0f;      // 0.0–1.0+
    bool muted = false;
    std::string profile;      // active card profile
    std::vector<std::string> available_profiles;
};

/// Represents a per-application audio stream.
struct AudioStream {
    uint32_t id = 0;
    std::string app_name;
    std::string binary;        // process binary name
    pid_t pid = 0;
    float volume = 1.0f;
    bool muted = false;
    uint32_t device_id = 0;   // sink/source it's connected to
    std::string device_name;
    int channels = 2;
    int sample_rate = 48000;
    bool is_input = false;     // true = recording, false = playback
    std::string media_name;    // e.g. "Firefox — Music"
};

/// Loopback route between devices.
struct AudioLoopback {
    uint32_t id = 0;
    uint32_t from_device = 0;
    uint32_t to_device = 0;
    std::string from_name;
    std::string to_name;
    float volume = 1.0f;
};

/// Audio equalizer preset.
struct EQPreset {
    std::string name;
    struct Band {
        double frequency_hz = 0;
        double gain_db = 0;
        double q = 1.0;
    };
    std::vector<Band> bands;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    /// Connect to PipeWire daemon and enumerate initial state.
    Result<void, std::string> init();

    /// Refresh device and stream lists from PipeWire state.
    Result<void, std::string> refresh();

    /// List all audio devices (sinks and sources).
    Result<std::vector<AudioDevice>, std::string> list_devices() const;

    /// List all active audio streams.
    Result<std::vector<AudioStream>, std::string> list_streams() const;

    /// Set volume for a specific stream.
    Result<void, std::string> set_volume(uint32_t stream_id, float level);

    /// Set volume for a specific device.
    Result<void, std::string> set_device_volume(uint32_t device_id, float level);

    /// Mute/unmute a stream.
    Result<void, std::string> set_mute(uint32_t stream_id, bool muted);

    /// Mute/unmute a device.
    Result<void, std::string> set_device_mute(uint32_t device_id, bool muted);

    /// Move a stream to a different device.
    Result<void, std::string> set_device(uint32_t stream_id, uint32_t device_id);

    /// Set default sink or source.
    Result<void, std::string> set_default(AudioDevice::Type type, uint32_t device_id);

    /// Create an audio loopback route between two devices.
    Result<AudioLoopback, std::string> create_loopback(uint32_t from_device, uint32_t to_device);

    /// Remove a loopback route.
    Result<void, std::string> remove_loopback(uint32_t loopback_id);

    /// List active loopback routes.
    std::vector<AudioLoopback> list_loopbacks() const;

    /// Set card profile (e.g. "a2dp_sink", "headset_head_unit").
    Result<void, std::string> set_card_profile(uint32_t device_id, const std::string& profile);

    /// Apply an EQ preset.
    Result<void, std::string> apply_eq(const EQPreset& preset);

private:
    /// Run a PipeWire command and capture output.
    Result<std::string, std::string> run_pw_cmd(const std::string& cmd) const;

    /// Run a PulseAudio command (pactl) for compatibility.
    Result<std::string, std::string> run_pa_cmd(const std::string& cmd) const;

    /// Run a generic shell command.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Parse pw-dump JSON output for devices.
    std::vector<AudioDevice> parse_pw_devices(const std::string& json) const;

    /// Parse pw-dump JSON output for streams.
    std::vector<AudioStream> parse_pw_streams(const std::string& json) const;

    /// Parse /proc/asound for ALSA device info.
    std::vector<AudioDevice> parse_alsa_devices() const;

    /// Cached state.
    std::vector<AudioDevice> devices_;
    std::vector<AudioStream> streams_;
    std::vector<AudioLoopback> loopbacks_;
    uint32_t next_loopback_id_ = 1;
    bool initialized_ = false;
};

} // namespace straylight
