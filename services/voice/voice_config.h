// services/voice/voice_config.h
// Configuration for the StrayLight Voice assistant daemon.
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace straylight::voice {

/// Alias used by every voice component.
template <typename E = std::string>
using VoidResult = Result<void, E>;

/// Configuration for the voice daemon, loaded from /etc/straylight/voice.conf.
struct VoiceConfig {
    // [general]
    std::string wake_word           = "hey straylight";
    std::string language            = "en";
    bool        push_to_talk        = false;

    // [stt]
    std::string stt_model_path      = "/usr/share/straylight/models/whisper-base.gguf";
    std::string stt_language        = "auto";

    // [tts]
    std::string tts_model_path      = "/usr/share/straylight/models/piper-en.onnx";
    std::string tts_voice           = "default";
    float       tts_speed           = 1.0f;
    float       tts_pitch           = 1.0f;

    // [llm]
    std::string llm_model_path      = "/usr/share/straylight/models/qwen-0.5b-q4.gguf";
    std::string system_prompt       = "You are Straylight Voice, an AI assistant built into "
                                      "StrayLight OS. You can control the system using tools. "
                                      "Be concise and helpful.";
    float       llm_temperature     = 0.7f;
    int         llm_max_tokens      = 512;
    int         context_window      = 10;

    // [audio]
    std::string audio_device        = "default";
    int         sample_rate         = 16000;
    float       vad_threshold       = 0.3f;
    int         silence_timeout_ms  = 1500;
    int         max_recording_s     = 30;

    // [safety]
    std::set<std::string> require_confirmation = {
        "shutdown", "reboot", "format", "delete", "encrypt"
    };
    std::vector<std::string> blocked_commands = {
        "rm -rf /", "dd if=/dev/zero"
    };

    /// Load configuration from an INI-style file.
    static Result<VoiceConfig, std::string> load(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) {
            return Result<VoiceConfig, std::string>::error(
                "cannot open config: " + path);
        }

        VoiceConfig cfg;
        std::string section;
        std::string line;

        while (std::getline(in, line)) {
            // Strip leading/trailing whitespace.
            auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);

            // Skip comments and blank lines.
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            // Section header.
            if (line[0] == '[') {
                auto end = line.find(']');
                if (end != std::string::npos) {
                    section = line.substr(1, end - 1);
                }
                continue;
            }

            // Key = Value.
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            // Trim key and value.
            auto trim = [](std::string& s) {
                auto a = s.find_first_not_of(" \t");
                auto b = s.find_last_not_of(" \t\r\n");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
            };
            trim(key);
            trim(val);

            // Apply values by section.
            if (section == "general") {
                if (key == "wake_word")       cfg.wake_word = val;
                else if (key == "language")   cfg.language = val;
                else if (key == "push_to_talk") cfg.push_to_talk = (val == "true" || val == "1");
            } else if (section == "stt") {
                if (key == "model")           cfg.stt_model_path = val;
                else if (key == "language")   cfg.stt_language = val;
            } else if (section == "tts") {
                if (key == "model")           cfg.tts_model_path = val;
                else if (key == "voice")      cfg.tts_voice = val;
                else if (key == "speed")      cfg.tts_speed = std::stof(val);
                else if (key == "pitch")      cfg.tts_pitch = std::stof(val);
            } else if (section == "llm") {
                if (key == "model")           cfg.llm_model_path = val;
                else if (key == "system_prompt") cfg.system_prompt = val;
                else if (key == "temperature") cfg.llm_temperature = std::stof(val);
                else if (key == "max_tokens") cfg.llm_max_tokens = std::stoi(val);
                else if (key == "context_window") cfg.context_window = std::stoi(val);
            } else if (section == "audio") {
                if (key == "device")          cfg.audio_device = val;
                else if (key == "sample_rate") cfg.sample_rate = std::stoi(val);
                else if (key == "vad_threshold") cfg.vad_threshold = std::stof(val);
                else if (key == "silence_timeout_ms") cfg.silence_timeout_ms = std::stoi(val);
                else if (key == "max_recording_s") cfg.max_recording_s = std::stoi(val);
            } else if (section == "safety") {
                if (key == "require_confirmation") {
                    cfg.require_confirmation.clear();
                    std::istringstream ss(val);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        auto a = token.find_first_not_of(" \t");
                        auto b = token.find_last_not_of(" \t");
                        if (a != std::string::npos) {
                            cfg.require_confirmation.insert(token.substr(a, b - a + 1));
                        }
                    }
                } else if (key == "blocked_commands") {
                    cfg.blocked_commands.clear();
                    std::istringstream ss(val);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        auto a = token.find_first_not_of(" \t");
                        auto b = token.find_last_not_of(" \t");
                        if (a != std::string::npos) {
                            cfg.blocked_commands.push_back(token.substr(a, b - a + 1));
                        }
                    }
                }
            }
        }

        return Result<VoiceConfig, std::string>::ok(std::move(cfg));
    }

    /// Check if a command keyword requires verbal confirmation.
    bool needs_confirmation(const std::string& keyword) const {
        for (const auto& kw : require_confirmation) {
            if (keyword.find(kw) != std::string::npos) return true;
        }
        return false;
    }

    /// Check if a command is blocked.
    bool is_blocked(const std::string& cmd) const {
        for (const auto& blocked : blocked_commands) {
            if (cmd.find(blocked) != std::string::npos) return true;
        }
        return false;
    }
};

} // namespace straylight::voice
