// apps/terminal/pty.h
// PTY master/slave management for the StrayLight terminal emulator
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <functional>
#include <string>
#include <sys/types.h>

namespace straylight::terminal {

/// Manages a pseudo-terminal (PTY) pair.
/// Creates the master/slave, forks a child process running the user's shell,
/// and provides non-blocking read/write to the master side.
class Pty {
public:
    Pty();
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&&) noexcept;
    Pty& operator=(Pty&&) noexcept;

    /// Open a PTY and fork the child shell process.
    /// cols/rows set the initial terminal size.
    Result<void, std::string> spawn(int cols, int rows,
                                    const std::string& shell = "");

    /// Write data to the PTY master (sends to child).
    Result<ssize_t, std::string> write(const char* data, size_t len);

    /// Read available data from the PTY master (from child).
    /// Returns number of bytes read, or 0 if nothing available.
    Result<ssize_t, std::string> read(char* buf, size_t max_len);

    /// Resize the PTY to new dimensions.
    Result<void, std::string> resize(int cols, int rows);

    /// Check if the child process is still alive.
    [[nodiscard]] bool is_alive() const;

    /// Send a signal to the child process.
    void signal(int sig);

    /// Get the master file descriptor (for polling).
    [[nodiscard]] int master_fd() const { return master_fd_; }

    /// Get the child PID.
    [[nodiscard]] pid_t child_pid() const { return child_pid_; }

    /// Close the PTY and wait for the child.
    void close();

private:
    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    bool closed_ = true;
};

} // namespace straylight::terminal
