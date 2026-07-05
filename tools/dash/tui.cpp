// tools/dash/tui.cpp
// Full terminal UI engine implementation using ANSI escape sequences.

#include "tui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sstream>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace straylight {

static std::atomic<bool> g_resized{false};

static void sigwinch_handler(int) {
    g_resized.store(true);
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

std::string Color::fg256(int code) {
    return "\033[38;5;" + std::to_string(code) + "m";
}

std::string Color::bg256(int code) {
    return "\033[48;5;" + std::to_string(code) + "m";
}

// ---------------------------------------------------------------------------
// TUI lifecycle
// ---------------------------------------------------------------------------

TUI::TUI() = default;

TUI::~TUI() {
    if (initialized_) {
        shutdown();
    }
}

Result<void, std::string> TUI::init() {
    if (initialized_) {
        return Result<void, std::string>::ok();
    }

    // Save terminal state.
    saved_termios_ = new struct termios;
    if (tcgetattr(STDIN_FILENO, saved_termios_) < 0) {
        delete saved_termios_;
        saved_termios_ = nullptr;
        return Result<void, std::string>::error(
            "tcgetattr failed: " + std::string(strerror(errno)));
    }

    // Enter raw mode.
    struct termios raw = *saved_termios_;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0; // Non-blocking read.

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        delete saved_termios_;
        saved_termios_ = nullptr;
        return Result<void, std::string>::error(
            "tcsetattr failed: " + std::string(strerror(errno)));
    }

    // Enter alternate screen buffer.
    write_raw("\033[?1049h");
    // Hide cursor.
    write_raw("\033[?25l");
    flush();

    initialized_ = true;
    return Result<void, std::string>::ok();
}

void TUI::shutdown() {
    if (!initialized_) return;

    // Show cursor.
    write_raw("\033[?25h");
    // Leave alternate screen buffer.
    write_raw("\033[?1049l");
    flush();

    // Restore terminal state.
    if (saved_termios_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, saved_termios_);
        delete saved_termios_;
        saved_termios_ = nullptr;
    }

    initialized_ = false;
}

// ---------------------------------------------------------------------------
// Terminal queries
// ---------------------------------------------------------------------------

TermSize TUI::size() const {
    TermSize ts;
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        ts.cols = ws.ws_col;
        ts.rows = ws.ws_row;
    }
    return ts;
}

void TUI::install_resize_handler() {
    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, nullptr);
}

bool TUI::was_resized() {
    return g_resized.exchange(false);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void TUI::write_raw(const std::string& s) {
    output_buf_ += s;
}

void TUI::flush() {
    if (!output_buf_.empty()) {
        ::write(STDOUT_FILENO, output_buf_.data(), output_buf_.size());
        output_buf_.clear();
    }
}

void TUI::clear() {
    write_raw("\033[2J");
}

void TUI::move_to(int x, int y) {
    // ANSI is 1-based.
    write_raw("\033[" + std::to_string(y + 1) + ";" +
              std::to_string(x + 1) + "H");
}

void TUI::print(const std::string& text) {
    write_raw(text);
}

void TUI::print_at(int x, int y, const std::string& text) {
    move_to(x, y);
    write_raw(text);
}

void TUI::hline(int x, int y, int width, char ch) {
    move_to(x, y);
    write_raw(std::string(std::max(0, width), ch));
}

void TUI::vline(int x, int y, int height, char ch) {
    for (int i = 0; i < height; ++i) {
        move_to(x, y + i);
        write_raw(std::string(1, ch));
    }
}

void TUI::box(const Rect& r, const std::string& title) {
    // Top border.
    move_to(r.x, r.y);
    write_raw("+");
    if (!title.empty()) {
        std::string t = " " + title + " ";
        int remaining = r.w - 2 - static_cast<int>(t.size());
        if (remaining < 0) remaining = 0;
        write_raw(std::string(Color::Bold) + t + Color::Reset);
        write_raw(std::string(remaining, '-'));
    } else {
        write_raw(std::string(std::max(0, r.w - 2), '-'));
    }
    write_raw("+");

    // Side borders.
    for (int i = 1; i < r.h - 1; ++i) {
        move_to(r.x, r.y + i);
        write_raw("|");
        move_to(r.x + r.w - 1, r.y + i);
        write_raw("|");
    }

    // Bottom border.
    if (r.h > 1) {
        move_to(r.x, r.y + r.h - 1);
        write_raw("+");
        write_raw(std::string(std::max(0, r.w - 2), '-'));
        write_raw("+");
    }
}

void TUI::bar(int x, int y, int width, double fraction,
               const std::string& color) {
    fraction = std::max(0.0, std::min(1.0, fraction));
    int filled = static_cast<int>(fraction * width);

    move_to(x, y);
    if (!color.empty()) write_raw(color);

    // Use block characters for sub-cell precision.
    static const char* blocks[] = {" ", "\u2588"}; // Full block.
    for (int i = 0; i < width; ++i) {
        write_raw(i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91"); // Full block / light shade.
    }
    write_raw(Color::Reset);
}

void TUI::sparkline(int x, int y, int width,
                      const std::vector<double>& data,
                      const std::string& color) {
    // Unicode sparkline characters: 8 levels.
    static const char* sparks[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
    };

    if (data.empty()) return;

    double max_val = *std::max_element(data.begin(), data.end());
    if (max_val <= 0.0) max_val = 1.0;

    move_to(x, y);
    if (!color.empty()) write_raw(color);

    // Take the last 'width' data points.
    int start = static_cast<int>(data.size()) - width;
    if (start < 0) start = 0;

    // Pad with spaces if not enough data.
    for (int i = 0; i < width - static_cast<int>(data.size()); ++i) {
        write_raw(" ");
    }

    for (int i = start; i < static_cast<int>(data.size()); ++i) {
        double norm = data[i] / max_val;
        int level = static_cast<int>(norm * 7.0);
        level = std::max(0, std::min(7, level));
        write_raw(sparks[level]);
    }
    write_raw(Color::Reset);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

int TUI::read_key() {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;

    if (c == '\033') {
        // Possible escape sequence.
        char seq[3];
        if (::read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESC;
        if (::read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESC;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }

    return static_cast<int>(c);
}

} // namespace straylight
