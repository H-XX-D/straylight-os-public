// tools/remote/terminal.h
#pragma once

#include "tls_client.h"
#include <string>
#include <atomic>
#include <functional>

struct termios;

namespace straylight {

/// PTY terminal emulator for interactive remote shell sessions.
/// Puts the local terminal in raw mode, forwards input to the remote PTY stream,
/// and renders remote output to the local terminal.
/// Press Ctrl+] to disconnect (like telnet escape).
class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// Run an interactive shell session over the given TLS connection.
    /// Blocks until the user disconnects (Ctrl+]) or the connection drops.
    /// Returns 0 on clean disconnect, 1 on error.
    int run(TlsClient& client);

private:
    struct termios* original_termios_ = nullptr;
    std::atomic<bool> running_{false};

    // Terminal dimensions
    int rows_ = 24;
    int cols_ = 80;

    // Enter raw terminal mode (no echo, no line buffering, no signal generation).
    bool enter_raw_mode();

    // Restore original terminal settings.
    void restore_terminal();

    // Get current terminal window size.
    void update_window_size();

    // Install SIGWINCH handler for window resize detection.
    void install_sigwinch_handler();

    // Send a window resize notification to the remote PTY.
    void send_resize(TlsClient& client);

    // Static SIGWINCH handler
    static std::atomic<bool> s_resize_pending_;
    static void sigwinch_handler(int sig);
};

} // namespace straylight
