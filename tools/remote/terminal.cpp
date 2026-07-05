// tools/remote/terminal.cpp
#include "terminal.h"

#include <nlohmann/json.hpp>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace straylight {

std::atomic<bool> Terminal::s_resize_pending_{false};

Terminal::Terminal() {
    original_termios_ = new struct termios;
    std::memset(original_termios_, 0, sizeof(struct termios));
}

Terminal::~Terminal() {
    restore_terminal();
    delete original_termios_;
}

int Terminal::run(TlsClient& client) {
    if (!enter_raw_mode()) {
        std::cerr << "Failed to enter raw terminal mode\n";
        return 1;
    }

    install_sigwinch_handler();
    update_window_size();

    // Start the remote shell via exec_stream
    nlohmann::json start_params;
    start_params["cmd"] = "bash -l";
    start_params["pty"] = true;

    auto result = client.request("exec_stream", start_params.dump());
    if (!result.has_value()) {
        restore_terminal();
        std::cerr << "Failed to start remote shell: " << result.error() << "\n";
        return 1;
    }

    // Print initial output
    try {
        auto resp = nlohmann::json::parse(result.value());
        if (resp.contains("result") && resp["result"].contains("data")) {
            std::string data = resp["result"]["data"];
            write(STDOUT_FILENO, data.data(), data.size());
        }
    } catch (...) {}

    running_ = true;

    std::cerr << "\r\n[Connected to remote shell. Press Ctrl+] to disconnect.]\r\n";

    // Main loop: multiplex between local stdin and remote data
    while (running_ && client.is_connected()) {
        // Check for window resize
        if (s_resize_pending_.exchange(false)) {
            update_window_size();
            send_resize(client);
        }

        struct pollfd pfds[1];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;

        int poll_result = ::poll(pfds, 1, 100);

        if (poll_result > 0 && (pfds[0].revents & POLLIN)) {
            // Read local input
            char buf[1024];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                running_ = false;
                break;
            }

            // Check for Ctrl+] (0x1d = GS, Group Separator)
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == 0x1d) {
                    running_ = false;
                    std::cerr << "\r\n[Disconnected.]\r\n";
                    break;
                }
            }

            if (!running_) break;

            // Send input to remote
            std::string input(buf, static_cast<size_t>(n));
            nlohmann::json exec_params;
            exec_params["cmd"] = input;
            exec_params["pty"] = true;

            auto send_result = client.request("exec_stream", exec_params.dump(), 5000);
            if (send_result.has_value()) {
                try {
                    auto resp = nlohmann::json::parse(send_result.value());
                    if (resp.contains("result") && resp["result"].contains("data")) {
                        std::string data = resp["result"]["data"];
                        write(STDOUT_FILENO, data.data(), data.size());
                    }
                } catch (...) {}
            }
        }
    }

    restore_terminal();
    return 0;
}

bool Terminal::enter_raw_mode() {
    if (!isatty(STDIN_FILENO)) {
        return false;
    }

    // Save current terminal settings
    if (tcgetattr(STDIN_FILENO, original_termios_) < 0) {
        return false;
    }

    struct termios raw = *original_termios_;

    // Input flags: no break, no CR-to-NL, no parity check, no strip, no start/stop
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Output flags: disable post-processing
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);

    // Control flags: set 8-bit chars
    raw.c_cflag |= CS8;

    // Local flags: no echo, no canonical, no extended functions, no signal chars
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);

    // Control chars: return each byte, no timeout
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return false;
    }

    return true;
}

void Terminal::restore_terminal() {
    if (original_termios_ && isatty(STDIN_FILENO)) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, original_termios_);
    }
}

void Terminal::update_window_size() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows_ = ws.ws_row;
        cols_ = ws.ws_col;
    }
}

void Terminal::install_sigwinch_handler() {
    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, nullptr);
}

void Terminal::send_resize(TlsClient& client) {
    nlohmann::json resize_params;
    resize_params["cmd"] = "stty rows " + std::to_string(rows_) +
                           " cols " + std::to_string(cols_);
    resize_params["pty"] = false;

    // Fire and forget — don't block on resize
    client.request("exec", resize_params.dump(), 2000);
}

void Terminal::sigwinch_handler(int /*sig*/) {
    s_resize_pending_.store(true);
}

} // namespace straylight
