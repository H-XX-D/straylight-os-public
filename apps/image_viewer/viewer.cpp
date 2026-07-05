// apps/image_viewer/viewer.cpp
// Pan/zoom image canvas implementation.
#include "viewer.h"

#include <algorithm>
#include <cmath>

namespace straylight::viewer {

// ---------------------------------------------------------------------------
// fit_to_window
// ---------------------------------------------------------------------------

void ImageCanvas::fit_to_window(ImVec2 canvas_size) {
    // pan stays 0 — image centred
    pan_ = {0.0f, 0.0f};
    zoom_ = 1.0f; // will be overridden by draw() for auto-fit, but set a default
}

void ImageCanvas::actual_size(ImVec2 canvas_size) {
    zoom_ = 1.0f;
    pan_  = {0.0f, 0.0f};
}

// ---------------------------------------------------------------------------
// process_input
// ---------------------------------------------------------------------------

void ImageCanvas::process_input(ImVec2 canvas_pos, ImVec2 canvas_size) {
    ImGuiIO& io = ImGui::GetIO();

    // Check if mouse is inside the canvas
    const bool hovered =
        io.MousePos.x >= canvas_pos.x && io.MousePos.x < canvas_pos.x + canvas_size.x &&
        io.MousePos.y >= canvas_pos.y && io.MousePos.y < canvas_pos.y + canvas_size.y;

    if (!hovered) return;

    // Scroll to zoom (centred on mouse cursor)
    if (std::abs(io.MouseWheel) > 1e-4f) {
        const float zoom_before = zoom_;
        const float factor = 1.0f + kZoomStep * io.MouseWheel;
        zoom_ = std::max(kMinZoom, std::min(kMaxZoom, zoom_ * factor));

        // Adjust pan so zoom is centred on the cursor
        const float cx = canvas_pos.x + canvas_size.x * 0.5f;
        const float cy = canvas_pos.y + canvas_size.y * 0.5f;
        const float mx = io.MousePos.x - cx;  // cursor offset from canvas centre
        const float my = io.MousePos.y - cy;
        // old image position at cursor = pan + cursor_offset / zoom_before
        // new image position at cursor = pan_new + cursor_offset / zoom_
        // => pan_new = pan + cursor_offset * (1/zoom_before - 1/zoom_)
        pan_.x += mx * (1.0f / zoom_before - 1.0f / zoom_);
        pan_.y += my * (1.0f / zoom_before - 1.0f / zoom_);
    }

    // Drag to pan
    if (io.MouseDown[0]) {
        if (!dragging_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            dragging_ = true;
        }
        if (dragging_) {
            pan_.x += io.MouseDelta.x;
            pan_.y += io.MouseDelta.y;
        }
    } else {
        dragging_ = false;
    }
}

// ---------------------------------------------------------------------------
// draw
// ---------------------------------------------------------------------------

void ImageCanvas::draw(ImDrawList* dl, ImVec2 canvas_pos, ImVec2 canvas_size,
                       const ImageAsset& asset) const {
    if (!asset.valid()) return;

    // Fill background
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(20, 20, 20, 255));

    // Compute displayed image dimensions
    const float img_aspect = asset.aspect();
    const float cv_aspect  = (canvas_size.y > 0) ? canvas_size.x / canvas_size.y : 1.0f;

    // Base "fit" zoom
    float fit_zoom = 1.0f;
    if (img_aspect > cv_aspect) {
        fit_zoom = canvas_size.x / static_cast<float>(asset.width);
    } else {
        fit_zoom = canvas_size.y / static_cast<float>(asset.height);
    }

    const float effective_zoom = (zoom_ == 1.0f && std::abs(pan_.x) < 1e-3f &&
                                   std::abs(pan_.y) < 1e-3f)
                                  ? fit_zoom
                                  : zoom_;

    const float disp_w = static_cast<float>(asset.width)  * effective_zoom;
    const float disp_h = static_cast<float>(asset.height) * effective_zoom;

    // Centre of canvas
    const float cx = canvas_pos.x + canvas_size.x * 0.5f;
    const float cy = canvas_pos.y + canvas_size.y * 0.5f;

    // Image rect: centre + pan
    const float x0 = cx + pan_.x - disp_w * 0.5f;
    const float y0 = cy + pan_.y - disp_h * 0.5f;
    const float x1 = x0 + disp_w;
    const float y1 = y0 + disp_h;

    // Clip to canvas
    dl->PushClipRect(canvas_pos,
                     ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                     true);

    dl->AddImage(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(asset.texture_id)),
                 ImVec2(x0, y0), ImVec2(x1, y1));

    dl->PopClipRect();

    // Checkerboard background hint (draws behind — we already drew solid bg)
    // Optionally draw a thin border around the image
    dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1),
                IM_COL32(60, 60, 60, 120), 0.0f, 0, 1.0f);
}

} // namespace straylight::viewer
