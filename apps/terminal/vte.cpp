// apps/terminal/vte.cpp
// VT100/xterm escape sequence parser — full state machine implementation
#include "vte.h"

#include <algorithm>
#include <cstring>

namespace straylight::terminal {

// Standard 16-color ANSI palette (cyberpunk-themed)
static constexpr std::array<uint32_t, 16> kAnsiColors = {{
    0xFF1A1A2E, // 0: Black
    0xFFE74C3C, // 1: Red
    0xFF2ECC71, // 2: Green
    0xFFF39C12, // 3: Yellow
    0xFF3498DB, // 4: Blue
    0xFF9B59B6, // 5: Magenta
    0xFF1ABC9C, // 6: Cyan
    0xFFCCCCCC, // 7: White
    0xFF555555, // 8: Bright Black
    0xFFFF6B6B, // 9: Bright Red
    0xFF55EFC4, // 10: Bright Green
    0xFFFFD93D, // 11: Bright Yellow
    0xFF74B9FF, // 12: Bright Blue
    0xFFDDA0DD, // 13: Bright Magenta
    0xFF48DBFB, // 14: Bright Cyan
    0xFFFFFFFF, // 15: Bright White
}};

uint32_t Vte::color_from_index(int idx) {
    if (idx < 0) return 0xFFCCCCCC;
    if (idx < 16) return kAnsiColors[static_cast<size_t>(idx)];

    // 216-color cube (indices 16..231)
    if (idx < 232) {
        int v = idx - 16;
        int b = v % 6;
        int g = (v / 6) % 6;
        int r = v / 36;
        auto component = [](int c) -> uint8_t {
            return c == 0 ? 0 : static_cast<uint8_t>(55 + c * 40);
        };
        return 0xFF000000u |
               (static_cast<uint32_t>(component(r)) << 16) |
               (static_cast<uint32_t>(component(g)) << 8) |
               static_cast<uint32_t>(component(b));
    }

    // Grayscale ramp (indices 232..255)
    if (idx < 256) {
        uint8_t v = static_cast<uint8_t>(8 + (idx - 232) * 10);
        return 0xFF000000u |
               (static_cast<uint32_t>(v) << 16) |
               (static_cast<uint32_t>(v) << 8) |
               static_cast<uint32_t>(v);
    }

    return 0xFFCCCCCC;
}

Vte::Vte(int cols, int rows)
    : cols_(cols), rows_(rows), scroll_bottom_(rows) {
    current_attrs_ = default_attrs_;
    screen_.resize(static_cast<size_t>(rows));
    for (auto& row : screen_) {
        row.resize(static_cast<size_t>(cols));
    }
    init_tab_stops();
}

void Vte::init_tab_stops() {
    tab_stops_.resize(static_cast<size_t>(cols_), false);
    for (int i = 0; i < cols_; i += 8) {
        tab_stops_[static_cast<size_t>(i)] = true;
    }
}

void Vte::reset() {
    cursor_col_ = 0;
    cursor_row_ = 0;
    cursor_visible_ = true;
    current_attrs_ = default_attrs_;
    state_ = VteState::Ground;
    csi_params_.clear();
    csi_current_param_ = -1;
    csi_private_ = false;
    osc_string_.clear();
    utf8_char_ = 0;
    utf8_remaining_ = 0;
    scroll_top_ = 0;
    scroll_bottom_ = rows_;
    origin_mode_ = false;
    wraparound_ = true;
    insert_mode_ = false;
    using_alt_screen_ = false;

    screen_.clear();
    screen_.resize(static_cast<size_t>(rows_));
    for (auto& row : screen_) {
        row.resize(static_cast<size_t>(cols_));
    }
    scrollback_.clear();
    init_tab_stops();
    dirty_ = true;
}

void Vte::resize(int cols, int rows) {
    if (cols == cols_ && rows == rows_) return;

    int old_cols = cols_;
    int old_rows = rows_;
    cols_ = cols;
    rows_ = rows;

    // Resize screen rows
    while (static_cast<int>(screen_.size()) < rows) {
        Row new_row(static_cast<size_t>(cols));
        screen_.push_back(std::move(new_row));
    }
    while (static_cast<int>(screen_.size()) > rows) {
        // Push overflow into scrollback
        scrollback_.push_back(std::move(screen_.front()));
        screen_.pop_front();
        while (static_cast<int>(scrollback_.size()) > max_scrollback_) {
            scrollback_.pop_front();
        }
    }

    // Resize each row's column count
    for (auto& row : screen_) {
        row.resize(static_cast<size_t>(cols));
    }

    // Clamp cursor
    cursor_col_ = std::clamp(cursor_col_, 0, cols_ - 1);
    cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);

    // Reset scroll region
    scroll_top_ = 0;
    scroll_bottom_ = rows_;

    // Reinitialize tab stops
    init_tab_stops();

    dirty_ = true;
    (void)old_cols;
    (void)old_rows;
}

void Vte::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = static_cast<uint8_t>(data[i]);

        // UTF-8 decoding
        if (utf8_remaining_ > 0) {
            if ((byte & 0xC0) == 0x80) {
                utf8_char_ = (utf8_char_ << 6) | (byte & 0x3F);
                utf8_remaining_--;
                if (utf8_remaining_ == 0) {
                    process_char(utf8_char_);
                }
            } else {
                // Invalid continuation byte, reset
                utf8_remaining_ = 0;
                process_char(U'\xFFFD'); // replacement char
                // Re-process this byte
                --i;
            }
            continue;
        }

        if (byte < 0x80) {
            process_char(static_cast<char32_t>(byte));
        } else if ((byte & 0xE0) == 0xC0) {
            utf8_char_ = byte & 0x1F;
            utf8_remaining_ = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            utf8_char_ = byte & 0x0F;
            utf8_remaining_ = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            utf8_char_ = byte & 0x07;
            utf8_remaining_ = 3;
        } else {
            process_char(U'\xFFFD');
        }
    }
}

void Vte::process_char(char32_t ch) {
    switch (state_) {
    case VteState::Ground:
        if (ch == 0x1B) { // ESC
            state_ = VteState::Escape;
        } else if (ch < 0x20 || ch == 0x7F) {
            execute_control(ch);
        } else {
            put_char(ch);
        }
        break;

    case VteState::Escape:
        handle_esc(ch);
        break;

    case VteState::CsiEntry:
        csi_params_.clear();
        csi_current_param_ = -1;
        csi_private_ = false;
        if (ch == '?') {
            csi_private_ = true;
            state_ = VteState::CsiParam;
        } else if (ch == '>') {
            csi_private_ = true; // DA2 prefix
            state_ = VteState::CsiParam;
        } else if (ch >= '0' && ch <= '9') {
            csi_current_param_ = static_cast<int>(ch - '0');
            state_ = VteState::CsiParam;
        } else if (ch == ';') {
            csi_params_.push_back(0);
            state_ = VteState::CsiParam;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            // Final byte with no params
            handle_csi(ch);
            state_ = VteState::Ground;
        } else {
            state_ = VteState::CsiParam;
        }
        break;

    case VteState::CsiParam:
        if (ch >= '0' && ch <= '9') {
            if (csi_current_param_ < 0) csi_current_param_ = 0;
            csi_current_param_ = csi_current_param_ * 10 + static_cast<int>(ch - '0');
        } else if (ch == ';') {
            csi_params_.push_back(csi_current_param_ < 0 ? 0 : csi_current_param_);
            csi_current_param_ = -1;
        } else if (ch >= 0x20 && ch <= 0x2F) {
            // Intermediate bytes
            if (csi_current_param_ >= 0) {
                csi_params_.push_back(csi_current_param_);
                csi_current_param_ = -1;
            }
            state_ = VteState::CsiIntermed;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            // Final byte
            if (csi_current_param_ >= 0) {
                csi_params_.push_back(csi_current_param_);
            }
            handle_csi(ch);
            state_ = VteState::Ground;
        } else {
            // Unexpected, abort
            state_ = VteState::Ground;
        }
        break;

    case VteState::CsiIntermed:
        if (ch >= 0x40 && ch <= 0x7E) {
            // We ignore most intermediate-qualified sequences for now
            state_ = VteState::Ground;
        } else if (ch < 0x20 || ch > 0x2F) {
            state_ = VteState::Ground;
        }
        break;

    case VteState::OscString:
        if (ch == 0x07) { // BEL terminates OSC
            handle_osc_end();
            state_ = VteState::Ground;
        } else if (ch == 0x1B) {
            state_ = VteState::OscEnd;
        } else if (ch >= 0x20) {
            osc_string_ += static_cast<char>(ch);
        }
        break;

    case VteState::OscEnd:
        if (ch == '\\') { // ST = ESC backslash
            handle_osc_end();
        }
        // Either way, return to ground
        state_ = VteState::Ground;
        break;

    case VteState::DcsEntry:
    case VteState::DcsPassthrough:
        // Consume until ST
        if (ch == 0x1B) {
            state_ = VteState::Escape; // will handle ST
        } else if (ch == 0x9C) {
            state_ = VteState::Ground;
        }
        break;

    case VteState::SosPmApc:
        // Consume until ST
        if (ch == 0x1B) {
            state_ = VteState::Escape;
        } else if (ch == 0x9C) {
            state_ = VteState::Ground;
        }
        break;
    }
}

void Vte::execute_control(char32_t ch) {
    switch (ch) {
    case 0x07: // BEL
        // Could trigger system bell
        break;
    case 0x08: // BS (Backspace)
        if (cursor_col_ > 0) {
            cursor_col_--;
            dirty_ = true;
        }
        break;
    case 0x09: { // HT (Tab)
        int next = cursor_col_ + 1;
        while (next < cols_ && !tab_stops_[static_cast<size_t>(next)]) {
            next++;
        }
        cursor_col_ = std::min(next, cols_ - 1);
        dirty_ = true;
        break;
    }
    case 0x0A: // LF (Line Feed)
    case 0x0B: // VT (Vertical Tab)
    case 0x0C: // FF (Form Feed)
        newline();
        break;
    case 0x0D: // CR (Carriage Return)
        cursor_col_ = 0;
        dirty_ = true;
        break;
    case 0x0E: // SO (Shift Out) - switch to G1 charset
    case 0x0F: // SI (Shift In) - switch to G0 charset
        // Charset switching not implemented, but don't break
        break;
    case 0x7F: // DEL
        // Ignored
        break;
    default:
        break;
    }
}

void Vte::put_char(char32_t ch) {
    ensure_row(cursor_row_);

    if (cursor_col_ >= cols_) {
        if (wraparound_) {
            cursor_col_ = 0;
            newline();
        } else {
            cursor_col_ = cols_ - 1;
        }
    }

    auto& row = screen_[static_cast<size_t>(cursor_row_)];
    if (insert_mode_) {
        // Shift characters right
        for (int i = cols_ - 1; i > cursor_col_; --i) {
            row[static_cast<size_t>(i)] = row[static_cast<size_t>(i - 1)];
        }
    }

    Cell& cell = row[static_cast<size_t>(cursor_col_)];
    cell.ch = ch;
    cell.attrs = current_attrs_;
    cell.dirty = true;
    cursor_col_++;
    dirty_ = true;
}

void Vte::newline() {
    if (cursor_row_ == scroll_bottom_ - 1) {
        scroll_up();
    } else if (cursor_row_ < rows_ - 1) {
        cursor_row_++;
    }
    dirty_ = true;
}

void Vte::scroll_up() {
    if (scroll_top_ >= scroll_bottom_) return;

    Row scrolled_row = std::move(screen_[static_cast<size_t>(scroll_top_)]);

    // Only save to scrollback if scrolling the full screen
    if (scroll_top_ == 0 && !using_alt_screen_) {
        scrollback_.push_back(std::move(scrolled_row));
        while (static_cast<int>(scrollback_.size()) > max_scrollback_) {
            scrollback_.pop_front();
        }
    }

    // Shift rows up within scroll region
    for (int i = scroll_top_; i < scroll_bottom_ - 1; ++i) {
        screen_[static_cast<size_t>(i)] =
            std::move(screen_[static_cast<size_t>(i + 1)]);
    }

    // New blank row at bottom of scroll region
    screen_[static_cast<size_t>(scroll_bottom_ - 1)] =
        Row(static_cast<size_t>(cols_));
    dirty_ = true;
}

void Vte::scroll_down() {
    if (scroll_top_ >= scroll_bottom_) return;

    // Shift rows down within scroll region
    for (int i = scroll_bottom_ - 1; i > scroll_top_; --i) {
        screen_[static_cast<size_t>(i)] =
            std::move(screen_[static_cast<size_t>(i - 1)]);
    }

    // New blank row at top of scroll region
    screen_[static_cast<size_t>(scroll_top_)] =
        Row(static_cast<size_t>(cols_));
    dirty_ = true;
}

void Vte::ensure_row(int row) {
    while (static_cast<int>(screen_.size()) <= row) {
        screen_.push_back(Row(static_cast<size_t>(cols_)));
    }
    auto& r = screen_[static_cast<size_t>(row)];
    if (static_cast<int>(r.size()) < cols_) {
        r.resize(static_cast<size_t>(cols_));
    }
}

void Vte::handle_esc(char32_t ch) {
    switch (ch) {
    case '[': // CSI
        state_ = VteState::CsiEntry;
        return;
    case ']': // OSC
        osc_string_.clear();
        state_ = VteState::OscString;
        return;
    case 'P': // DCS
        state_ = VteState::DcsEntry;
        return;
    case 'X': // SOS
    case '^': // PM
    case '_': // APC
        state_ = VteState::SosPmApc;
        return;
    case '\\': // ST (String Terminator)
        state_ = VteState::Ground;
        return;
    case '7': // DECSC (Save Cursor)
        saved_cursor_col_ = cursor_col_;
        saved_cursor_row_ = cursor_row_;
        saved_attrs_ = current_attrs_;
        state_ = VteState::Ground;
        return;
    case '8': // DECRC (Restore Cursor)
        cursor_col_ = saved_cursor_col_;
        cursor_row_ = saved_cursor_row_;
        current_attrs_ = saved_attrs_;
        dirty_ = true;
        state_ = VteState::Ground;
        return;
    case 'D': // IND (Index — move down, scroll if at bottom)
        newline();
        state_ = VteState::Ground;
        return;
    case 'E': // NEL (Next Line)
        cursor_col_ = 0;
        newline();
        state_ = VteState::Ground;
        return;
    case 'M': // RI (Reverse Index — move up, scroll down if at top)
        if (cursor_row_ == scroll_top_) {
            scroll_down();
        } else if (cursor_row_ > 0) {
            cursor_row_--;
        }
        dirty_ = true;
        state_ = VteState::Ground;
        return;
    case 'H': // HTS (Horizontal Tab Set)
        if (cursor_col_ >= 0 && cursor_col_ < cols_) {
            tab_stops_[static_cast<size_t>(cursor_col_)] = true;
        }
        state_ = VteState::Ground;
        return;
    case 'c': // RIS (Full Reset)
        reset();
        state_ = VteState::Ground;
        return;
    case '(': // Designate G0 charset
    case ')': // Designate G1 charset
    case '*': // Designate G2 charset
    case '+': // Designate G3 charset
        // Next char is charset designator, consume it
        // We treat all charsets as ASCII/Latin-1
        state_ = VteState::Ground;
        return;
    default:
        state_ = VteState::Ground;
        return;
    }
}

int Vte::param(int index, int default_val) const {
    if (index < 0 || index >= static_cast<int>(csi_params_.size())) {
        return default_val;
    }
    int v = csi_params_[static_cast<size_t>(index)];
    return v <= 0 ? default_val : v;
}

int Vte::param_count() const {
    return static_cast<int>(csi_params_.size());
}

void Vte::handle_csi(char32_t ch) {
    switch (ch) {
    case 'A': // CUU — Cursor Up
        csi_cursor_up(param(0, 1));
        break;
    case 'B': // CUD — Cursor Down
        csi_cursor_down(param(0, 1));
        break;
    case 'C': // CUF — Cursor Forward
        csi_cursor_forward(param(0, 1));
        break;
    case 'D': // CUB — Cursor Back
        csi_cursor_back(param(0, 1));
        break;
    case 'E': // CNL — Cursor Next Line
        cursor_col_ = 0;
        csi_cursor_down(param(0, 1));
        break;
    case 'F': // CPL — Cursor Previous Line
        cursor_col_ = 0;
        csi_cursor_up(param(0, 1));
        break;
    case 'G': // CHA — Cursor Horizontal Absolute
        cursor_col_ = std::clamp(param(0, 1) - 1, 0, cols_ - 1);
        dirty_ = true;
        break;
    case 'H': // CUP — Cursor Position
    case 'f': // HVP — Horizontal Vertical Position (same as CUP)
        csi_cursor_position(param(0, 1), param(1, 1));
        break;
    case 'J': // ED — Erase in Display
        csi_erase_display(param(0, 0));
        break;
    case 'K': // EL — Erase in Line
        csi_erase_line(param(0, 0));
        break;
    case 'L': // IL — Insert Lines
        csi_insert_lines(param(0, 1));
        break;
    case 'M': // DL — Delete Lines
        csi_delete_lines(param(0, 1));
        break;
    case 'P': // DCH — Delete Characters
        csi_delete_chars(param(0, 1));
        break;
    case '@': // ICH — Insert Characters
        csi_insert_chars(param(0, 1));
        break;
    case 'S': // SU — Scroll Up
        csi_scroll_up(param(0, 1));
        break;
    case 'T': // SD — Scroll Down
        csi_scroll_down(param(0, 1));
        break;
    case 'd': // VPA — Vertical Position Absolute
        cursor_row_ = std::clamp(param(0, 1) - 1, 0, rows_ - 1);
        dirty_ = true;
        break;
    case 'h': // SM — Set Mode
        csi_set_mode(true);
        break;
    case 'l': // RM — Reset Mode
        csi_set_mode(false);
        break;
    case 'm': // SGR — Select Graphic Rendition
        csi_sgr();
        break;
    case 'n': // DSR — Device Status Report
        // We don't send responses here; the main loop would need to handle this
        break;
    case 'r': // DECSTBM — Set Scrolling Region
        scroll_top_ = std::clamp(param(0, 1) - 1, 0, rows_ - 1);
        scroll_bottom_ = std::clamp(param(1, rows_), 1, rows_);
        if (scroll_top_ >= scroll_bottom_) {
            scroll_top_ = 0;
            scroll_bottom_ = rows_;
        }
        // Cursor goes home after setting scroll region
        cursor_col_ = 0;
        cursor_row_ = origin_mode_ ? scroll_top_ : 0;
        dirty_ = true;
        break;
    case 's': // SCP — Save Cursor Position
        saved_cursor_col_ = cursor_col_;
        saved_cursor_row_ = cursor_row_;
        break;
    case 'u': // RCP — Restore Cursor Position
        cursor_col_ = saved_cursor_col_;
        cursor_row_ = saved_cursor_row_;
        dirty_ = true;
        break;
    case 'X': { // ECH — Erase Characters
        int n = param(0, 1);
        ensure_row(cursor_row_);
        auto& row = screen_[static_cast<size_t>(cursor_row_)];
        for (int i = cursor_col_; i < std::min(cursor_col_ + n, cols_); ++i) {
            row[static_cast<size_t>(i)] = Cell{};
            row[static_cast<size_t>(i)].attrs = current_attrs_;
            row[static_cast<size_t>(i)].dirty = true;
        }
        dirty_ = true;
        break;
    }
    case 'g': // TBC — Tab Clear
        if (param(0, 0) == 0) {
            if (cursor_col_ >= 0 && cursor_col_ < cols_)
                tab_stops_[static_cast<size_t>(cursor_col_)] = false;
        } else if (param(0, 0) == 3) {
            std::fill(tab_stops_.begin(), tab_stops_.end(), false);
        }
        break;
    case 'c': // DA — Device Attributes (reply handled externally)
        break;
    default:
        break;
    }
}

void Vte::csi_cursor_up(int n) {
    cursor_row_ = std::max(cursor_row_ - n, scroll_top_);
    dirty_ = true;
}

void Vte::csi_cursor_down(int n) {
    cursor_row_ = std::min(cursor_row_ + n, scroll_bottom_ - 1);
    dirty_ = true;
}

void Vte::csi_cursor_forward(int n) {
    cursor_col_ = std::min(cursor_col_ + n, cols_ - 1);
    dirty_ = true;
}

void Vte::csi_cursor_back(int n) {
    cursor_col_ = std::max(cursor_col_ - n, 0);
    dirty_ = true;
}

void Vte::csi_cursor_position(int row, int col) {
    int base_row = origin_mode_ ? scroll_top_ : 0;
    cursor_row_ = std::clamp(row - 1 + base_row, 0, rows_ - 1);
    cursor_col_ = std::clamp(col - 1, 0, cols_ - 1);
    dirty_ = true;
}

void Vte::csi_erase_display(int mode) {
    switch (mode) {
    case 0: // Erase below (from cursor to end)
        ensure_row(cursor_row_);
        {
            auto& row = screen_[static_cast<size_t>(cursor_row_)];
            for (int c = cursor_col_; c < cols_; ++c) {
                row[static_cast<size_t>(c)] = Cell{};
                row[static_cast<size_t>(c)].attrs.bg_color = current_attrs_.bg_color;
            }
        }
        for (int r = cursor_row_ + 1; r < rows_; ++r) {
            ensure_row(r);
            auto& row = screen_[static_cast<size_t>(r)];
            for (int c = 0; c < cols_; ++c) {
                row[static_cast<size_t>(c)] = Cell{};
                row[static_cast<size_t>(c)].attrs.bg_color = current_attrs_.bg_color;
            }
        }
        break;
    case 1: // Erase above (from start to cursor)
        for (int r = 0; r < cursor_row_; ++r) {
            ensure_row(r);
            auto& row = screen_[static_cast<size_t>(r)];
            for (int c = 0; c < cols_; ++c) {
                row[static_cast<size_t>(c)] = Cell{};
                row[static_cast<size_t>(c)].attrs.bg_color = current_attrs_.bg_color;
            }
        }
        ensure_row(cursor_row_);
        {
            auto& row = screen_[static_cast<size_t>(cursor_row_)];
            for (int c = 0; c <= cursor_col_ && c < cols_; ++c) {
                row[static_cast<size_t>(c)] = Cell{};
                row[static_cast<size_t>(c)].attrs.bg_color = current_attrs_.bg_color;
            }
        }
        break;
    case 2: // Erase entire display
    case 3: // Erase entire display and scrollback (xterm)
        for (int r = 0; r < rows_; ++r) {
            ensure_row(r);
            auto& row = screen_[static_cast<size_t>(r)];
            for (int c = 0; c < cols_; ++c) {
                row[static_cast<size_t>(c)] = Cell{};
                row[static_cast<size_t>(c)].attrs.bg_color = current_attrs_.bg_color;
            }
        }
        if (mode == 3) {
            scrollback_.clear();
        }
        break;
    }
    dirty_ = true;
}

void Vte::csi_erase_line(int mode) {
    ensure_row(cursor_row_);
    auto& row = screen_[static_cast<size_t>(cursor_row_)];

    int start = 0, end = cols_;
    switch (mode) {
    case 0: start = cursor_col_; break;             // Erase to right
    case 1: end = std::min(cursor_col_ + 1, cols_); break; // Erase to left
    case 2: break;                                    // Erase entire line
    }

    for (int c = start; c < end; ++c) {
        row[static_cast<size_t>(c)] = Cell{};
        row[static_cast<size_t>(c)].attrs.bg_color = current_attrs_.bg_color;
    }
    dirty_ = true;
}

void Vte::csi_insert_lines(int n) {
    if (cursor_row_ < scroll_top_ || cursor_row_ >= scroll_bottom_) return;

    for (int i = 0; i < n; ++i) {
        // Shift lines down within scroll region from cursor to bottom
        for (int r = scroll_bottom_ - 1; r > cursor_row_; --r) {
            screen_[static_cast<size_t>(r)] =
                std::move(screen_[static_cast<size_t>(r - 1)]);
        }
        screen_[static_cast<size_t>(cursor_row_)] =
            Row(static_cast<size_t>(cols_));
    }
    cursor_col_ = 0;
    dirty_ = true;
}

void Vte::csi_delete_lines(int n) {
    if (cursor_row_ < scroll_top_ || cursor_row_ >= scroll_bottom_) return;

    for (int i = 0; i < n; ++i) {
        // Shift lines up within scroll region
        for (int r = cursor_row_; r < scroll_bottom_ - 1; ++r) {
            screen_[static_cast<size_t>(r)] =
                std::move(screen_[static_cast<size_t>(r + 1)]);
        }
        screen_[static_cast<size_t>(scroll_bottom_ - 1)] =
            Row(static_cast<size_t>(cols_));
    }
    cursor_col_ = 0;
    dirty_ = true;
}

void Vte::csi_delete_chars(int n) {
    ensure_row(cursor_row_);
    auto& row = screen_[static_cast<size_t>(cursor_row_)];

    n = std::min(n, cols_ - cursor_col_);
    for (int i = cursor_col_; i < cols_ - n; ++i) {
        row[static_cast<size_t>(i)] = row[static_cast<size_t>(i + n)];
    }
    for (int i = cols_ - n; i < cols_; ++i) {
        row[static_cast<size_t>(i)] = Cell{};
    }
    dirty_ = true;
}

void Vte::csi_insert_chars(int n) {
    ensure_row(cursor_row_);
    auto& row = screen_[static_cast<size_t>(cursor_row_)];

    n = std::min(n, cols_ - cursor_col_);
    // Shift right
    for (int i = cols_ - 1; i >= cursor_col_ + n; --i) {
        row[static_cast<size_t>(i)] = row[static_cast<size_t>(i - n)];
    }
    for (int i = cursor_col_; i < cursor_col_ + n && i < cols_; ++i) {
        row[static_cast<size_t>(i)] = Cell{};
        row[static_cast<size_t>(i)].attrs = current_attrs_;
    }
    dirty_ = true;
}

void Vte::csi_scroll_up(int n) {
    for (int i = 0; i < n; ++i) {
        scroll_up();
    }
}

void Vte::csi_scroll_down(int n) {
    for (int i = 0; i < n; ++i) {
        scroll_down();
    }
}

void Vte::csi_set_mode(bool set) {
    if (csi_private_) {
        for (int i = 0; i < param_count(); ++i) {
            int mode_num = csi_params_[static_cast<size_t>(i)];
            switch (mode_num) {
            case 1: // DECCKM — Cursor Keys Mode
                // Application mode vs normal mode — affects key sequences
                break;
            case 6: // DECOM — Origin Mode
                origin_mode_ = set;
                cursor_col_ = 0;
                cursor_row_ = origin_mode_ ? scroll_top_ : 0;
                dirty_ = true;
                break;
            case 7: // DECAWM — Autowrap Mode
                wraparound_ = set;
                break;
            case 12: // AT&T cursor blink
                break;
            case 25: // DECTCEM — Show/Hide Cursor
                cursor_visible_ = set;
                dirty_ = true;
                break;
            case 47:   // Alternate screen buffer (old xterm)
            case 1047: // Alternate screen buffer
                if (set && !using_alt_screen_) {
                    alt_screen_ = screen_;
                    alt_cursor_col_ = cursor_col_;
                    alt_cursor_row_ = cursor_row_;
                    using_alt_screen_ = true;
                    // Clear the screen for alt buffer
                    for (auto& row : screen_) {
                        row = Row(static_cast<size_t>(cols_));
                    }
                    dirty_ = true;
                } else if (!set && using_alt_screen_) {
                    screen_ = alt_screen_;
                    cursor_col_ = alt_cursor_col_;
                    cursor_row_ = alt_cursor_row_;
                    using_alt_screen_ = false;
                    dirty_ = true;
                }
                break;
            case 1048: // Save/restore cursor (xterm)
                if (set) {
                    saved_cursor_col_ = cursor_col_;
                    saved_cursor_row_ = cursor_row_;
                    saved_attrs_ = current_attrs_;
                } else {
                    cursor_col_ = saved_cursor_col_;
                    cursor_row_ = saved_cursor_row_;
                    current_attrs_ = saved_attrs_;
                    dirty_ = true;
                }
                break;
            case 1049: // Alternate screen + save/restore cursor
                if (set) {
                    saved_cursor_col_ = cursor_col_;
                    saved_cursor_row_ = cursor_row_;
                    saved_attrs_ = current_attrs_;
                    alt_screen_ = screen_;
                    using_alt_screen_ = true;
                    for (auto& row : screen_) {
                        row = Row(static_cast<size_t>(cols_));
                    }
                    dirty_ = true;
                } else if (using_alt_screen_) {
                    screen_ = alt_screen_;
                    cursor_col_ = saved_cursor_col_;
                    cursor_row_ = saved_cursor_row_;
                    current_attrs_ = saved_attrs_;
                    using_alt_screen_ = false;
                    dirty_ = true;
                }
                break;
            case 2004: // Bracketed paste mode
                // We note this but handle it at the input layer
                break;
            default:
                break;
            }
        }
    } else {
        // Standard (non-private) modes
        for (int i = 0; i < param_count(); ++i) {
            int mode_num = csi_params_[static_cast<size_t>(i)];
            switch (mode_num) {
            case 4: // IRM — Insert/Replace Mode
                insert_mode_ = set;
                break;
            case 20: // LNM — Line Feed / New Line Mode
                // Affects whether LF implies CR
                break;
            default:
                break;
            }
        }
    }
}

int Vte::parse_sgr_color(int start_idx, uint32_t& color_out) const {
    if (start_idx >= param_count()) return 0;
    int sub = csi_params_[static_cast<size_t>(start_idx)];

    if (sub == 5 && start_idx + 1 < param_count()) {
        // 256-color: ESC[38;5;Nm or ESC[48;5;Nm
        int idx = csi_params_[static_cast<size_t>(start_idx + 1)];
        color_out = color_from_index(idx);
        return 2;
    }

    if (sub == 2 && start_idx + 3 < param_count()) {
        // Truecolor: ESC[38;2;R;G;Bm or ESC[48;2;R;G;Bm
        int r = csi_params_[static_cast<size_t>(start_idx + 1)];
        int g = csi_params_[static_cast<size_t>(start_idx + 2)];
        int b = csi_params_[static_cast<size_t>(start_idx + 3)];
        r = std::clamp(r, 0, 255);
        g = std::clamp(g, 0, 255);
        b = std::clamp(b, 0, 255);
        color_out = 0xFF000000u |
                    (static_cast<uint32_t>(r) << 16) |
                    (static_cast<uint32_t>(g) << 8) |
                    static_cast<uint32_t>(b);
        return 4;
    }

    return 1; // consume at least the sub-parameter
}

void Vte::csi_sgr() {
    if (param_count() == 0) {
        // ESC[m — reset
        current_attrs_ = default_attrs_;
        return;
    }

    for (int i = 0; i < param_count(); ++i) {
        int p = csi_params_[static_cast<size_t>(i)];

        if (p == 0) {
            current_attrs_ = default_attrs_;
        } else if (p == 1) {
            current_attrs_.bold = true;
        } else if (p == 2) {
            current_attrs_.dim = true;
        } else if (p == 3) {
            current_attrs_.italic = true;
        } else if (p == 4) {
            current_attrs_.underline = true;
        } else if (p == 5 || p == 6) {
            current_attrs_.blink = true;
        } else if (p == 7) {
            current_attrs_.reverse = true;
        } else if (p == 8) {
            current_attrs_.hidden = true;
        } else if (p == 9) {
            current_attrs_.strikethrough = true;
        } else if (p == 21) {
            current_attrs_.bold = false; // sometimes "doubly underlined"
        } else if (p == 22) {
            current_attrs_.bold = false;
            current_attrs_.dim = false;
        } else if (p == 23) {
            current_attrs_.italic = false;
        } else if (p == 24) {
            current_attrs_.underline = false;
        } else if (p == 25) {
            current_attrs_.blink = false;
        } else if (p == 27) {
            current_attrs_.reverse = false;
        } else if (p == 28) {
            current_attrs_.hidden = false;
        } else if (p == 29) {
            current_attrs_.strikethrough = false;
        } else if (p >= 30 && p <= 37) {
            // Standard foreground colors
            current_attrs_.fg_color = kAnsiColors[static_cast<size_t>(p - 30)];
        } else if (p == 38) {
            // Extended foreground color
            int consumed = parse_sgr_color(i + 1, current_attrs_.fg_color);
            i += consumed;
        } else if (p == 39) {
            // Default foreground
            current_attrs_.fg_color = default_attrs_.fg_color;
        } else if (p >= 40 && p <= 47) {
            // Standard background colors
            current_attrs_.bg_color = kAnsiColors[static_cast<size_t>(p - 40)];
        } else if (p == 48) {
            // Extended background color
            int consumed = parse_sgr_color(i + 1, current_attrs_.bg_color);
            i += consumed;
        } else if (p == 49) {
            // Default background
            current_attrs_.bg_color = default_attrs_.bg_color;
        } else if (p >= 90 && p <= 97) {
            // Bright foreground colors
            current_attrs_.fg_color = kAnsiColors[static_cast<size_t>(p - 90 + 8)];
        } else if (p >= 100 && p <= 107) {
            // Bright background colors
            current_attrs_.bg_color = kAnsiColors[static_cast<size_t>(p - 100 + 8)];
        }
    }
}

void Vte::handle_osc_end() {
    // Parse OSC: Ps ; Pt
    auto semi_pos = osc_string_.find(';');
    if (semi_pos == std::string::npos) {
        osc_string_.clear();
        return;
    }

    std::string ps_str = osc_string_.substr(0, semi_pos);
    std::string pt = osc_string_.substr(semi_pos + 1);

    int ps = 0;
    try {
        ps = std::stoi(ps_str);
    } catch (...) {
        osc_string_.clear();
        return;
    }

    switch (ps) {
    case 0: // Set icon name and window title
    case 2: // Set window title
        title_ = pt;
        break;
    case 1: // Set icon name (ignored, use title)
        break;
    case 4: // Set color palette entry (ignored)
        break;
    case 7: // Set working directory (iTerm2/modern terminals)
        break;
    case 8: // Hyperlink (ignored for now)
        break;
    case 10: // Set foreground color
    case 11: // Set background color
        // Could parse X11 color names or #rrggbb
        break;
    case 52: // Clipboard operations (ignored for security)
        break;
    default:
        break;
    }

    osc_string_.clear();
}

void Vte::mark_clean() {
    for (auto& row : screen_) {
        for (auto& cell : row) {
            cell.dirty = false;
        }
    }
    dirty_ = false;
}

} // namespace straylight::terminal
