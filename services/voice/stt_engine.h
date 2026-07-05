// services/voice/stt_engine.h
// Speech-to-Text engine backed by whisper.cpp (local, offline).
#pragma once

#include "audio_capture.h"
#include "voice_config.h"
#include "straylight/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::voice {

/// A single word-level timestamp from transcription.
struct WordTimestamp {
    std::string word;
    double start_s = 0.0;
    double end_s   = 0.0;
};

/// Full transcription result.
struct TranscribeResult {
    std::string text;                    // full transcript
    std::string language;                // detected language code
    float       confidence = 0.0f;       // average token probability
    double      audio_duration_s = 0.0;  // input audio length
    double      process_time_s   = 0.0;  // wall-clock inference time
    std::vector<WordTimestamp> timestamps;
};

/// Available model sizes for Whisper.
enum class WhisperModelSize {
    Tiny,    // ~39 MB
    Base,    // ~74 MB
    Small,   // ~244 MB
    Medium,  // ~769 MB
};

/// Speech-to-Text engine.
class SttEngine {
public:
    SttEngine() = default;
    ~SttEngine();

    // Non-copyable.
    SttEngine(const SttEngine&) = delete;
    SttEngine& operator=(const SttEngine&) = delete;

    /// Initialize with config.  Loads the Whisper GGUF model into memory.
    Result<void, std::string> init(const VoiceConfig& cfg);

    /// Transcribe an audio buffer.  Blocking call.
    Result<TranscribeResult, std::string> transcribe(
        const AudioBuffer& audio);

    /// Transcribe with explicit sample rate (for raw PCM from files).
    Result<TranscribeResult, std::string> transcribe(
        const std::vector<float>& samples, int sample_rate);

    /// Check if a model is loaded.
    bool is_loaded() const { return model_loaded_; }

    /// Get the current model path.
    const std::string& model_path() const { return model_path_; }

    /// Get the model size.
    WhisperModelSize model_size() const { return model_size_; }

    /// Load a different model at runtime.
    Result<void, std::string> load_model(const std::string& path);

    /// Unload the current model to free memory.
    void unload_model();

private:
    std::string model_path_;
    std::string language_ = "auto";
    bool model_loaded_    = false;
    WhisperModelSize model_size_ = WhisperModelSize::Base;

    // Opaque handle to whisper_context (whisper.cpp).
    void* whisper_ctx_ = nullptr;

    // Fallback: shell out to whisper-cli or espeak.
    Result<TranscribeResult, std::string> fallback_transcribe(
        const std::vector<float>& samples, int sample_rate);

    // Write PCM to a temp WAV file for CLI fallback.
    Result<std::string, std::string> write_temp_wav(
        const std::vector<float>& samples, int sample_rate);

    // Determine model size from file path.
    WhisperModelSize detect_model_size(const std::string& path) const;
};

} // namespace straylight::voice
