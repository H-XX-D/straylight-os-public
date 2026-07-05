// services/voice/audio_capture.h
// Microphone input with VAD, wake-word buffering, and silence detection.
#pragma once

#include "voice_config.h"
#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight::voice {

/// A contiguous chunk of 16-bit PCM audio.
struct AudioBuffer {
    std::vector<int16_t> samples;
    int sample_rate = 16000;
    int channels    = 1;

    /// Duration in seconds.
    double duration_seconds() const {
        if (sample_rate == 0 || channels == 0) return 0.0;
        return static_cast<double>(samples.size()) / (sample_rate * channels);
    }

    /// Convert to float [-1.0, 1.0] for model inference.
    std::vector<float> to_float() const {
        std::vector<float> out(samples.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            out[i] = static_cast<float>(samples[i]) / 32768.0f;
        }
        return out;
    }

    /// Append another buffer.
    void append(const AudioBuffer& other) {
        samples.insert(samples.end(), other.samples.begin(), other.samples.end());
    }

    void clear() { samples.clear(); }
    bool empty() const { return samples.empty(); }
};

/// Voice Activity Detection result for a single frame.
struct VadResult {
    bool   is_speech   = false;
    float  energy      = 0.0f;
    float  zcr         = 0.0f;   // zero-crossing rate
    double timestamp_s = 0.0;
};

/// Callback when speech segment is fully captured (after silence timeout).
using SpeechCallback = std::function<void(AudioBuffer)>;

/// Callback for each raw audio frame (for streaming).
using FrameCallback = std::function<void(const int16_t*, size_t)>;

/// Microphone capture engine with VAD and ring buffer.
class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture();

    // Non-copyable.
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /// Initialize audio capture with the given config.
    Result<void, std::string> init(const VoiceConfig& cfg);

    /// Start continuous capture.  Calls speech_cb when a full utterance is
    /// detected (speech + silence timeout).
    Result<void, std::string> start_capture(SpeechCallback speech_cb);

    /// Stop capture.
    void stop_capture();

    /// Are we currently capturing?
    bool is_capturing() const { return capturing_.load(); }

    /// Start a push-to-talk recording.  Returns immediately; call
    /// stop_push_to_talk() to get the buffer.
    Result<void, std::string> start_push_to_talk();

    /// End push-to-talk and return the captured audio.
    Result<AudioBuffer, std::string> stop_push_to_talk();

    /// Get the last N seconds from the ring buffer (always-on background capture).
    AudioBuffer get_ring_buffer(double seconds) const;

    /// Run VAD on a single frame.
    VadResult analyze_frame(const int16_t* data, size_t count) const;

    /// Get current sample rate.
    int sample_rate() const { return sample_rate_; }

    /// Set a raw frame callback for streaming mode.
    void set_frame_callback(FrameCallback cb);

private:
    // Configuration.
    std::string device_name_   = "default";
    int         sample_rate_   = 16000;
    float       vad_threshold_ = 0.3f;
    int         silence_ms_    = 1500;
    int         max_record_s_  = 30;

    // Ring buffer: stores last 30 seconds of audio for wake-word rewind.
    mutable std::mutex ring_mutex_;
    std::vector<int16_t> ring_buffer_;
    size_t ring_capacity_ = 0;   // max samples
    size_t ring_write_    = 0;   // write cursor

    // Capture state.
    std::atomic<bool> capturing_{false};
    std::atomic<bool> ptt_active_{false};
    std::thread capture_thread_;
    SpeechCallback speech_cb_;
    FrameCallback  frame_cb_;

    // Speech segment accumulator.
    AudioBuffer current_speech_;
    bool        in_speech_       = false;
    int         silence_frames_  = 0;
    int         frames_per_silence_timeout_ = 0;

    // Internal capture loop.
    void capture_loop();

    // Read a frame from the microphone (ALSA or PipeWire).
    Result<size_t, std::string> read_frame(int16_t* buf, size_t max_samples);

    // Write samples to ring buffer.
    void ring_write(const int16_t* data, size_t count);

    // Push-to-talk buffer.
    mutable std::mutex ptt_mutex_;
    AudioBuffer ptt_buffer_;

    // File descriptor for the capture device (ALSA PCM handle abstracted as fd).
    void* pcm_handle_ = nullptr;
};

} // namespace straylight::voice
