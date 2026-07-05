// services/voice/tts_engine.cpp
// Piper TTS with espeak-ng fallback and ALSA/PipeWire playback.

#include "tts_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

namespace straylight::voice {

// ─── Constants ──────────────────────────────────────────────────────────────

static constexpr const char* MODELS_DIR = "/usr/share/straylight/models";
static constexpr int PLAYBACK_BUFFER_FRAMES = 1024;

// ─── Lifecycle ──────────────────────────────────────────────────────────────

TtsEngine::~TtsEngine() {
    stop_playback();
    // Piper ONNX session cleanup would go here.
    piper_session_ = nullptr;
}

Result<void, std::string> TtsEngine::init(const VoiceConfig& cfg) {
    model_path_   = cfg.tts_model_path;
    voice_name_   = cfg.tts_voice;
    audio_device_ = cfg.audio_device;
    speed_        = cfg.tts_speed;
    pitch_        = cfg.tts_pitch;

    // Scan for installed voice models.
    scan_voice_models();

    // Try to load the Piper ONNX model.
    namespace fs = std::filesystem;
    if (fs::exists(model_path_)) {
        fprintf(stdout, "[voice:tts] loading Piper model %s...\n", model_path_.c_str());

        // In a full build, we would load the ONNX model via onnxruntime:
        //   Ort::SessionOptions opts;
        //   opts.SetIntraOpNumThreads(2);
        //   piper_session_ = new Ort::Session(env, model_path_.c_str(), opts);
        //
        // For now, we detect the model's existence and set the loaded flag.
        // Actual synthesis will use the piper-tts CLI if the library is not linked.

        // Check for the JSON config that accompanies every Piper model.
        std::string config_path = model_path_;
        auto dot = config_path.rfind('.');
        if (dot != std::string::npos) {
            config_path = config_path.substr(0, dot) + ".onnx.json";
        }

        if (fs::exists(config_path)) {
            // Parse sample rate from JSON config.
            std::ifstream cfg_in(config_path);
            std::string json_content((std::istreambuf_iterator<char>(cfg_in)),
                                      std::istreambuf_iterator<char>());
            auto sr_pos = json_content.find("\"sample_rate\"");
            if (sr_pos != std::string::npos) {
                auto colon = json_content.find(':', sr_pos);
                if (colon != std::string::npos) {
                    auto num_start = json_content.find_first_of("0123456789", colon);
                    if (num_start != std::string::npos) {
                        try {
                            output_sample_rate_ = std::stoi(json_content.substr(num_start));
                        } catch (...) {}
                    }
                }
            }
        }

        model_loaded_ = true;
        fprintf(stdout, "[voice:tts] model loaded, output_rate=%d\n", output_sample_rate_);
    } else {
        fprintf(stderr, "[voice:tts] model not found: %s\n", model_path_.c_str());
        fprintf(stderr, "[voice:tts] will use espeak-ng fallback\n");
    }

    fprintf(stdout, "[voice:tts] initialized voice=%s speed=%.1f pitch=%.1f voices=%zu\n",
            voice_name_.c_str(), speed_, pitch_, available_voices_.size());

    return Result<void, std::string>::ok();
}

// ─── Voice scanning ─────────────────────────────────────────────────────────

void TtsEngine::scan_voice_models() {
    namespace fs = std::filesystem;
    available_voices_.clear();

    // Always add the default configured model.
    TtsVoice default_voice;
    default_voice.name = "default";
    default_voice.model_path = model_path_;
    default_voice.language = "en";
    default_voice.description = "Default English voice";
    default_voice.loaded = fs::exists(model_path_);
    available_voices_.push_back(default_voice);

    // Scan models directory for additional .onnx files.
    std::error_code ec;
    if (!fs::exists(MODELS_DIR, ec)) return;

    for (const auto& entry : fs::directory_iterator(MODELS_DIR, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (fname.find("piper") == std::string::npos) continue;
        if (entry.path().extension() != ".onnx") continue;
        if (entry.path().string() == model_path_) continue; // skip default

        TtsVoice v;
        v.model_path = entry.path().string();
        v.loaded = true;

        // Derive name from filename: piper-en-us-lessac.onnx → en-us-lessac
        v.name = fname.substr(0, fname.size() - 5); // strip .onnx
        if (v.name.substr(0, 6) == "piper-") v.name = v.name.substr(6);

        // Guess language from name.
        if (v.name.size() >= 2) {
            v.language = v.name.substr(0, 2);
        }
        v.description = "Piper voice: " + v.name;

        available_voices_.push_back(std::move(v));
    }
}

std::vector<TtsVoice> TtsEngine::list_voices() const {
    return available_voices_;
}

Result<void, std::string> TtsEngine::set_voice(const std::string& name) {
    for (const auto& v : available_voices_) {
        if (v.name == name) {
            model_path_ = v.model_path;
            voice_name_ = v.name;
            model_loaded_ = v.loaded;
            return Result<void, std::string>::ok();
        }
    }
    return Result<void, std::string>::error("voice not found: " + name);
}

// ─── Synthesis ──────────────────────────────────────────────────────────────

Result<AudioBuffer, std::string> TtsEngine::synthesize(const std::string& text) {
    if (text.empty()) {
        return Result<AudioBuffer, std::string>::error("empty text");
    }

    // If Piper model is loaded, use the piper-tts CLI (which loads the ONNX model).
    // In a full build with libonnxruntime linked, we would call the C API directly.
    if (model_loaded_) {
        // Use piper CLI: echo text | piper --model path --output_raw
        std::string escaped_text;
        for (char c : text) {
            if (c == '"') escaped_text += "\\\"";
            else if (c == '\\') escaped_text += "\\\\";
            else if (c == '`') escaped_text += "\\`";
            else if (c == '$') escaped_text += "\\$";
            else escaped_text += c;
        }

        std::string cmd = "echo \"" + escaped_text + "\" | piper --model " +
                          model_path_ + " --output_raw 2>/dev/null";

        // Adjust speed via --length_scale (inverse: 0.5 speed = 2.0 length_scale).
        if (std::fabs(speed_ - 1.0f) > 0.01f) {
            float length_scale = 1.0f / speed_;
            cmd = "echo \"" + escaped_text + "\" | piper --model " +
                  model_path_ + " --length_scale " +
                  std::to_string(length_scale) + " --output_raw 2>/dev/null";
        }

        // Check if piper is available.
        if (std::system("which piper >/dev/null 2>&1") == 0) {
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) {
                return fallback_synthesize(text);
            }

            // Read raw PCM (16-bit signed, mono, at output_sample_rate_).
            AudioBuffer buf;
            buf.sample_rate = output_sample_rate_;
            buf.channels = 1;

            int16_t sample;
            while (fread(&sample, sizeof(int16_t), 1, pipe) == 1) {
                buf.samples.push_back(sample);
            }
            int status = pclose(pipe);

            if (status != 0 || buf.empty()) {
                return fallback_synthesize(text);
            }

            return Result<AudioBuffer, std::string>::ok(std::move(buf));
        }
    }

    return fallback_synthesize(text);
}

Result<AudioBuffer, std::string> TtsEngine::synthesize_ssml(const std::string& ssml) {
    // Strip SSML tags for a simple implementation, preserving <break> as pauses.
    std::string plain;
    bool in_tag = false;
    bool in_break = false;

    for (size_t i = 0; i < ssml.size(); ++i) {
        if (ssml[i] == '<') {
            in_tag = true;
            // Check for <break> tag.
            if (ssml.substr(i, 6) == "<break") {
                in_break = true;
            }
            continue;
        }
        if (ssml[i] == '>') {
            if (in_break) {
                // Insert a pause marker (period + space) for natural break.
                plain += ". ";
                in_break = false;
            }
            in_tag = false;
            continue;
        }
        if (!in_tag) {
            plain += ssml[i];
        }
    }

    return synthesize(plain);
}

// ─── Playback ───────────────────────────────────────────────────────────────

Result<void, std::string> TtsEngine::speak(const std::string& text) {
    auto result = synthesize(text);
    if (!result.has_value()) {
        return Result<void, std::string>::error(result.error());
    }
    return play_audio(result.value());
}

Result<void, std::string> TtsEngine::play_audio(const AudioBuffer& buf) {
    if (buf.empty()) {
        return Result<void, std::string>::error("empty audio buffer");
    }
    return play_pcm(buf.samples.data(), buf.samples.size(), buf.sample_rate);
}

void TtsEngine::stop_playback() {
    playing_ = false;
}

Result<void, std::string> TtsEngine::play_pcm(
    const int16_t* data, size_t count, int sample_rate)
{
    playing_ = true;

#ifdef __linux__
    snd_pcm_t* handle = nullptr;
    int err = snd_pcm_open(&handle, audio_device_.c_str(),
                           SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        playing_ = false;
        // Fall back to aplay.
        std::string tmp = "/tmp/straylight-tts-" + std::to_string(getpid()) + ".wav";

        // Write WAV header + data.
        std::ofstream out(tmp, std::ios::binary);
        if (!out.is_open()) {
            return Result<void, std::string>::error("cannot write temp WAV for playback");
        }

        uint32_t data_size = static_cast<uint32_t>(count * sizeof(int16_t));
        uint32_t file_size = 36 + data_size;
        auto write_u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto write_u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

        out.write("RIFF", 4);
        write_u32(file_size);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        write_u32(16);
        write_u16(1);
        write_u16(1);
        write_u32(static_cast<uint32_t>(sample_rate));
        write_u32(static_cast<uint32_t>(sample_rate * 2));
        write_u16(2);
        write_u16(16);
        out.write("data", 4);
        write_u32(data_size);
        out.write(reinterpret_cast<const char*>(data), data_size);
        out.close();

        std::string cmd = "aplay " + tmp + " 2>/dev/null; rm -f " + tmp;
        std::system(cmd.c_str());
        playing_ = false;
        return Result<void, std::string>::ok();
    }

    // Configure ALSA playback.
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(handle, hw);
    snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, hw, 1);

    unsigned int rate = static_cast<unsigned int>(sample_rate);
    snd_pcm_hw_params_set_rate_near(handle, hw, &rate, nullptr);

    snd_pcm_uframes_t period = PLAYBACK_BUFFER_FRAMES;
    snd_pcm_hw_params_set_period_size_near(handle, hw, &period, nullptr);

    snd_pcm_hw_params(handle, hw);
    snd_pcm_prepare(handle);

    // Write audio data in chunks.
    size_t offset = 0;
    while (offset < count && playing_) {
        snd_pcm_uframes_t frames = std::min(
            static_cast<snd_pcm_uframes_t>(count - offset),
            period);
        snd_pcm_sframes_t written = snd_pcm_writei(handle, data + offset, frames);
        if (written < 0) {
            snd_pcm_recover(handle, static_cast<int>(written), 0);
        } else {
            offset += static_cast<size_t>(written);
        }
    }

    snd_pcm_drain(handle);
    snd_pcm_close(handle);
#else
    // Non-Linux: write WAV and play with system player.
    std::string tmp = "/tmp/straylight-tts-" + std::to_string(getpid()) + ".wav";
    std::ofstream out(tmp, std::ios::binary);
    if (!out.is_open()) {
        playing_ = false;
        return Result<void, std::string>::error("cannot write temp WAV for playback");
    }

    uint32_t data_size = static_cast<uint32_t>(count * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;
    auto write_u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto write_u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    write_u32(file_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(16);
    write_u16(1);
    write_u16(1);
    write_u32(static_cast<uint32_t>(sample_rate));
    write_u32(static_cast<uint32_t>(sample_rate * 2));
    write_u16(2);
    write_u16(16);
    out.write("data", 4);
    write_u32(data_size);
    out.write(reinterpret_cast<const char*>(data), data_size);
    out.close();

    // macOS: use afplay; Linux fallback: aplay.
    std::string cmd = "afplay " + tmp + " 2>/dev/null || aplay " + tmp +
                      " 2>/dev/null; rm -f " + tmp;
    std::system(cmd.c_str());
#endif

    playing_ = false;
    return Result<void, std::string>::ok();
}

// ─── espeak-ng fallback ─────────────────────────────────────────────────────

Result<AudioBuffer, std::string> TtsEngine::fallback_synthesize(const std::string& text) {
    // Use espeak-ng to generate WAV, then read it back.
    bool have_espeak = (std::system("which espeak-ng >/dev/null 2>&1") == 0);
    bool have_espeak_classic = (std::system("which espeak >/dev/null 2>&1") == 0);

    std::string espeak_bin;
    if (have_espeak)         espeak_bin = "espeak-ng";
    else if (have_espeak_classic) espeak_bin = "espeak";
    else {
        return Result<AudioBuffer, std::string>::error(
            "no TTS backend: install piper-tts or espeak-ng");
    }

    std::string tmp_wav = "/tmp/straylight-tts-" + std::to_string(getpid()) + ".wav";

    // Escape text for shell.
    std::string escaped;
    for (char c : text) {
        if (c == '\'') escaped += "'\\''";
        else escaped += c;
    }

    // Build command with speed adjustment.
    int wpm = static_cast<int>(175.0f * speed_); // espeak default is 175 wpm
    int pitch_val = static_cast<int>(50.0f * pitch_); // espeak pitch 0-99, default 50

    std::string cmd = espeak_bin + " -w " + tmp_wav +
                      " -s " + std::to_string(wpm) +
                      " -p " + std::to_string(pitch_val) +
                      " '" + escaped + "' 2>/dev/null";

    int status = std::system(cmd.c_str());
    if (status != 0) {
        std::remove(tmp_wav.c_str());
        return Result<AudioBuffer, std::string>::error(
            "espeak-ng failed with exit code " + std::to_string(status));
    }

    // Read the WAV file back.
    std::ifstream wav_file(tmp_wav, std::ios::binary);
    if (!wav_file.is_open()) {
        return Result<AudioBuffer, std::string>::error("cannot read espeak output");
    }

    // Parse WAV header (skip to data chunk).
    char header[44];
    wav_file.read(header, 44);

    // Extract sample rate from header (bytes 24-27).
    int sr = *reinterpret_cast<int*>(header + 24);

    // Read PCM data.
    AudioBuffer buf;
    buf.sample_rate = sr;
    buf.channels = 1;

    int16_t sample;
    while (wav_file.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
        buf.samples.push_back(sample);
    }
    wav_file.close();
    std::remove(tmp_wav.c_str());

    if (buf.empty()) {
        return Result<AudioBuffer, std::string>::error("espeak produced no audio");
    }

    return Result<AudioBuffer, std::string>::ok(std::move(buf));
}

// ─── Resampler ──────────────────────────────────────────────────────────────

std::vector<int16_t> TtsEngine::resample(
    const int16_t* data, size_t count, int from_rate, int to_rate)
{
    if (from_rate == to_rate || count == 0) {
        return std::vector<int16_t>(data, data + count);
    }

    double ratio = static_cast<double>(to_rate) / from_rate;
    size_t new_size = static_cast<size_t>(count * ratio);
    std::vector<int16_t> out(new_size);

    for (size_t i = 0; i < new_size; ++i) {
        double src = i / ratio;
        size_t idx = static_cast<size_t>(src);
        double frac = src - idx;

        if (idx + 1 < count) {
            out[i] = static_cast<int16_t>(
                data[idx] * (1.0 - frac) + data[idx + 1] * frac);
        } else {
            out[i] = data[std::min(idx, count - 1)];
        }
    }

    return out;
}

} // namespace straylight::voice
