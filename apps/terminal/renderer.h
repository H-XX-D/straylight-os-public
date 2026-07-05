// apps/terminal/renderer.h
// ImGui-based terminal renderer with scrollback and selection support
#pragma once

#include "vte.h"

#include <imgui.h>

#include <string>
#include <utility>

namespace straylight::terminal {

/// Selection state for copy support.
struct Selection {
    bool active = false;
    bool in_progress = false; // mouse button held
    int start_col = 0;
    int start_row = 0; // row index into combined scrollback+screen
    int end_col = 0;
    int end_row = 0;

    /// Normalize so start <= end.
    void normalize(int& sc, int& sr, int& ec, int& er) const;

    /// Check if a cell at (col, row) is within the selection.
    [[nodiscard]] bool contains(int col, int row) const;
};

/// Renders the terminal grid using ImGui draw lists.
class Renderer {
public:
    Renderer();

    /// Initialize the renderer with font settings.
    void init(const std::string& font_family, float font_size);

    /// Render the terminal state.
    void render(Vte& vte, float window_width, float window_height);

    /// Handle mouse input for selection.
    void handle_mouse(Vte& vte, float window_x, float window_y);

    /// Get the selected text.
    [[nodiscard]] std::string get_selection_text(const Vte& vte) const;

    /// Get/set scroll offset (lines scrolled back from bottom).
    [[nodiscard]] int scroll_offset() const { return scroll_offset_; }
    void set_scroll_offset(int offset) { scroll_offset_ = offset; }
    void scroll_by(int delta);

    /// Get cell dimensions.
    [[nodiscard]] float cell_width() const { return cell_width_; }
    [[nodiscard]] float cell_height() const { return cell_height_; }

    /// Get the current selection.
    [[nodiscard]] const Selection& selection() const { return selection_; }

    /// Clear selection.
    void clear_selection() { selection_ = {}; }

    /// Check if there's an active selection.
    [[nodiscard]] bool has_selection() const { return selection_.active; }

private:
    ImU32 to_imu32(uint32_t argb) const;
    void render_cell(ImDrawList* dl, float x, float y,
                     const Cell& cell, bool selected, bool is_cursor);
    void render_cursor(ImDrawList* dl, float x, float y,
                       const Vte& vte);

    // Convert pixel coordinates to grid coordinates (in combined
    // scrollback+screen space).
    std::pair<int, int> pixel_to_grid(float px, float py,
                                       const Vte& vte) const;

    float cell_width_ = 8.0f;
    float cell_height_ = 16.0f;
    float font_size_ = 14.0f;
    int scroll_offset_ = 0;
    float padding_x_ = 4.0f;
    float padding_y_ = 2.0f;

    Selection selection_{};

    // Cursor blink
    float blink_timer_ = 0.0f;
    bool blink_visible_ = true;
};

} // namespace straylight::terminal
