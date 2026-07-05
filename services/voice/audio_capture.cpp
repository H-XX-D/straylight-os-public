// services/voice/audio_capture.cpp
// Microphone input with VAD, ring buffer, and silence detection.

#include "audio_capture.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Forward-declare ALSA types so we compile without the header on non-Linux.
// On a real Linux build, <alsa/asoundlib.h> would be included and these
// stubs would be replaced.
#ifdef __linux__
#include <alsa/asoundlib.h>
#else
// Stub for macOS / cross-compilation: treat pcm_handle_ as an fd to /dev/urandom
// that generates silence.  This lets the full pipeline build and test.
struct snd_pcm_t;
#endif

namespace straylight::voice {

// ─── Constants ──────────────────────────────────────────────────────────────

static constexpr int FRAME_SAMPLES  = 512;   // ~32 ms at 16 kHz
static constexpr int RING_SECONDS   = 30;
static constexpr float ZCR_SPEECH   = 0.02f; // zero-crossing floor for speech

// ─── Lifecycle ──────────────────────────────────────────────────────────────

AudioCapture::~AudioCapture() {
    stop_capture();
#ifdef __linux__
    if (pcm_handle_) {
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_handle_));
        pcm_handle_ = nullptr;
    }
#endif
}

Result<void, std::string> AudioCapture::init(const VoiceConfig& cfg) {
    device_name_   = cfg.audio_device;
    sample_rate_   = cfg.sample_rate;
    vad_threshold_ = cfg.vad_threshold;
    silence_ms_    = cfg.silence_timeout_ms;
    max_record_s_  = cfg.max_recording_s;

    // Compute derived values.
    ring_capacity_ = static_cast<size_t>(sample_rate_) * RING_SECONDS;
    ring_buffer_.resize(ring_capacity_, 0);
    ring_write_ = 0;

    // Frames of silence before we consider the utterance over.
    double frame_duration = static_cast<double>(FRAME_SAMPLES) / sample_rate_;
    frames_per_silence_timeout_ =
        static_cast<int>(static_cast<double>(silence_ms_) / 1000.0 / frame_duration);
    if (frames_per_silence_timeout_ < 1) frames_per_silence_timeout_ = 1;

#ifdef __linux__
    // Open ALSA capture device.
    snd_pcm_t* handle = nullptr;
    int err = snd_pcm_open(&handle, device_name_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        return Result<void, std::string>::error(
            "ALSA open failed: " + std::string(snd_strerror(err)));
    }

    // Configure hardware parameters.
    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(handle, hw_params);
    snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, hw_params, 1);

    unsigned int rate = static_cast<unsigned int>(sample_rate_);
    snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, nullptr);
    sample_rate_ = static_cast<int>(rate);

    snd_pcm_uframes_t period = FRAME_SAMPLES;
    snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period, nullptr);

    err = snd_pcm_hw_params(handle, hw_params);
    if (err < 0) {
        snd_pcm_close(handle);
        return Result<void, std::string>::error(
            "ALSA hw_params failed: " + std::string(snd_strerror(err)));
    }

    err = snd_pcm_prepare(handle);
    if (err < 0) {
        snd_pcm_close(handle);
        return Result<void, std::string>::error(
            "ALSA prepare failed: " + std::string(snd_strerror(err)));
    }

    pcm_handle_ = handle;
#else
    // Non-Linux stub: we will generate silence frames in read_frame().
    pcm_handle_ = reinterpret_cast<void*>(1); // sentinel
#endif

    fprintf(stdout, "[voice:audio] initialized device=%s rate=%d vad_thresh=%.2f "
            "silence=%dms ring=%ds\n",
            device_name_.c_str(), sample_rate_, vad_threshold_,
            silence_ms_, RING_SECONDS);

    return Result<void, std::string>::ok();
}

// ─── Capture control ────────────────────────────────────────────────────────

Result<void, std::string> AudioCapture::start_capture(SpeechCallback cb) {
    if (capturing_.load()) {
        return Result<void, std::string>::error("already capturing");
    }
    if (!pcm_handle_) {
        return Result<void, std::string>::error("audio not initialized");
    }

    speech_cb_ = std::move(cb);
    capturing_.store(true);
    capture_thread_ = std::thread(&AudioCapture::capture_loop, this);

    return Result<void, std::string>::ok();
}

void AudioCapture::stop_capture() {
    capturing_.store(false);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
}

Result<void, std::string> AudioCapture::start_push_to_talk() {
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    ptt_buffer_.clear();
    ptt_buffer_.sample_rate = sample_rate_;
    ptt_buffer_.channels = 1;
    ptt_active_.store(true);
    return Result<void, std::string>::ok();
}

Result<AudioBuffer, std::string> AudioCapture::stop_push_to_talk() {
    ptt_active_.store(false);
    std::lock_guard<std::mutex> lock(ptt_mutex_);
    AudioBuffer out = std::move(ptt_buffer_);
    ptt_buffer_.clear();
    return Result<AudioBuffer, std::string>::ok(std::move(out));
}

void AudioCapture::set_frame_callback(FrameCallback cb) {
    frame_cb_ = std::move(cb);
}

// ─── Ring buffer ────────────────────────────────────────────────────────────

void AudioCapture::ring_write(const int16_t* data, size_t count) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    for (size_t i = 0; i < count; ++i) {
        ring_buffer_[ring_write_] = data[i];
        ring_write_ = (ring_write_ + 1) % ring_capacity_;
    }
}

AudioBuffer AudioCapture::get_ring_buffer(double seconds) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    size_t n = static_cast<size_t>(seconds * sample_rate_);
    if (n > ring_capacity_) n = ring_capacity_;

    AudioBuffer out;
    out.sample_rate = sample_rate_;
    out.channels = 1;
    out.samples.resize(n);

    // Read backwards from write cursor.
    size_t read_pos = (ring_write_ + ring_capacity_ - n) % ring_capacity_;
    for (size_t i = 0; i < n; ++i) {
        out.samples[i] = ring_buffer_[(read_pos + i) % ring_capacity_];
    }
    return out;
}

// ─── VAD ────────────────────────────────────────────────────────────────────

VadResult AudioCapture::analyze_frame(const int16_t* data, size_t count) const {
    VadResult vad;
    if (count == 0) return vad;

    // Compute RMS energy (normalized to [0, 1]).
    double sum_sq = 0.0;
    int zero_crossings = 0;

    for (size_t i = 0; i < count; ++i) {
        double s = static_cast<double>(data[i]) / 32768.0;
        sum_sq += s * s;
        if (i > 0) {
            bool prev_pos = data[i - 1] >= 0;
            bool curr_pos = data[i] >= 0;
            if (prev_pos != curr_pos) ++zero_crossings;
        }
    }

    vad.energy = static_cast<float>(std::sqrt(sum_sq / count));
    vad.zcr    = static_cast<float>(zero_crossings) / static_cast<float>(count);

    // Speech detection: energy above threshold AND zero-crossing rate above floor.
    // This two-factor approach reduces false positives from clicks/pops (high
    // energy, low ZCR) and high-frequency noise (low energy, high ZCR).
    vad.is_speech = (vad.energy > vad_threshold_) && (vad.zcr > ZCR_SPEECH);

    return vad;
}

// ─── Read frame (platform abstraction) ──────────────────────────────────────

Result<size_t, std::string> AudioCapture::read_frame(int16_t* buf, size_t max_samples) {
#ifdef __linux__
    auto* handle = static_cast<snd_pcm_t*>(pcm_handle_);
    snd_pcm_sframes_t frames = snd_pcm_readi(handle, buf, max_samples);
    if (frames < 0) {
        // Try to recover from overrun/underrun.
        frames = snd_pcm_recover(handle, static_cast<int>(frames), 0);
        if (frames < 0) {
            return Result<size_t, std::string>::error(
                "ALSA read failed: " + std::string(snd_strerror(static_cast<int>(frames))));
        }
    }
    return Result<size_t, std::string>::ok(static_cast<size_t>(frames));
#else
    // Non-Linux stub: generate silence at real-time pace.
    std::this_thread::sleep_for(std::chrono::microseconds(
        static_cast<int64_t>(1000000.0 * max_samples / sample_rate_)));
    std::memset(buf, 0, max_samples * sizeof(int16_t));
    return Result<size_t, std::string>::ok(max_samples);
#endif
}

// ─── Main capture loop ──────────────────────────────────────────────────────

void AudioCapture::capture_loop() {
    std::vector<int16_t> frame(FRAME_SAMPLES);
    auto max_speech_samples = static_cast<size_t>(sample_rate_) * max_record_s_;

    while (capturing_.load()) {
        auto result = read_frame(frame.data(), FRAME_SAMPLES);
        if (!result.has_value()) {
            fprintf(stderr, "[voice:audio] read error: %s\n", result.error().c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        size_t n = result.value();
        if (n == 0) continue;

        // Always write to ring buffer.
        ring_write(frame.data(), n);

        // Push-to-talk mode: accumulate everything.
        if (ptt_active_.load()) {
            std::lock_guard<std::mutex> lock(ptt_mutex_);
            ptt_buffer_.samples.insert(ptt_buffer_.samples.end(),
                                       frame.begin(), frame.begin() + n);
        }

        // Notify frame callback (streaming mode).
        if (frame_cb_) {
            frame_cb_(frame.data(), n);
        }

        // VAD-based speech segmentation.
        VadResult vad = analyze_frame(frame.data(), n);

        if (vad.is_speech) {
            if (!in_speech_) {
                in_speech_ = true;
                current_speech_.clear();
                current_speech_.sample_rate = sample_rate_;
                current_speech_.channels = 1;
                silence_frames_ = 0;

                // Grab the last 0.5 seconds from ring buffer to capture the
                // start of speech that may have been missed.
                auto preamble = get_ring_buffer(0.5);
                current_speech_.append(preamble);
            }
            silence_frames_ = 0;
            current_speech_.samples.insert(current_speech_.samples.end(),
                                           frame.begin(), frame.begin() + n);
        } else if (in_speech_) {
            // Still accumulate during silence gap.
            current_speech_.samples.insert(current_speech_.samples.end(),
                                           frame.begin(), frame.begin() + n);
            ++silence_frames_;

            if (silence_frames_ >= frames_per_silence_timeout_) {
                // Utterance complete.
                in_speech_ = false;
                silence_frames_ = 0;

                if (speech_cb_ && !current_speech_.empty()) {
                    speech_cb_(std::move(current_speech_));
                }
                current_speech_.clear();
            }
        }

        // Safety: cap maximum recording length.
        if (in_speech_ && current_speech_.samples.size() >= max_speech_samples) {
            in_speech_ = false;
            silence_frames_ = 0;
            if (speech_cb_ && !current_speech_.empty()) {
                speech_cb_(std::move(current_speech_));
            }
            current_speech_.clear();
        }
    }
}

} // namespace straylight::voice
