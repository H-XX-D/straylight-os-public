// apps/player/visualizer.h
// StrayLight Player — audio waveform / spectrum visualiser
#pragma once

#include "playback.h"

#include <array>
#include <cstdint>

struct ImVec2;
struct ImDrawList;

namespace straylight::player {

/// Visualiser modes.
enum class VisMode {
    Spectrum,   ///< Bar-spectrum from level element peak data
    VUMeter,    ///< Classic VU meter with peak hold
    Oscilloscope, ///< Simulated waveform from RMS history
};

/// Renders audio visualisation into the current ImGui window.
/// State is retained between frames for smooth animation.
class AudioVisualizer {
public:
    AudioVisualizer();

    /// Call each frame with the latest level data from the playback engine.
    void update(const PlaybackEngine::LevelData& level);

    /// Draw into the region [pos, pos + size].
    void draw(ImDrawList* draw_list, ImVec2 pos, ImVec2 size) const;

    VisMode mode() const { return mode_; }
    void    set_mode(VisMode m) { mode_ = m; }

private:
    VisMode mode_ = VisMode::Spectrum;

    // Smoothed peak/rms values
    static constexpr int kChannels = 2;
    float smoothed_peak_[kChannels] = {};
    float smoothed_rms_[kChannels]  = {};
    float peak_hold_[kChannels]     = {};
    int   peak_hold_timer_[kChannels] = {};

    // RMS history for oscilloscope
    static constexpr int kHistLen = 256;
    float rms_history_[kHistLen]  = {};
    int   history_head_           = 0;

    // Spectrum bars (simulated from peak/rms data with harmonic spread)
    static constexpr int kBars = 24;
    float bar_levels_[kBars]  = {};
    float bar_targets_[kBars] = {};

    void draw_spectrum   (ImDrawList* dl, ImVec2 pos, ImVec2 size) const;
    void draw_vu_meter   (ImDrawList* dl, ImVec2 pos, ImVec2 size) const;
    void draw_oscilloscope(ImDrawList* dl, ImVec2 pos, ImVec2 size) const;

    static float db_to_linear(float db);
    static uint32_t level_colour(float normalised); ///< green→yellow→red
};

} // namespace straylight::player
