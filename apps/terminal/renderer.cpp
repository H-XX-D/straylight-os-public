// apps/terminal/renderer.cpp
// ImGui-based terminal cell-grid renderer with scrollback and selection
#include "renderer.h"

#include <algorithm>
#include <cmath>

namespace straylight::terminal {

void Selection::normalize(int& sc, int& sr, int& ec, int& er) const {
    sr = start_row;
    sc = start_col;
    er = end_row;
    ec = end_col;
    if (sr > er || (sr == er && sc > ec)) {
        std::swap(sr, er);
        std::swap(sc, ec);
    }
}

bool Selection::contains(int col, int row) const {
    if (!active) return false;
    int sc, sr, ec, er;
    normalize(sc, sr, ec, er);
    if (row < sr || row > er) return false;
    if (row == sr && row == er) return col >= sc && col <= ec;
    if (row == sr) return col >= sc;
    if (row == er) return col <= ec;
    return true;
}

Renderer::Renderer() = default;

void Renderer::init(const std::string& /*font_family*/, float font_size) {
    font_size_ = font_size;
    // Calculate cell dimensions based on font metrics.
    // ImGui monospace: approximate width = font_size * 0.6, height = font_size * 1.2
    cell_width_ = std::round(font_size_ * 0.6f);
    cell_height_ = std::round(font_size_ * 1.2f);
}

ImU32 Renderer::to_imu32(uint32_t argb) const {
    // ARGB -> ImGui's ABGR format
    uint8_t a = static_cast<uint8_t>((argb >> 24) & 0xFF);
    uint8_t r = static_cast<uint8_t>((argb >> 16) & 0xFF);
    uint8_t g = static_cast<uint8_t>((argb >> 8) & 0xFF);
    uint8_t b = static_cast<uint8_t>(argb & 0xFF);
    return IM_COL32(r, g, b, a);
}

void Renderer::render_cell(ImDrawList* dl, float x, float y,
                            const Cell& cell, bool selected, bool is_cursor) {
    uint32_t fg = cell.attrs.fg_color;
    uint32_t bg = cell.attrs.bg_color;

    // Apply reverse video
    if (cell.attrs.reverse) {
        std::swap(fg, bg);
    }

    // Apply selection highlight
    if (selected) {
        // Invert colors for selection
        std::swap(fg, bg);
    }

    // Apply dim
    if (cell.attrs.dim) {
        uint8_t r = static_cast<uint8_t>((fg >> 16) & 0xFF);
        uint8_t g = static_cast<uint8_t>((fg >> 8) & 0xFF);
        uint8_t b = static_cast<uint8_t>(fg & 0xFF);
        fg = 0xFF000000u |
             (static_cast<uint32_t>(r / 2) << 16) |
             (static_cast<uint32_t>(g / 2) << 8) |
             static_cast<uint32_t>(b / 2);
    }

    // Draw background
    ImVec2 tl(x, y);
    ImVec2 br(x + cell_width_, y + cell_height_);
    dl->AddRectFilled(tl, br, to_imu32(bg));

    // Draw character
    if (cell.ch > U' ' && !cell.attrs.hidden) {
        // Convert char32_t to UTF-8 for ImGui
        char utf8_buf[5] = {};
        if (cell.ch < 0x80) {
            utf8_buf[0] = static_cast<char>(cell.ch);
        } else if (cell.ch < 0x800) {
            utf8_buf[0] = static_cast<char>(0xC0 | (cell.ch >> 6));
            utf8_buf[1] = static_cast<char>(0x80 | (cell.ch & 0x3F));
        } else if (cell.ch < 0x10000) {
            utf8_buf[0] = static_cast<char>(0xE0 | (cell.ch >> 12));
            utf8_buf[1] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F));
            utf8_buf[2] = static_cast<char>(0x80 | (cell.ch & 0x3F));
        } else {
            utf8_buf[0] = static_cast<char>(0xF0 | (cell.ch >> 18));
            utf8_buf[1] = static_cast<char>(0x80 | ((cell.ch >> 12) & 0x3F));
            utf8_buf[2] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F));
            utf8_buf[3] = static_cast<char>(0x80 | (cell.ch & 0x3F));
        }

        ImU32 fg_col = to_imu32(fg);

        // Bold text: draw twice with 1px offset for faux bold
        float text_y = y + (cell_height_ - font_size_) * 0.5f;
        dl->AddText(ImVec2(x, text_y), fg_col, utf8_buf);
        if (cell.attrs.bold) {
            dl->AddText(ImVec2(x + 1.0f, text_y), fg_col, utf8_buf);
        }

        // Underline
        if (cell.attrs.underline) {
            float uy = y + cell_height_ - 2.0f;
            dl->AddLine(ImVec2(x, uy), ImVec2(x + cell_width_, uy), fg_col);
        }

        // Strikethrough
        if (cell.attrs.strikethrough) {
            float sy = y + cell_height_ * 0.5f;
            dl->AddLine(ImVec2(x, sy), ImVec2(x + cell_width_, sy), fg_col);
        }

        // Italic: we can't truly italicize with ImGui default font,
        // but we mark it in case a custom font is loaded
    }

    // Draw cursor
    if (is_cursor) {
        render_cursor(dl, x, y, *(const Vte*)nullptr); // cursor drawn separately
    }
}

void Renderer::render_cursor(ImDrawList* dl, float x, float y,
                              const Vte& /*vte*/) {
    // Block cursor with blink
    blink_timer_ += ImGui::GetIO().DeltaTime;
    if (blink_timer_ > 0.5f) {
        blink_timer_ = 0.0f;
        blink_visible_ = !blink_visible_;
    }

    if (blink_visible_) {
        ImU32 cursor_color = IM_COL32(0, 255, 170, 200); // Cyberpunk green
        ImVec2 tl(x, y);
        ImVec2 br(x + cell_width_, y + cell_height_);
        dl->AddRectFilled(tl, br, cursor_color);
    }
}

std::pair<int, int> Renderer::pixel_to_grid(float px, float py,
                                              const Vte& vte) const {
    int col = static_cast<int>((px - padding_x_) / cell_width_);
    int vis_row = static_cast<int>((py - padding_y_) / cell_height_);

    col = std::clamp(col, 0, vte.cols() - 1);
    vis_row = std::clamp(vis_row, 0, vte.rows() - 1);

    // Convert to absolute row (scrollback + screen)
    int scrollback_size = static_cast<int>(vte.scrollback().size());
    int abs_row = scrollback_size - scroll_offset_ + vis_row;

    return {col, abs_row};
}

void Renderer::handle_mouse(Vte& vte, float window_x, float window_y) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    float local_x = mouse.x - window_x;
    float local_y = mouse.y - window_y;

    // Scroll with mouse wheel
    if (std::abs(io.MouseWheel) > 0.0f) {
        int delta = static_cast<int>(-io.MouseWheel * 3.0f);
        scroll_by(delta);
        int max_scroll = static_cast<int>(vte.scrollback().size());
        scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll);
    }

    // Selection via mouse drag
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto [col, row] = pixel_to_grid(local_x, local_y, vte);
        selection_.start_col = col;
        selection_.start_row = row;
        selection_.end_col = col;
        selection_.end_row = row;
        selection_.in_progress = true;
        selection_.active = false;
    }

    if (selection_.in_progress && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        auto [col, row] = pixel_to_grid(local_x, local_y, vte);
        selection_.end_col = col;
        selection_.end_row = row;
        selection_.active = true;
    }

    if (selection_.in_progress && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        selection_.in_progress = false;
        if (selection_.start_col == selection_.end_col &&
            selection_.start_row == selection_.end_row) {
            selection_.active = false; // Click without drag = no selection
        }
    }
}

std::string Renderer::get_selection_text(const Vte& vte) const {
    if (!selection_.active) return "";

    int sc, sr, ec, er;
    selection_.normalize(sc, sr, ec, er);

    int scrollback_size = static_cast<int>(vte.scrollback().size());
    std::string result;

    for (int row = sr; row <= er; ++row) {
        const Row* row_data = nullptr;
        if (row < scrollback_size) {
            row_data = &vte.scrollback()[static_cast<size_t>(row)];
        } else {
            int screen_row = row - scrollback_size;
            if (screen_row >= 0 && screen_row < vte.rows()) {
                row_data = &vte.screen()[static_cast<size_t>(screen_row)];
            }
        }

        if (!row_data) continue;

        int col_start = (row == sr) ? sc : 0;
        int col_end = (row == er) ? ec : vte.cols() - 1;

        for (int c = col_start; c <= col_end && c < static_cast<int>(row_data->size()); ++c) {
            char32_t ch = (*row_data)[static_cast<size_t>(c)].ch;
            // Convert char32_t to UTF-8
            if (ch < 0x80) {
                result += static_cast<char>(ch);
            } else if (ch < 0x800) {
                result += static_cast<char>(0xC0 | (ch >> 6));
                result += static_cast<char>(0x80 | (ch & 0x3F));
            } else if (ch < 0x10000) {
                result += static_cast<char>(0xE0 | (ch >> 12));
                result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                result += static_cast<char>(0xF0 | (ch >> 18));
                result += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }

        if (row < er) {
            // Trim trailing spaces and add newline
            while (!result.empty() && result.back() == ' ') {
                result.pop_back();
            }
            result += '\n';
        }
    }

    return result;
}

void Renderer::scroll_by(int delta) {
    scroll_offset_ += delta;
}

void Renderer::render(Vte& vte, float window_width, float window_height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 win_pos = ImGui::GetCursorScreenPos();

    // Clamp scroll offset
    int max_scroll = static_cast<int>(vte.scrollback().size());
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll);

    // If user scrolls to bottom, auto-follow
    if (scroll_offset_ == 0) {
        // Already at bottom
    }

    int scrollback_size = static_cast<int>(vte.scrollback().size());
    int first_visible_abs_row = scrollback_size - scroll_offset_;

    // Fill background
    dl->AddRectFilled(
        win_pos,
        ImVec2(win_pos.x + window_width, win_pos.y + window_height),
        to_imu32(vte.default_bg()));

    // Render visible rows
    for (int vis_row = 0; vis_row < vte.rows(); ++vis_row) {
        int abs_row = first_visible_abs_row + vis_row;
        const Row* row_data = nullptr;

        if (abs_row < 0) {
            continue;
        } else if (abs_row < scrollback_size) {
            row_data = &vte.scrollback()[static_cast<size_t>(abs_row)];
        } else {
            int screen_row = abs_row - scrollback_size;
            if (screen_row >= 0 && screen_row < static_cast<int>(vte.screen().size())) {
                row_data = &vte.screen()[static_cast<size_t>(screen_row)];
            }
        }

        if (!row_data) continue;

        float y = win_pos.y + padding_y_ + vis_row * cell_height_;

        for (int col = 0; col < vte.cols() && col < static_cast<int>(row_data->size()); ++col) {
            float x = win_pos.x + padding_x_ + col * cell_width_;
            const Cell& cell = (*row_data)[static_cast<size_t>(col)];
            bool selected = selection_.contains(col, abs_row);

            // Check if this is the cursor position
            bool is_cursor = false;
            if (scroll_offset_ == 0 && vte.cursor_visible()) {
                int screen_row = abs_row - scrollback_size;
                if (screen_row == vte.cursor_row() && col == vte.cursor_col()) {
                    is_cursor = true;
                }
            }

            // Draw cell
            uint32_t fg = cell.attrs.fg_color;
            uint32_t bg = cell.attrs.bg_color;

            if (cell.attrs.reverse) std::swap(fg, bg);
            if (selected) std::swap(fg, bg);
            if (cell.attrs.dim) {
                uint8_t r = static_cast<uint8_t>((fg >> 16) & 0xFF);
                uint8_t g = static_cast<uint8_t>((fg >> 8) & 0xFF);
                uint8_t b = static_cast<uint8_t>(fg & 0xFF);
                fg = 0xFF000000u |
                     (static_cast<uint32_t>(r / 2) << 16) |
                     (static_cast<uint32_t>(g / 2) << 8) |
                     static_cast<uint32_t>(b / 2);
            }

            // Background
            dl->AddRectFilled(
                ImVec2(x, y),
                ImVec2(x + cell_width_, y + cell_height_),
                to_imu32(bg));

            // Character
            if (cell.ch > U' ' && !cell.attrs.hidden) {
                char utf8_buf[5] = {};
                if (cell.ch < 0x80) {
                    utf8_buf[0] = static_cast<char>(cell.ch);
                } else if (cell.ch < 0x800) {
                    utf8_buf[0] = static_cast<char>(0xC0 | (cell.ch >> 6));
                    utf8_buf[1] = static_cast<char>(0x80 | (cell.ch & 0x3F));
                } else if (cell.ch < 0x10000) {
                    utf8_buf[0] = static_cast<char>(0xE0 | (cell.ch >> 12));
                    utf8_buf[1] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F));
                    utf8_buf[2] = static_cast<char>(0x80 | (cell.ch & 0x3F));
                } else {
                    utf8_buf[0] = static_cast<char>(0xF0 | (cell.ch >> 18));
                    utf8_buf[1] = static_cast<char>(0x80 | ((cell.ch >> 12) & 0x3F));
                    utf8_buf[2] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F));
                    utf8_buf[3] = static_cast<char>(0x80 | (cell.ch & 0x3F));
                }

                ImU32 fg_col = to_imu32(fg);
                float text_y = y + (cell_height_ - font_size_) * 0.5f;
                dl->AddText(ImVec2(x, text_y), fg_col, utf8_buf);
                if (cell.attrs.bold) {
                    dl->AddText(ImVec2(x + 1.0f, text_y), fg_col, utf8_buf);
                }

                if (cell.attrs.underline) {
                    float uy = y + cell_height_ - 2.0f;
                    dl->AddLine(ImVec2(x, uy), ImVec2(x + cell_width_, uy), fg_col);
                }
                if (cell.attrs.strikethrough) {
                    float sy = y + cell_height_ * 0.5f;
                    dl->AddLine(ImVec2(x, sy), ImVec2(x + cell_width_, sy), fg_col);
                }
            }

            // Cursor overlay
            if (is_cursor) {
                blink_timer_ += ImGui::GetIO().DeltaTime;
                if (blink_timer_ > 0.5f) {
                    blink_timer_ -= 0.5f;
                    blink_visible_ = !blink_visible_;
                }
                if (blink_visible_) {
                    ImU32 cursor_col = IM_COL32(0, 255, 170, 180);
                    dl->AddRectFilled(
                        ImVec2(x, y),
                        ImVec2(x + cell_width_, y + cell_height_),
                        cursor_col);
                    // Redraw char in inverted color over cursor
                    if (cell.ch > U' ') {
                        char utf8_buf[5] = {};
                        if (cell.ch < 0x80) {
                            utf8_buf[0] = static_cast<char>(cell.ch);
                        } else if (cell.ch < 0x800) {
                            utf8_buf[0] = static_cast<char>(0xC0 | (cell.ch >> 6));
                            utf8_buf[1] = static_cast<char>(0x80 | (cell.ch & 0x3F));
                        } else if (cell.ch < 0x10000) {
                            utf8_buf[0] = static_cast<char>(0xE0 | (cell.ch >> 12));
                            utf8_buf[1] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F));
                            utf8_buf[2] = static_cast<char>(0x80 | (cell.ch & 0x3F));
                        } else {
                            utf8_buf[0] = static_cast<char>(0xF0 | (cell.ch >> 18));
                            utf8_buf[1] = static_cast<char>(0x80 | ((cell.ch >> 12) & 0x3F));
                            utf8_buf[2] = static_cast<char>(0x80 | ((cell.ch >> 6) & 0x3F));
                            utf8_buf[3] = static_cast<char>(0x80 | (cell.ch & 0x3F));
                        }
                        float text_y = y + (cell_height_ - font_size_) * 0.5f;
                        dl->AddText(ImVec2(x, text_y),
                                    to_imu32(vte.default_bg()), utf8_buf);
                    }
                }
            }
        }
    }

    // Scrollbar indicator
    if (max_scroll > 0) {
        float total_lines = static_cast<float>(max_scroll + vte.rows());
        float visible_frac = static_cast<float>(vte.rows()) / total_lines;
        float scroll_frac = static_cast<float>(max_scroll - scroll_offset_) / total_lines;

        float sb_x = win_pos.x + window_width - 6.0f;
        float sb_h = window_height * visible_frac;
        float sb_y = win_pos.y + window_height * scroll_frac;

        dl->AddRectFilled(
            ImVec2(sb_x, sb_y),
            ImVec2(sb_x + 4.0f, sb_y + std::max(sb_h, 20.0f)),
            IM_COL32(100, 100, 100, 150),
            2.0f);
    }

    vte.mark_clean();
}

} // namespace straylight::terminal
