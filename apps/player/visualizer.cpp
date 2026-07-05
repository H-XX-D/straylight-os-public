// apps/player/visualizer.cpp
// StrayLight Player — audio visualiser (spectrum / VU / oscilloscope)
#include "visualizer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace straylight::player {

namespace {
constexpr float kSmoothing      = 0.15f;
constexpr float kPeakHoldFrames = 45.0f;
constexpr float kDecayPerFrame  = 0.012f;

uint32_t hsv_to_rgba(float h, float s, float v) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    int   i = static_cast<int>(h * 6.0f);
    float f = h * 6.0f - static_cast<float>(i);
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    auto byte = [](float x) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(x * 255.0f, 0.0f, 255.0f));
    };
    return IM_COL32(byte(r), byte(g), byte(b), 255);
}
} // namespace

AudioVisualizer::AudioVisualizer() {
    std::fill(std::begin(bar_levels_),  std::end(bar_levels_),  0.0f);
    std::fill(std::begin(bar_targets_), std::end(bar_targets_), 0.0f);
    std::fill(std::begin(rms_history_), std::end(rms_history_), 0.0f);
}

float AudioVisualizer::db_to_linear(float db) {
    return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

uint32_t AudioVisualizer::level_colour(float n) {
    float hue = 0.33f * (1.0f - n);
    return hsv_to_rgba(hue, 0.9f, 1.0f);
}

void AudioVisualizer::update(const PlaybackEngine::LevelData& level) {
    for (int ch = 0; ch < kChannels; ++ch) {
        const float peak_lin = db_to_linear(static_cast<float>(level.peak_db[ch]));
        const float rms_lin  = db_to_linear(static_cast<float>(level.rms_db[ch]));

        smoothed_peak_[ch] = smoothed_peak_[ch] * (1.0f - kSmoothing) + peak_lin * kSmoothing;
        smoothed_rms_[ch]  = smoothed_rms_[ch]  * (1.0f - kSmoothing) + rms_lin  * kSmoothing;

        if (peak_lin >= peak_hold_[ch]) {
            peak_hold_[ch]       = peak_lin;
            peak_hold_timer_[ch] = static_cast<int>(kPeakHoldFrames);
        } else {
            if (peak_hold_timer_[ch] > 0) {
                --peak_hold_timer_[ch];
            } else {
                peak_hold_[ch] -= kDecayPerFrame;
                if (peak_hold_[ch] < 0.0f) peak_hold_[ch] = 0.0f;
            }
        }
    }

    const float avg_rms = (smoothed_rms_[0] + smoothed_rms_[1]) * 0.5f;
    rms_history_[history_head_] = avg_rms;
    history_head_ = (history_head_ + 1) % kHistLen;

    const float base_level = (smoothed_peak_[0] + smoothed_peak_[1]) * 0.5f;
    for (int b = 0; b < kBars; ++b) {
        const float freq_norm = static_cast<float>(b) / static_cast<float>(kBars - 1);
        const float roll_off  = std::exp(-2.5f * freq_norm);
        const float variation = 0.8f + 0.4f * std::abs(
            std::sin(static_cast<float>(b * 7 + 3)));
        bar_targets_[b] = base_level * roll_off * variation;

        if (bar_targets_[b] > bar_levels_[b])
            bar_levels_[b] += (bar_targets_[b] - bar_levels_[b]) * 0.4f;
        else
            bar_levels_[b] += (bar_targets_[b] - bar_levels_[b]) * 0.08f;

        bar_levels_[b] = std::clamp(bar_levels_[b], 0.0f, 1.0f);
    }
}

void AudioVisualizer::draw(ImDrawList* dl, ImVec2 pos, ImVec2 size) const {
    switch (mode_) {
        case VisMode::Spectrum:      draw_spectrum   (dl, pos, size); break;
        case VisMode::VUMeter:       draw_vu_meter   (dl, pos, size); break;
        case VisMode::Oscilloscope:  draw_oscilloscope(dl, pos, size); break;
    }
}

void AudioVisualizer::draw_spectrum(ImDrawList* dl, ImVec2 pos, ImVec2 size) const {
    constexpr float kGap = 2.0f;
    const float bar_w = (size.x - kGap * (kBars - 1)) / static_cast<float>(kBars);
    const float max_h = size.y - 4.0f;

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      IM_COL32(10, 10, 20, 200));

    for (int b = 0; b < kBars; ++b) {
        const float x0    = pos.x + static_cast<float>(b) * (bar_w + kGap);
        const float bar_h = bar_levels_[b] * max_h;
        const float y0    = pos.y + size.y - bar_h;
        const float y1    = pos.y + size.y;
        const float x1    = x0 + bar_w;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), level_colour(bar_levels_[b]));
    }

    // Peak hold markers on first two bars (L/R)
    for (int ch = 0; ch < kChannels; ++ch) {
        if (peak_hold_[ch] > 0.01f) {
            const float x0 = pos.x + static_cast<float>(ch) * (bar_w + kGap);
            const float ph_y = pos.y + size.y - peak_hold_[ch] * max_h - 2.0f;
            dl->AddRectFilled(ImVec2(x0, ph_y), ImVec2(x0 + bar_w, ph_y + 2.0f),
                              IM_COL32(255, 255, 255, 220));
        }
    }
}

void AudioVisualizer::draw_vu_meter(ImDrawList* dl, ImVec2 pos, ImVec2 size) const {
    const float lane_w = (size.x - 6.0f) * 0.5f;
    const float max_h  = size.y - 4.0f;

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      IM_COL32(10, 10, 20, 200));

    for (int ch = 0; ch < kChannels; ++ch) {
        const float x0 = pos.x + static_cast<float>(ch) * (lane_w + 6.0f);
        const float y1 = pos.y + size.y;

        const float rms_h  = smoothed_rms_[ch]  * max_h;
        const float peak_h = smoothed_peak_[ch] * max_h;

        dl->AddRectFilled(ImVec2(x0, y1 - rms_h),  ImVec2(x0 + lane_w, y1),
                          level_colour(smoothed_rms_[ch]));
        dl->AddRectFilled(ImVec2(x0, y1 - peak_h), ImVec2(x0 + lane_w, y1),
                          IM_COL32(255, 255, 255, 30));

        if (peak_hold_[ch] > 0.01f) {
            const float ph_y = y1 - peak_hold_[ch] * max_h - 2.0f;
            dl->AddLine(ImVec2(x0, ph_y), ImVec2(x0 + lane_w, ph_y),
                        IM_COL32(255, 80, 80, 240), 2.0f);
        }

        // -6 dB and -20 dB reference lines
        dl->AddLine(ImVec2(x0, y1 - 0.50f * max_h), ImVec2(x0 + lane_w, y1 - 0.50f * max_h),
                    IM_COL32(80, 80, 80, 160), 1.0f);
        dl->AddLine(ImVec2(x0, y1 - 0.17f * max_h), ImVec2(x0 + lane_w, y1 - 0.17f * max_h),
                    IM_COL32(60, 60, 60, 120), 1.0f);
    }
}

void AudioVisualizer::draw_oscilloscope(ImDrawList* dl, ImVec2 pos, ImVec2 size) const {
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      IM_COL32(5, 5, 15, 200));

    for (int g = 1; g < 4; ++g) {
        const float gy = pos.y + size.y * static_cast<float>(g) / 4.0f;
        dl->AddLine(ImVec2(pos.x, gy), ImVec2(pos.x + size.x, gy),
                    IM_COL32(30, 30, 50, 120), 1.0f);
    }

    const float cx = size.x / static_cast<float>(kHistLen);
    constexpr uint32_t kLineCol = IM_COL32(80, 220, 120, 240);

    for (int i = 0; i < kHistLen - 1; ++i) {
        const int i0 = (history_head_ + i)     % kHistLen;
        const int i1 = (history_head_ + i + 1) % kHistLen;
        const float v0 = rms_history_[i0];
        const float v1 = rms_history_[i1];
        const float y0 = pos.y + size.y * 0.5f - v0 * size.y * 0.45f;
        const float y1 = pos.y + size.y * 0.5f - v1 * size.y * 0.45f;
        const float x0 = pos.x + static_cast<float>(i)     * cx;
        const float x1 = pos.x + static_cast<float>(i + 1) * cx;
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), kLineCol, 1.5f);
    }
}

} // namespace straylight::player
