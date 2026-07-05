// apps/player/playback.cpp
// StrayLight Player — GStreamer pipeline implementation
#include "playback.h"

#include <straylight/log.h>

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace straylight::player {

// ---------------------------------------------------------------------------
// Static init
// ---------------------------------------------------------------------------

void PlaybackEngine::gst_init_once(int* argc, char*** argv) {
    gst_init(argc, argv);
    gst_pb_utils_init();
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

PlaybackEngine::PlaybackEngine() {
    // Create playbin3 for full-featured playback
    pipeline_ = gst_element_factory_make("playbin3", "playbin");
    if (!pipeline_) {
        // Fall back to playbin
        pipeline_ = gst_element_factory_make("playbin", "playbin");
    }
    if (!pipeline_) {
        SL_ERROR("Cannot create GStreamer playbin element");
        return;
    }

    // Build a custom audio sink chain: audioconvert ! audioresample ! level ! autoaudiosink
    GstElement* audio_sink    = gst_element_factory_make("autoaudiosink",  "audio_out");
    GstElement* audio_convert = gst_element_factory_make("audioconvert",   "aconv");
    GstElement* audio_resample= gst_element_factory_make("audioresample",  "aresample");
    level_elem_               = gst_element_factory_make("level",          "level");

    if (audio_sink && audio_convert && audio_resample && level_elem_) {
        GstElement* bin = gst_bin_new("audio_bin");
        gst_bin_add_many(GST_BIN(bin), audio_convert, audio_resample,
                         level_elem_, audio_sink, nullptr);
        gst_element_link_many(audio_convert, audio_resample, level_elem_, audio_sink, nullptr);

        // Ghost pad
        GstPad* pad = gst_element_get_static_pad(audio_convert, "sink");
        gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
        gst_object_unref(pad);

        // Configure level: post messages every 50ms
        g_object_set(level_elem_,
                     "post-messages",      TRUE,
                     "interval",           (guint64)50000000, // 50ms in nanoseconds
                     "peak-falloff",       10.0,
                     "peak-ttl",           (guint64)300000000,
                     nullptr);

        g_object_set(pipeline_, "audio-sink", bin, nullptr);
    } else {
        // Cleanup any partial allocations
        if (audio_sink)     gst_object_unref(audio_sink);
        if (audio_convert)  gst_object_unref(audio_convert);
        if (audio_resample) gst_object_unref(audio_resample);
        if (level_elem_)  { gst_object_unref(level_elem_); level_elem_ = nullptr; }
    }

    bus_ = gst_element_get_bus(pipeline_);

    // Default volume
    g_object_set(pipeline_, "volume", volume_, nullptr);
}

PlaybackEngine::~PlaybackEngine() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    if (bus_) {
        gst_object_unref(bus_);
        bus_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

Result<void, SLError> PlaybackEngine::open(std::string_view uri) {
    if (!pipeline_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "GStreamer pipeline not created"});
    }

    // Stop any existing playback
    gst_element_set_state(pipeline_, GST_STATE_NULL);

    current_uri_ = std::string(uri);
    // Prepend file:// if it looks like an absolute filesystem path
    if (!current_uri_.starts_with("file://") &&
        !current_uri_.starts_with("http://") &&
        !current_uri_.starts_with("https://") &&
        current_uri_.starts_with('/')) {
        current_uri_ = "file://" + current_uri_;
    }

    g_object_set(pipeline_, "uri", current_uri_.c_str(), nullptr);
    g_object_set(pipeline_, "volume", volume_, nullptr);

    // Pre-roll to PAUSED to obtain duration
    gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    // Wait briefly for state change (non-blocking approach — let pump_messages handle it)

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = PlaybackState::Paused;
    }

    SL_INFO("Opened URI: {}", current_uri_);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

Result<void, SLError> PlaybackEngine::play() {
    if (!pipeline_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pipeline not ready"});
    }

    SLError err;
    if (!set_pipeline_state(pipeline_, PlaybackState::Playing, err)) {
        return Result<void, SLError>::error(err);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = PlaybackState::Playing;
    return Result<void, SLError>::ok();
}

Result<void, SLError> PlaybackEngine::pause() {
    if (!pipeline_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pipeline not ready"});
    }

    SLError err;
    if (!set_pipeline_state(pipeline_, PlaybackState::Paused, err)) {
        return Result<void, SLError>::error(err);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = PlaybackState::Paused;
    return Result<void, SLError>::ok();
}

Result<void, SLError> PlaybackEngine::stop() {
    if (!pipeline_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pipeline not ready"});
    }

    gst_element_set_state(pipeline_, GST_STATE_NULL);

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = PlaybackState::Stopped;
    return Result<void, SLError>::ok();
}

Result<void, SLError> PlaybackEngine::seek(double seconds) {
    if (!pipeline_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pipeline not ready"});
    }

    const gint64 pos_ns = static_cast<gint64>(seconds * 1e9);
    if (!gst_element_seek_simple(pipeline_, GST_FORMAT_TIME,
                                  static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                             GST_SEEK_FLAG_KEY_UNIT),
                                  pos_ns)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Seek failed"});
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

double PlaybackEngine::position() const {
    if (!pipeline_) return 0.0;
    gint64 pos = 0;
    if (!gst_element_query_position(pipeline_, GST_FORMAT_TIME, &pos)) return 0.0;
    return static_cast<double>(pos) / 1e9;
}

double PlaybackEngine::duration() const {
    if (!pipeline_) return -1.0;
    gint64 dur = -1;
    if (!gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &dur)) return -1.0;
    return static_cast<double>(dur) / 1e9;
}

PlaybackState PlaybackEngine::state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void PlaybackEngine::set_volume(double v) {
    volume_ = std::clamp(v, 0.0, 1.0);
    if (pipeline_) g_object_set(pipeline_, "volume", volume_, nullptr);
}

double PlaybackEngine::volume() const { return volume_; }

void PlaybackEngine::set_mute(bool m) {
    muted_ = m;
    if (pipeline_) g_object_set(pipeline_, "mute", static_cast<gboolean>(m), nullptr);
}

bool PlaybackEngine::muted() const { return muted_; }

// ---------------------------------------------------------------------------
// Level data
// ---------------------------------------------------------------------------

PlaybackEngine::LevelData PlaybackEngine::level() const {
    std::lock_guard<std::mutex> lock(level_mutex_);
    return level_data_;
}

void PlaybackEngine::update_level(double peak0, double peak1,
                                   double rms0,  double rms1, int ch) {
    std::lock_guard<std::mutex> lock(level_mutex_);
    level_data_.channels    = ch;
    level_data_.peak_db[0]  = peak0;
    level_data_.peak_db[1]  = (ch > 1) ? peak1 : peak0;
    level_data_.rms_db[0]   = rms0;
    level_data_.rms_db[1]   = (ch > 1) ? rms1 : rms0;
}

// ---------------------------------------------------------------------------
// Bus message pump
// ---------------------------------------------------------------------------

void PlaybackEngine::pump_messages() {
    if (!bus_) return;

    GstMessage* msg = nullptr;
    while ((msg = gst_bus_pop(bus_)) != nullptr) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            handle_eos();
            break;

        case GST_MESSAGE_ERROR: {
            GError* err  = nullptr;
            gchar*  dbg  = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::string msg_str = err ? err->message : "Unknown error";
            if (dbg) msg_str += std::string(" [") + dbg + "]";
            g_error_free(err);
            g_free(dbg);
            handle_error(std::move(msg_str));
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_s, new_s, pending;
            gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                switch (new_s) {
                case GST_STATE_PLAYING: state_ = PlaybackState::Playing;  break;
                case GST_STATE_PAUSED:  state_ = PlaybackState::Paused;   break;
                case GST_STATE_NULL:
                case GST_STATE_READY:   state_ = PlaybackState::Stopped;  break;
                default: break;
                }
            }
            break;
        }

        case GST_MESSAGE_ELEMENT: {
            // Level messages from the level element
            const GstStructure* s = gst_message_get_structure(msg);
            if (s && strcmp(gst_structure_get_name(s), "level") == 0) {
                // peak and rms are GValueArray inside GstValueList
                const GValue* peak_val = gst_structure_get_value(s, "peak");
                const GValue* rms_val  = gst_structure_get_value(s, "rms");

                if (peak_val && rms_val) {
                    const int n = static_cast<int>(gst_value_list_get_size(peak_val));
                    double peak0 = -100.0, peak1 = -100.0;
                    double rms0  = -100.0, rms1  = -100.0;

                    if (n > 0) {
                        peak0 = g_value_get_double(gst_value_list_get_value(peak_val, 0));
                        rms0  = g_value_get_double(gst_value_list_get_value(rms_val,  0));
                    }
                    if (n > 1) {
                        peak1 = g_value_get_double(gst_value_list_get_value(peak_val, 1));
                        rms1  = g_value_get_double(gst_value_list_get_value(rms_val,  1));
                    }
                    update_level(peak0, peak1, rms0, rms1, n);
                }
            }
            break;
        }

        default:
            break;
        }

        gst_message_unref(msg);
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void PlaybackEngine::handle_eos() {
    SL_INFO("End of stream");
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = PlaybackState::Stopped;
    }
    if (eos_cb_) eos_cb_();
}

void PlaybackEngine::handle_error(std::string msg) {
    SL_ERROR("GStreamer error: {}", msg);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = PlaybackState::Stopped;
    }
    if (error_cb_) error_cb_(std::move(msg));
}

bool PlaybackEngine::set_pipeline_state(GstElement* pipeline,
                                         PlaybackState desired,
                                         SLError& out_err) {
    GstState gst_state = GST_STATE_NULL;
    switch (desired) {
    case PlaybackState::Playing: gst_state = GST_STATE_PLAYING; break;
    case PlaybackState::Paused:  gst_state = GST_STATE_PAUSED;  break;
    case PlaybackState::Stopped: gst_state = GST_STATE_NULL;    break;
    }

    const GstStateChangeReturn ret = gst_element_set_state(pipeline, gst_state);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        out_err = SLError{SLErrorCode::Internal, "GStreamer state change failed"};
        return false;
    }
    return true;
}

} // namespace straylight::player
