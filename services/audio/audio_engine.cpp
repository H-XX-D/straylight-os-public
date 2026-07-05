// services/audio/audio_engine.cpp
// Full PipeWire/PulseAudio audio routing engine implementation.

#include "audio_engine.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() = default;

Result<std::string, std::string> AudioEngine::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
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
            "command failed (rc=" + std::to_string(rc) + "): " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

Result<std::string, std::string> AudioEngine::run_pw_cmd(const std::string& cmd) const {
    return run_cmd("pw-cli " + cmd + " 2>/dev/null");
}

Result<std::string, std::string> AudioEngine::run_pa_cmd(const std::string& cmd) const {
    return run_cmd("pactl " + cmd + " 2>/dev/null");
}

// ---------------------------------------------------------------------------
// init / refresh
// ---------------------------------------------------------------------------

Result<void, std::string> AudioEngine::init() {
    // Verify PipeWire is running
    auto res = run_cmd("pw-cli info 0 2>/dev/null");
    if (!res.has_value()) {
        // Try PulseAudio fallback
        auto pa_res = run_pa_cmd("info");
        if (!pa_res.has_value()) {
            return Result<void, std::string>::error(
                "neither PipeWire nor PulseAudio is running");
        }
    }

    initialized_ = true;
    return refresh();
}

Result<void, std::string> AudioEngine::refresh() {
    if (!initialized_) {
        return Result<void, std::string>::error("engine not initialized");
    }

    // Refresh devices
    devices_.clear();

    // Try PipeWire first
    auto pw_res = run_cmd("pw-dump 2>/dev/null");
    if (pw_res.has_value() && !pw_res.value().empty()) {
        devices_ = parse_pw_devices(pw_res.value());
        streams_ = parse_pw_streams(pw_res.value());
    } else {
        // PulseAudio fallback for devices
        auto sinks = run_pa_cmd("list sinks");
        if (sinks.has_value()) {
            std::istringstream stream(sinks.value());
            std::string line;
            AudioDevice current;
            bool in_sink = false;

            while (std::getline(stream, line)) {
                // Trim
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Sink #", 0) == 0) {
                    if (in_sink && current.id != 0) {
                        current.type = AudioDevice::Type::Sink;
                        devices_.push_back(current);
                    }
                    current = AudioDevice{};
                    current.id = std::stoul(line.substr(6));
                    in_sink = true;
                } else if (in_sink) {
                    if (line.rfind("Name:", 0) == 0) {
                        current.name = line.substr(6);
                    } else if (line.rfind("Description:", 0) == 0) {
                        current.description = line.substr(13);
                    } else if (line.rfind("Driver:", 0) == 0) {
                        current.driver = line.substr(8);
                    } else if (line.rfind("Mute:", 0) == 0) {
                        current.muted = (line.find("yes") != std::string::npos);
                    } else if (line.rfind("Volume:", 0) == 0) {
                        // Parse "Volume: front-left: 65536 / 100% / 0.00 dB ..."
                        std::regex vol_re(R"((\d+)%)");
                        std::smatch m;
                        if (std::regex_search(line, m, vol_re)) {
                            current.volume = std::stof(m[1].str()) / 100.0f;
                        }
                    } else if (line.rfind("Sample Specification:", 0) == 0) {
                        // "s16le 2ch 48000Hz"
                        std::regex sr_re(R"((\d+)Hz)");
                        std::regex ch_re(R"((\d+)ch)");
                        std::smatch m;
                        if (std::regex_search(line, m, sr_re)) {
                            current.sample_rate = std::stoi(m[1].str());
                        }
                        if (std::regex_search(line, m, ch_re)) {
                            current.channels = std::stoi(m[1].str());
                        }
                    }
                }
            }
            if (in_sink && current.id != 0) {
                current.type = AudioDevice::Type::Sink;
                devices_.push_back(current);
            }
        }

        // Sources
        auto sources = run_pa_cmd("list sources");
        if (sources.has_value()) {
            std::istringstream stream(sources.value());
            std::string line;
            AudioDevice current;
            bool in_source = false;

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Source #", 0) == 0) {
                    if (in_source && current.id != 0) {
                        current.type = AudioDevice::Type::Source;
                        devices_.push_back(current);
                    }
                    current = AudioDevice{};
                    current.id = std::stoul(line.substr(8));
                    in_source = true;
                } else if (in_source) {
                    if (line.rfind("Name:", 0) == 0) {
                        current.name = line.substr(6);
                        current.is_monitor = (current.name.find(".monitor") != std::string::npos);
                    } else if (line.rfind("Description:", 0) == 0) {
                        current.description = line.substr(13);
                    } else if (line.rfind("Driver:", 0) == 0) {
                        current.driver = line.substr(8);
                    } else if (line.rfind("Mute:", 0) == 0) {
                        current.muted = (line.find("yes") != std::string::npos);
                    } else if (line.rfind("Volume:", 0) == 0) {
                        std::regex vol_re(R"((\d+)%)");
                        std::smatch m;
                        if (std::regex_search(line, m, vol_re)) {
                            current.volume = std::stof(m[1].str()) / 100.0f;
                        }
                    }
                }
            }
            if (in_source && current.id != 0) {
                current.type = AudioDevice::Type::Source;
                devices_.push_back(current);
            }
        }

        // Mark defaults
        auto server_info = run_pa_cmd("info");
        if (server_info.has_value()) {
            std::string default_sink, default_source;
            std::istringstream stream(server_info.value());
            std::string line;
            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);
                if (line.rfind("Default Sink:", 0) == 0) {
                    default_sink = line.substr(14);
                } else if (line.rfind("Default Source:", 0) == 0) {
                    default_source = line.substr(16);
                }
            }
            for (auto& dev : devices_) {
                if (dev.type == AudioDevice::Type::Sink && dev.name == default_sink) {
                    dev.is_default = true;
                } else if (dev.type == AudioDevice::Type::Source && dev.name == default_source) {
                    dev.is_default = true;
                }
            }
        }

        // Streams via pactl list sink-inputs / source-outputs
        streams_.clear();
        auto sink_inputs = run_pa_cmd("list sink-inputs");
        if (sink_inputs.has_value()) {
            std::istringstream stream(sink_inputs.value());
            std::string line;
            AudioStream current;
            bool in_stream = false;

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Sink Input #", 0) == 0) {
                    if (in_stream && current.id != 0) {
                        current.is_input = false;
                        streams_.push_back(current);
                    }
                    current = AudioStream{};
                    current.id = std::stoul(line.substr(12));
                    in_stream = true;
                } else if (in_stream) {
                    if (line.rfind("Sink:", 0) == 0) {
                        current.device_id = std::stoul(line.substr(6));
                    } else if (line.rfind("Volume:", 0) == 0) {
                        std::regex vol_re(R"((\d+)%)");
                        std::smatch m;
                        if (std::regex_search(line, m, vol_re)) {
                            current.volume = std::stof(m[1].str()) / 100.0f;
                        }
                    } else if (line.rfind("Mute:", 0) == 0) {
                        current.muted = (line.find("yes") != std::string::npos);
                    } else if (line.find("application.name") != std::string::npos) {
                        std::regex name_re(R"rx(application\.name\s*=\s*"([^"]*)")rx");
                        std::smatch m;
                        if (std::regex_search(line, m, name_re)) {
                            current.app_name = m[1].str();
                        }
                    } else if (line.find("application.process.binary") != std::string::npos) {
                        std::regex bin_re(R"rx(application\.process\.binary\s*=\s*"([^"]*)")rx");
                        std::smatch m;
                        if (std::regex_search(line, m, bin_re)) {
                            current.binary = m[1].str();
                        }
                    } else if (line.find("application.process.id") != std::string::npos) {
                        std::regex pid_re(R"rx(application\.process\.id\s*=\s*"(\d+)")rx");
                        std::smatch m;
                        if (std::regex_search(line, m, pid_re)) {
                            current.pid = std::stoi(m[1].str());
                        }
                    } else if (line.find("media.name") != std::string::npos) {
                        std::regex media_re(R"rx(media\.name\s*=\s*"([^"]*)")rx");
                        std::smatch m;
                        if (std::regex_search(line, m, media_re)) {
                            current.media_name = m[1].str();
                        }
                    }
                }
            }
            if (in_stream && current.id != 0) {
                current.is_input = false;
                streams_.push_back(current);
            }
        }
    }

    // Supplement with ALSA info
    auto alsa_devices = parse_alsa_devices();
    for (const auto& alsa : alsa_devices) {
        bool found = false;
        for (auto& dev : devices_) {
            if (dev.name.find(alsa.name) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) {
            devices_.push_back(alsa);
        }
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// PipeWire JSON parsing (simplified)
// ---------------------------------------------------------------------------

std::vector<AudioDevice> AudioEngine::parse_pw_devices(const std::string& json) const {
    std::vector<AudioDevice> devices;

    // Parse pw-dump output for Audio/Sink and Audio/Source nodes
    // Each object has "type": "PipeWire:Interface:Node" with "info.props"
    std::regex node_re(R"("id"\s*:\s*(\d+))");
    std::regex class_re(R"rx("media\.class"\s*:\s*"([^"]*)")rx");
    std::regex name_re(R"rx("node\.name"\s*:\s*"([^"]*)")rx");
    std::regex desc_re(R"rx("node\.description"\s*:\s*"([^"]*)")rx");
    std::regex nick_re(R"rx("node\.nick"\s*:\s*"([^"]*)")rx");

    // Split JSON into object blocks (simplified)
    size_t pos = 0;
    while (true) {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < json.size() && depth > 0) {
            if (json[obj_end] == '{') ++depth;
            else if (json[obj_end] == '}') --depth;
            ++obj_end;
        }

        std::string obj = json.substr(obj_start, obj_end - obj_start);

        // Check if this is an audio node
        std::smatch m;
        std::string media_class;
        if (std::regex_search(obj, m, class_re)) {
            media_class = m[1].str();
        }

        if (media_class == "Audio/Sink" || media_class == "Audio/Source") {
            AudioDevice dev;
            dev.type = (media_class == "Audio/Sink") ? AudioDevice::Type::Sink
                                                      : AudioDevice::Type::Source;
            if (std::regex_search(obj, m, node_re)) dev.id = std::stoul(m[1].str());
            if (std::regex_search(obj, m, name_re)) dev.name = m[1].str();
            if (std::regex_search(obj, m, desc_re)) dev.description = m[1].str();
            else if (std::regex_search(obj, m, nick_re)) dev.description = m[1].str();

            dev.driver = "pipewire";
            dev.is_monitor = (dev.name.find(".monitor") != std::string::npos);

            devices.push_back(dev);
        }

        pos = obj_end;
    }

    return devices;
}

std::vector<AudioStream> AudioEngine::parse_pw_streams(const std::string& json) const {
    std::vector<AudioStream> streams;

    std::regex class_re(R"rx("media\.class"\s*:\s*"([^"]*)")rx");
    std::regex id_re(R"("id"\s*:\s*(\d+))");
    std::regex app_re(R"rx("application\.name"\s*:\s*"([^"]*)")rx");
    std::regex bin_re(R"rx("application\.process\.binary"\s*:\s*"([^"]*)")rx");
    std::regex pid_re(R"rx("application\.process\.id"\s*:\s*"(\d+)")rx");
    std::regex media_re(R"rx("media\.name"\s*:\s*"([^"]*)")rx");

    size_t pos = 0;
    while (true) {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < json.size() && depth > 0) {
            if (json[obj_end] == '{') ++depth;
            else if (json[obj_end] == '}') --depth;
            ++obj_end;
        }

        std::string obj = json.substr(obj_start, obj_end - obj_start);

        std::smatch m;
        std::string media_class;
        if (std::regex_search(obj, m, class_re)) media_class = m[1].str();

        if (media_class == "Stream/Output/Audio" || media_class == "Stream/Input/Audio") {
            AudioStream s;
            s.is_input = (media_class == "Stream/Input/Audio");
            if (std::regex_search(obj, m, id_re)) s.id = std::stoul(m[1].str());
            if (std::regex_search(obj, m, app_re)) s.app_name = m[1].str();
            if (std::regex_search(obj, m, bin_re)) s.binary = m[1].str();
            if (std::regex_search(obj, m, pid_re)) s.pid = std::stoi(m[1].str());
            if (std::regex_search(obj, m, media_re)) s.media_name = m[1].str();
            streams.push_back(s);
        }

        pos = obj_end;
    }

    return streams;
}

std::vector<AudioDevice> AudioEngine::parse_alsa_devices() const {
    std::vector<AudioDevice> devices;
    std::string alsa_dir = "/proc/asound";

    if (!fs::exists(alsa_dir)) return devices;

    // Read /proc/asound/cards
    std::ifstream cards(alsa_dir + "/cards");
    if (!cards.is_open()) return devices;

    std::string line;
    while (std::getline(cards, line)) {
        // Lines like: " 0 [PCH            ]: HDA-Intel - HDA Intel PCH"
        std::regex card_re(R"(\s*(\d+)\s+\[(\S+)\s*\]:\s*(.+))");
        std::smatch m;
        if (std::regex_match(line, m, card_re)) {
            AudioDevice dev;
            dev.id = 10000 + std::stoul(m[1].str()); // offset to avoid ID conflicts
            dev.name = "alsa:" + m[2].str();
            dev.description = m[3].str();
            dev.driver = "alsa";
            dev.type = AudioDevice::Type::Sink;
            devices.push_back(dev);
        }
    }

    return devices;
}

// ---------------------------------------------------------------------------
// Device / stream control
// ---------------------------------------------------------------------------

Result<std::vector<AudioDevice>, std::string> AudioEngine::list_devices() const {
    if (!initialized_) {
        return Result<std::vector<AudioDevice>, std::string>::error("engine not initialized");
    }
    return Result<std::vector<AudioDevice>, std::string>::ok(devices_);
}

Result<std::vector<AudioStream>, std::string> AudioEngine::list_streams() const {
    if (!initialized_) {
        return Result<std::vector<AudioStream>, std::string>::error("engine not initialized");
    }
    return Result<std::vector<AudioStream>, std::string>::ok(streams_);
}

Result<void, std::string> AudioEngine::set_volume(uint32_t stream_id, float level) {
    int pct = static_cast<int>(level * 100);
    if (pct < 0) pct = 0;
    if (pct > 150) pct = 150;

    // Try PipeWire first
    std::string cmd = "pw-cli s " + std::to_string(stream_id) +
                      " Props '{channelVolumes: [" +
                      std::to_string(level) + "," + std::to_string(level) +
                      "]}' 2>/dev/null";
    auto res = run_cmd(cmd);
    if (res.has_value()) {
        // Update cached state
        for (auto& s : streams_) {
            if (s.id == stream_id) { s.volume = level; break; }
        }
        return Result<void, std::string>::ok();
    }

    // pactl fallback
    cmd = "pactl set-sink-input-volume " + std::to_string(stream_id) +
          " " + std::to_string(pct) + "% 2>/dev/null";
    res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set volume: " + res.error());
    }

    for (auto& s : streams_) {
        if (s.id == stream_id) { s.volume = level; break; }
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioEngine::set_device_volume(uint32_t device_id, float level) {
    int pct = static_cast<int>(level * 100);
    if (pct < 0) pct = 0;
    if (pct > 150) pct = 150;

    // Determine if sink or source
    bool is_source = false;
    for (const auto& dev : devices_) {
        if (dev.id == device_id) {
            is_source = (dev.type == AudioDevice::Type::Source);
            break;
        }
    }

    std::string target = is_source ? "source" : "sink";
    std::string cmd = "pactl set-" + target + "-volume " +
                      std::to_string(device_id) + " " +
                      std::to_string(pct) + "% 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set device volume: " + res.error());
    }

    for (auto& dev : devices_) {
        if (dev.id == device_id) { dev.volume = level; break; }
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioEngine::set_mute(uint32_t stream_id, bool muted) {
    std::string val = muted ? "1" : "0";
    std::string cmd = "pactl set-sink-input-mute " + std::to_string(stream_id) +
                      " " + val + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set mute: " + res.error());
    }
    for (auto& s : streams_) {
        if (s.id == stream_id) { s.muted = muted; break; }
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioEngine::set_device_mute(uint32_t device_id, bool muted) {
    bool is_source = false;
    for (const auto& dev : devices_) {
        if (dev.id == device_id) {
            is_source = (dev.type == AudioDevice::Type::Source);
            break;
        }
    }

    std::string target = is_source ? "source" : "sink";
    std::string val = muted ? "1" : "0";
    std::string cmd = "pactl set-" + target + "-mute " +
                      std::to_string(device_id) + " " + val + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set device mute: " + res.error());
    }
    for (auto& dev : devices_) {
        if (dev.id == device_id) { dev.muted = muted; break; }
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioEngine::set_device(uint32_t stream_id, uint32_t device_id) {
    // Determine if this is an input or output stream
    bool is_input = false;
    for (const auto& s : streams_) {
        if (s.id == stream_id) { is_input = s.is_input; break; }
    }

    std::string cmd;
    if (is_input) {
        cmd = "pactl move-source-output " + std::to_string(stream_id) +
              " " + std::to_string(device_id) + " 2>/dev/null";
    } else {
        cmd = "pactl move-sink-input " + std::to_string(stream_id) +
              " " + std::to_string(device_id) + " 2>/dev/null";
    }

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to move stream: " + res.error());
    }

    for (auto& s : streams_) {
        if (s.id == stream_id) { s.device_id = device_id; break; }
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> AudioEngine::set_default(AudioDevice::Type type, uint32_t device_id) {
    std::string device_name;
    for (const auto& dev : devices_) {
        if (dev.id == device_id) {
            device_name = dev.name;
            break;
        }
    }
    if (device_name.empty()) {
        return Result<void, std::string>::error("device not found: " + std::to_string(device_id));
    }

    std::string target = (type == AudioDevice::Type::Sink) ? "default-sink" : "default-source";
    std::string cmd = "pactl set-" + target + " " + device_name + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set default: " + res.error());
    }

    for (auto& dev : devices_) {
        if (dev.type == type) dev.is_default = (dev.id == device_id);
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Loopback routing
// ---------------------------------------------------------------------------

Result<AudioLoopback, std::string> AudioEngine::create_loopback(uint32_t from_device,
                                                                   uint32_t to_device) {
    // Find device names
    std::string from_name, to_name;
    for (const auto& dev : devices_) {
        if (dev.id == from_device) from_name = dev.name;
        if (dev.id == to_device) to_name = dev.name;
    }
    if (from_name.empty() || to_name.empty()) {
        return Result<AudioLoopback, std::string>::error("device not found");
    }

    // Create loopback via PipeWire
    std::string cmd = "pw-loopback --capture-props='target.object=" + from_name +
                      "' --playback-props='target.object=" + to_name +
                      "' &  2>/dev/null";
    auto res = run_cmd(cmd);

    // Also try pactl module-loopback
    if (!res.has_value()) {
        cmd = "pactl load-module module-loopback source=" + from_name +
              " sink=" + to_name + " 2>/dev/null";
        res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<AudioLoopback, std::string>::error(
                "failed to create loopback: " + res.error());
        }
    }

    AudioLoopback lb;
    lb.id = next_loopback_id_++;
    lb.from_device = from_device;
    lb.to_device = to_device;
    lb.from_name = from_name;
    lb.to_name = to_name;
    loopbacks_.push_back(lb);

    return Result<AudioLoopback, std::string>::ok(lb);
}

Result<void, std::string> AudioEngine::remove_loopback(uint32_t loopback_id) {
    auto it = std::find_if(loopbacks_.begin(), loopbacks_.end(),
                           [loopback_id](const AudioLoopback& lb) { return lb.id == loopback_id; });
    if (it == loopbacks_.end()) {
        return Result<void, std::string>::error("loopback not found: " + std::to_string(loopback_id));
    }

    // Try to unload the module
    std::string cmd = "pactl unload-module module-loopback 2>/dev/null";
    run_cmd(cmd);

    loopbacks_.erase(it);
    return Result<void, std::string>::ok();
}

std::vector<AudioLoopback> AudioEngine::list_loopbacks() const {
    return loopbacks_;
}

// ---------------------------------------------------------------------------
// Card profile
// ---------------------------------------------------------------------------

Result<void, std::string> AudioEngine::set_card_profile(uint32_t device_id,
                                                          const std::string& profile) {
    // Find the card number for this device
    std::string cmd = "pactl set-card-profile " + std::to_string(device_id) +
                      " " + profile + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to set card profile: " + res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// EQ
// ---------------------------------------------------------------------------

Result<void, std::string> AudioEngine::apply_eq(const EQPreset& preset) {
    // Apply via PipeWire filter-chain or PulseAudio equalizer module
    // Build filter-chain config for PipeWire
    std::ostringstream config;
    config << "context.modules = [\n"
           << "  { name = libpipewire-module-filter-chain\n"
           << "    args = {\n"
           << "      node.description = \"StrayLight EQ: " << preset.name << "\"\n"
           << "      media.name = \"StrayLight EQ\"\n"
           << "      filter.graph = {\n"
           << "        nodes = [\n";

    for (size_t i = 0; i < preset.bands.size(); ++i) {
        const auto& band = preset.bands[i];
        config << "          {\n"
               << "            type = builtin\n"
               << "            name = eq_band_" << i << "\n"
               << "            label = bq_peaking\n"
               << "            control = {\n"
               << "              \"Freq\" = " << band.frequency_hz << "\n"
               << "              \"Q\" = " << band.q << "\n"
               << "              \"Gain\" = " << band.gain_db << "\n"
               << "            }\n"
               << "          }\n";
    }

    config << "        ]\n"
           << "        links = [\n";

    for (size_t i = 0; i + 1 < preset.bands.size(); ++i) {
        config << "          { output = \"eq_band_" << i << ":Out\""
               << " input = \"eq_band_" << (i + 1) << ":In\" }\n";
    }

    config << "        ]\n"
           << "      }\n"
           << "      capture.props = {\n"
           << "        node.name = \"straylight_eq_capture\"\n"
           << "        media.class = Audio/Sink\n"
           << "      }\n"
           << "      playback.props = {\n"
           << "        node.name = \"straylight_eq_playback\"\n"
           << "        node.passive = true\n"
           << "      }\n"
           << "    }\n"
           << "  }\n"
           << "]\n";

    // Write to PipeWire config drop-in
    std::string config_path = "/etc/pipewire/pipewire.conf.d/straylight-eq.conf";
    fs::create_directories("/etc/pipewire/pipewire.conf.d");
    std::ofstream out(config_path);
    if (!out.is_open()) {
        // Fallback to user config
        const char* home = std::getenv("HOME");
        if (!home) home = "/root";
        config_path = std::string(home) + "/.config/pipewire/pipewire.conf.d/straylight-eq.conf";
        fs::create_directories(fs::path(config_path).parent_path());
        out.open(config_path);
        if (!out.is_open()) {
            return Result<void, std::string>::error("cannot write EQ config");
        }
    }
    out << config.str();
    out.close();

    // Restart PipeWire to pick up changes
    run_cmd("systemctl --user restart pipewire.service 2>/dev/null");

    return Result<void, std::string>::ok();
}

} // namespace straylight
