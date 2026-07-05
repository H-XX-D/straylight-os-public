// services/voice/tts_engine.h
// Text-to-Speech engine backed by Piper TTS (local, offline).
#pragma once

#include "audio_capture.h"
#include "voice_config.h"
#include "straylight/result.h"

#include <string>
#include <vector>

namespace straylight::voice {

/// Available TTS voices.
struct TtsVoice {
    std::string name;
    std::string model_path;
    std::string language;
    std::string description;
    bool loaded = false;
};

/// Text-to-Speech engine.
class TtsEngine {
public:
    TtsEngine() = default;
    ~TtsEngine();

    // Non-copyable.
    TtsEngine(const TtsEngine&) = delete;
    TtsEngine& operator=(const TtsEngine&) = delete;

    /// Initialize with config.
    Result<void, std::string> init(const VoiceConfig& cfg);

    /// Synthesize text to PCM audio.
    Result<AudioBuffer, std::string> synthesize(const std::string& text);

    /// Synthesize SSML to PCM audio (supports <break>, <emphasis>, etc.).
    Result<AudioBuffer, std::string> synthesize_ssml(const std::string& ssml);

    /// Synthesize and immediately play through speakers.
    Result<void, std::string> speak(const std::string& text);

    /// Play a raw PCM buffer through the audio device.
    Result<void, std::string> play_audio(const AudioBuffer& buf);

    /// Stop any currently playing audio.
    void stop_playback();

    /// Is audio currently playing?
    bool is_playing() const { return playing_; }

    /// List available voices.
    std::vector<TtsVoice> list_voices() const;

    /// Switch to a different voice.
    Result<void, std::string> set_voice(const std::string& name);

    /// Set speed multiplier (0.5 = half speed, 2.0 = double speed).
    void set_speed(float speed) { speed_ = speed; }

    /// Set pitch multiplier.
    void set_pitch(float pitch) { pitch_ = pitch; }

    /// Check if a model is loaded.
    bool is_loaded() const { return model_loaded_; }

    /// Get current voice name.
    const std::string& current_voice() const { return voice_name_; }

private:
    std::string model_path_;
    std::string voice_name_ = "default";
    std::string audio_device_ = "default";
    float speed_ = 1.0f;
    float pitch_ = 1.0f;
    int   output_sample_rate_ = 22050; // Piper default
    bool  model_loaded_ = false;
    bool  playing_ = false;

    // Opaque handle to Piper ONNX session.
    void* piper_session_ = nullptr;

    // Scan for available voice models.
    std::vector<TtsVoice> available_voices_;
    void scan_voice_models();

    // Fallback: use espeak-ng.
    Result<AudioBuffer, std::string> fallback_synthesize(const std::string& text);

    // Play PCM via ALSA / PipeWire.
    Result<void, std::string> play_pcm(const int16_t* data, size_t count, int sample_rate);

    // Resample audio to a target rate.
    static std::vector<int16_t> resample(const int16_t* data, size_t count,
                                         int from_rate, int to_rate);
};

} // namespace straylight::voice
