// apps/image_viewer/viewer.h
// Pan/zoom image canvas with fit-to-window, actual-size, mouse drag, scroll zoom.
#pragma once

#include "loader.h"

#include <imgui.h>
#include <cstdint>

namespace straylight::viewer {

/// Zoom / pan state for a single image canvas.
class ImageCanvas {
public:
    /// Reset view to fit the image inside `canvas_size`.
    void fit_to_window(ImVec2 canvas_size);

    /// Reset zoom to 1:1 (actual pixels), centred.
    void actual_size(ImVec2 canvas_size);

    /// Process mouse drag (call each frame with ImGui mouse delta and scroll).
    void process_input(ImVec2 canvas_pos, ImVec2 canvas_size);

    /// Draw the image centred at current pan/zoom into [canvas_pos, canvas_pos+canvas_size].
    /// `asset` must be valid.
    void draw(ImDrawList* dl, ImVec2 canvas_pos, ImVec2 canvas_size,
              const ImageAsset& asset) const;

    float zoom() const { return zoom_; }
    ImVec2 pan() const { return pan_; }

    void set_zoom(float z) { zoom_ = std::max(kMinZoom, std::min(kMaxZoom, z)); }

private:
    static constexpr float kMinZoom  = 0.01f;
    static constexpr float kMaxZoom  = 64.0f;
    static constexpr float kZoomStep = 0.12f; // fraction per scroll tick

    float  zoom_         = 1.0f;
    ImVec2 pan_          = {0.0f, 0.0f}; ///< offset from canvas centre to image centre (px)
    bool   dragging_     = false;
};

} // namespace straylight::viewer
