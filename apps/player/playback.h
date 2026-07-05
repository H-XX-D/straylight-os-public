// apps/player/playback.h
// StrayLight Player — GStreamer pipeline management
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

// Forward-declare GStreamer types to avoid polluting headers
using GstElement  = struct _GstElement;
using GstBus      = struct _GstBus;
using GstMessage  = struct _GstMessage;
using GMainLoop   = struct _GMainLoop;

namespace straylight::player {

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
};

/// GStreamer playbin-based audio/video playback engine.
/// Thread-safe: all public methods may be called from the UI thread while
/// the GStreamer thread drives the pipeline.
class PlaybackEngine {
public:
    PlaybackEngine();
    ~PlaybackEngine();

    // Non-copyable, non-movable (owns GStreamer resources)
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    /// Initialise GStreamer (must be called once from main, before any engine).
    static void gst_init_once(int* argc, char*** argv);

    /// Open a URI (file:// or network). Does not start playback.
    Result<void, SLError> open(std::string_view uri);

    /// Start / resume playback.
    Result<void, SLError> play();

    /// Pause playback.
    Result<void, SLError> pause();

    /// Stop and reset position.
    Result<void, SLError> stop();

    /// Seek to absolute position in seconds.
    Result<void, SLError> seek(double seconds);

    /// Current playback position in seconds.
    double position() const;

    /// Total media duration in seconds (-1 if unknown).
    double duration() const;

    /// Volume [0.0, 1.0].
    void   set_volume(double v);
    double volume() const;

    /// Mute toggle.
    void   set_mute(bool muted);
    bool   muted() const;

    PlaybackState state() const;

    /// Register a callback invoked (from GStreamer bus watch on main thread) when
    /// the current track ends.
    using EndOfStreamCallback = std::function<void()>;
    void on_end_of_stream(EndOfStreamCallback cb) { eos_cb_ = std::move(cb); }

    /// Register a callback for errors.
    using ErrorCallback = std::function<void(std::string)>;
    void on_error(ErrorCallback cb) { error_cb_ = std::move(cb); }

    /// Pump GStreamer bus messages. Call from the UI thread each frame.
    void pump_messages();

    /// Level data from the GStreamer level element (peak dB per channel).
    /// Updated asynchronously; read is lock-free via atomic snapshot.
    static constexpr int kMaxChannels = 2;
    struct LevelData {
        double peak_db[kMaxChannels]  = {-100.0, -100.0};
        double rms_db[kMaxChannels]   = {-100.0, -100.0};
        int    channels               = 0;
    };
    LevelData level() const;

private:
    GstElement* pipeline_  = nullptr;  ///< playbin element
    GstElement* level_elem_ = nullptr; ///< GStreamer level analyser (in audio sink chain)
    GstBus*     bus_       = nullptr;

    mutable std::mutex state_mutex_;
    PlaybackState      state_  = PlaybackState::Stopped;
    double             volume_ = 1.0;
    bool               muted_  = false;
    std::string        current_uri_;

    // Level data — written by GStreamer message thread, read by UI thread
    mutable std::mutex level_mutex_;
    LevelData          level_data_;

    EndOfStreamCallback eos_cb_;
    ErrorCallback       error_cb_;

    void handle_eos();
    void handle_error(std::string msg);
    void update_level(double peak0, double peak1, double rms0, double rms1, int ch);

    static bool set_pipeline_state(GstElement* pipeline,
                                    PlaybackState desired,
                                    SLError& out_err);
};

} // namespace straylight::player
