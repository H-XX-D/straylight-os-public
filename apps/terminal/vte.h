// apps/terminal/vte.h
// VT100/xterm escape sequence parser and terminal grid
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace straylight::terminal {

/// SGR (Select Graphic Rendition) attributes for a terminal cell.
struct CellAttrs {
    uint32_t fg_color = 0xFFCCCCCC; // ARGB default foreground (light gray)
    uint32_t bg_color = 0xFF1A1A2E; // ARGB default background (dark cyberpunk)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool dim = false;
    bool blink = false;
    bool reverse = false;
    bool hidden = false;

    bool operator==(const CellAttrs& o) const = default;
};

/// A single cell in the terminal grid.
struct Cell {
    char32_t ch = U' ';
    CellAttrs attrs{};
    bool dirty = true; // needs redraw
};

/// A row of cells.
using Row = std::vector<Cell>;

/// Parser state machine states for VT100/xterm sequences.
enum class VteState {
    Ground,      // Normal character processing
    Escape,      // After ESC
    CsiEntry,    // After ESC [
    CsiParam,    // Collecting CSI parameters
    CsiIntermed, // CSI intermediate bytes
    OscString,   // Operating System Command string
    OscEnd,      // OSC terminator detection
    DcsEntry,    // Device Control String
    DcsPassthrough,
    SosPmApc,    // SOS/PM/APC string
};

/// VTE (Virtual Terminal Emulator) — parses escape sequences and maintains
/// the terminal grid state.
class Vte {
public:
    /// Create a VTE with given dimensions.
    Vte(int cols, int rows);

    /// Feed raw bytes from the PTY into the parser.
    void feed(const char* data, size_t len);

    /// Resize the terminal grid.
    void resize(int cols, int rows);

    /// Get the current grid (visible rows).
    [[nodiscard]] const std::deque<Row>& screen() const { return screen_; }

    /// Get the scrollback buffer.
    [[nodiscard]] const std::deque<Row>& scrollback() const { return scrollback_; }

    /// Get cursor position.
    [[nodiscard]] int cursor_col() const { return cursor_col_; }
    [[nodiscard]] int cursor_row() const { return cursor_row_; }
    [[nodiscard]] bool cursor_visible() const { return cursor_visible_; }

    /// Get the terminal title (set via OSC).
    [[nodiscard]] const std::string& title() const { return title_; }

    /// Get dimensions.
    [[nodiscard]] int cols() const { return cols_; }
    [[nodiscard]] int rows() const { return rows_; }

    /// Get max scrollback lines.
    [[nodiscard]] int max_scrollback() const { return max_scrollback_; }
    void set_max_scrollback(int lines) { max_scrollback_ = lines; }

    /// Mark all cells as clean (after rendering).
    void mark_clean();

    /// Check if any cell is dirty.
    [[nodiscard]] bool is_dirty() const { return dirty_; }

    /// Reset the terminal to initial state.
    void reset();

    /// Get default foreground/background colors.
    [[nodiscard]] uint32_t default_fg() const { return default_attrs_.fg_color; }
    [[nodiscard]] uint32_t default_bg() const { return default_attrs_.bg_color; }
    void set_default_fg(uint32_t color) { default_attrs_.fg_color = color; }
    void set_default_bg(uint32_t color) { default_attrs_.bg_color = color; }

private:
    // Character processing
    void process_char(char32_t ch);
    void put_char(char32_t ch);
    void execute_control(char32_t ch);

    // Escape sequence handlers
    void handle_esc(char32_t ch);
    void handle_csi(char32_t ch);
    void handle_osc_end();

    // CSI sub-handlers
    void csi_cursor_up(int n);
    void csi_cursor_down(int n);
    void csi_cursor_forward(int n);
    void csi_cursor_back(int n);
    void csi_cursor_position(int row, int col);
    void csi_erase_display(int mode);
    void csi_erase_line(int mode);
    void csi_insert_lines(int n);
    void csi_delete_lines(int n);
    void csi_delete_chars(int n);
    void csi_insert_chars(int n);
    void csi_scroll_up(int n);
    void csi_scroll_down(int n);
    void csi_set_mode(bool set);
    void csi_sgr(); // Select Graphic Rendition

    // Line/scroll management
    void newline();
    void scroll_up();
    void scroll_down();
    void ensure_row(int row);

    // Helper: parse CSI params
    [[nodiscard]] int param(int index, int default_val = 0) const;
    [[nodiscard]] int param_count() const;

    // Parse a color from SGR params starting at given index, returns how many
    // params were consumed and the resulting color.
    int parse_sgr_color(int start_idx, uint32_t& color_out) const;

    // Xterm 256-color palette
    static uint32_t color_from_index(int idx);

    // State
    int cols_;
    int rows_;
    int max_scrollback_ = 10000;

    int cursor_col_ = 0;
    int cursor_row_ = 0;
    bool cursor_visible_ = true;

    // Saved cursor for DECSC/DECRC
    int saved_cursor_col_ = 0;
    int saved_cursor_row_ = 0;
    CellAttrs saved_attrs_{};

    // Scroll region
    int scroll_top_ = 0;
    int scroll_bottom_ = 0; // exclusive, set to rows_ on init

    // Current attributes
    CellAttrs current_attrs_{};
    CellAttrs default_attrs_{};

    // Parser state
    VteState state_ = VteState::Ground;
    std::vector<int> csi_params_;
    int csi_current_param_ = -1;
    bool csi_private_ = false; // '?' prefix in CSI
    std::string osc_string_;

    // UTF-8 accumulator
    char32_t utf8_char_ = 0;
    int utf8_remaining_ = 0;

    // Terminal grid
    std::deque<Row> screen_;
    std::deque<Row> scrollback_;

    // Title
    std::string title_;

    // Dirty tracking
    bool dirty_ = true;

    // Modes
    bool origin_mode_ = false;  // DECOM
    bool wraparound_ = true;    // DECAWM
    bool insert_mode_ = false;  // IRM

    // Alternate screen buffer support
    std::deque<Row> alt_screen_;
    int alt_cursor_col_ = 0;
    int alt_cursor_row_ = 0;
    bool using_alt_screen_ = false;

    // Tab stops
    std::vector<bool> tab_stops_;
    void init_tab_stops();
};

} // namespace straylight::terminal
