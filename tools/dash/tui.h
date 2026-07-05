// tools/dash/tui.h
// Terminal UI engine for straylight-dash -- pure ANSI escape sequences.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace straylight {

/// ANSI color codes.
struct Color {
    static constexpr const char* Reset   = "\033[0m";
    static constexpr const char* Bold    = "\033[1m";
    static constexpr const char* Dim     = "\033[2m";
    static constexpr const char* Red     = "\033[31m";
    static constexpr const char* Green   = "\033[32m";
    static constexpr const char* Yellow  = "\033[33m";
    static constexpr const char* Blue    = "\033[34m";
    static constexpr const char* Magenta = "\033[35m";
    static constexpr const char* Cyan    = "\033[36m";
    static constexpr const char* White   = "\033[37m";
    static constexpr const char* BgRed   = "\033[41m";
    static constexpr const char* BgGreen = "\033[42m";
    static constexpr const char* BgBlue  = "\033[44m";

    /// Generate 256-color foreground escape.
    static std::string fg256(int code);
    /// Generate 256-color background escape.
    static std::string bg256(int code);
};

/// Terminal size.
struct TermSize {
    int rows = 24;
    int cols = 80;
};

/// A rectangle in the terminal.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

/// Terminal UI engine. Manages raw mode, screen buffer, and input.
class TUI {
public:
    TUI();
    ~TUI();

    /// Enter raw mode and alternate screen buffer.
    Result<void, std::string> init();

    /// Restore terminal state.
    void shutdown();

    /// Get current terminal size.
    TermSize size() const;

    /// Clear the entire screen.
    void clear();

    /// Move cursor to (x, y) -- 0-based.
    void move_to(int x, int y);

    /// Print text at current cursor position.
    void print(const std::string& text);

    /// Print text at specific position.
    void print_at(int x, int y, const std::string& text);

    /// Draw a horizontal line.
    void hline(int x, int y, int width, char ch = '-');

    /// Draw a vertical line.
    void vline(int x, int y, int height, char ch = '|');

    /// Draw a box border.
    void box(const Rect& r, const std::string& title = "");

    /// Draw a horizontal bar graph.
    void bar(int x, int y, int width, double fraction,
             const std::string& color = "");

    /// Draw a sparkline from data points.
    void sparkline(int x, int y, int width,
                    const std::vector<double>& data,
                    const std::string& color = "");

    /// Flush output buffer to the terminal.
    void flush();

    /// Read a single keypress (non-blocking, returns 0 if no key).
    int read_key();

    /// Key constants.
    static constexpr int KEY_TAB = 9;
    static constexpr int KEY_ENTER = 13;
    static constexpr int KEY_ESC = 27;
    static constexpr int KEY_Q = 'q';
    static constexpr int KEY_S = 's';
    static constexpr int KEY_K = 'k';
    static constexpr int KEY_UP = 1000;
    static constexpr int KEY_DOWN = 1001;
    static constexpr int KEY_LEFT = 1002;
    static constexpr int KEY_RIGHT = 1003;

    /// Register a SIGWINCH handler for terminal resize.
    void install_resize_handler();

    /// Check if terminal was resized since last check.
    bool was_resized();

private:
    bool initialized_ = false;
    std::string output_buf_;
    struct termios* saved_termios_ = nullptr;

    void write_raw(const std::string& s);
};

} // namespace straylight
