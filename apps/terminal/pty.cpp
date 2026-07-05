// apps/terminal/pty.cpp
// PTY master/slave implementation using POSIX pty functions
#include "pty.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace straylight::terminal {

Pty::Pty() = default;

Pty::~Pty() {
    close();
}

Pty::Pty(Pty&& other) noexcept
    : master_fd_(other.master_fd_),
      child_pid_(other.child_pid_),
      closed_(other.closed_) {
    other.master_fd_ = -1;
    other.child_pid_ = -1;
    other.closed_ = true;
}

Pty& Pty::operator=(Pty&& other) noexcept {
    if (this != &other) {
        close();
        master_fd_ = other.master_fd_;
        child_pid_ = other.child_pid_;
        closed_ = other.closed_;
        other.master_fd_ = -1;
        other.child_pid_ = -1;
        other.closed_ = true;
    }
    return *this;
}

Result<void, std::string> Pty::spawn(int cols, int rows,
                                      const std::string& shell) {
    // Open a new PTY master
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return Result<void, std::string>::error(
            std::string("posix_openpt failed: ") + strerror(errno));
    }

    if (grantpt(master) != 0) {
        ::close(master);
        return Result<void, std::string>::error(
            std::string("grantpt failed: ") + strerror(errno));
    }

    if (unlockpt(master) != 0) {
        ::close(master);
        return Result<void, std::string>::error(
            std::string("unlockpt failed: ") + strerror(errno));
    }

    // Get the slave name
    char* slave_name = ptsname(master);
    if (!slave_name) {
        ::close(master);
        return Result<void, std::string>::error(
            std::string("ptsname failed: ") + strerror(errno));
    }

    // Set initial window size
    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(master, TIOCSWINSZ, &ws);

    // Set master to non-blocking
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }

    // Determine shell to execute
    std::string shell_path = shell;
    if (shell_path.empty()) {
        const char* env_shell = getenv("SHELL");
        if (env_shell && env_shell[0] != '\0') {
            shell_path = env_shell;
        } else {
            struct passwd* pw = getpwuid(getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0] != '\0') {
                shell_path = pw->pw_shell;
            } else {
                shell_path = "/bin/sh";
            }
        }
    }

    // Fork
    pid_t pid = fork();
    if (pid < 0) {
        ::close(master);
        return Result<void, std::string>::error(
            std::string("fork failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // Child process
        ::close(master);

        // Create a new session and set controlling terminal
        setsid();

        int slave = open(slave_name, O_RDWR);
        if (slave < 0) {
            _exit(EXIT_FAILURE);
        }

        // Set the slave as controlling terminal
        ioctl(slave, TIOCSCTTY, 0);

        // Set window size on slave
        ioctl(slave, TIOCSWINSZ, &ws);

        // Set up terminal attributes
        struct termios tios {};
        tcgetattr(slave, &tios);
        tios.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT | IUTF8;
        tios.c_oflag = OPOST | ONLCR;
        tios.c_cflag = CREAD | CS8 | HUPCL;
        tios.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK |
                        ECHOKE | ECHOCTL;
        tios.c_cc[VEOF] = 4;     // Ctrl-D
        tios.c_cc[VERASE] = 127; // Backspace
        tios.c_cc[VKILL] = 21;   // Ctrl-U
        tios.c_cc[VINTR] = 3;    // Ctrl-C
        tios.c_cc[VQUIT] = 28;   // Ctrl-backslash
        tios.c_cc[VSUSP] = 26;   // Ctrl-Z
        tios.c_cc[VSTART] = 17;  // Ctrl-Q
        tios.c_cc[VSTOP] = 19;   // Ctrl-S
        tcsetattr(slave, TCSANOW, &tios);

        // Redirect stdio to slave
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            ::close(slave);
        }

        // Close all other file descriptors
        for (int fd = 3; fd < 1024; ++fd) {
            ::close(fd);
        }

        // Set environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            setenv("HOME", pw->pw_dir, 0);
            setenv("USER", pw->pw_name, 0);
            setenv("LOGNAME", pw->pw_name, 0);
        }

        // Extract shell name for argv[0] (login shell prefix with -)
        const char* shell_base = strrchr(shell_path.c_str(), '/');
        std::string argv0 = "-";
        argv0 += (shell_base ? shell_base + 1 : shell_path.c_str());

        execl(shell_path.c_str(), argv0.c_str(), nullptr);
        _exit(EXIT_FAILURE);
    }

    // Parent process
    master_fd_ = master;
    child_pid_ = pid;
    closed_ = false;

    return Result<void, std::string>::ok();
}

Result<ssize_t, std::string> Pty::write(const char* data, size_t len) {
    if (master_fd_ < 0) {
        return Result<ssize_t, std::string>::error("PTY not open");
    }

    ssize_t written = ::write(master_fd_, data, len);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Result<ssize_t, std::string>::ok(0);
        }
        return Result<ssize_t, std::string>::error(
            std::string("write failed: ") + strerror(errno));
    }

    return Result<ssize_t, std::string>::ok(written);
}

Result<ssize_t, std::string> Pty::read(char* buf, size_t max_len) {
    if (master_fd_ < 0) {
        return Result<ssize_t, std::string>::error("PTY not open");
    }

    ssize_t n = ::read(master_fd_, buf, max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Result<ssize_t, std::string>::ok(0);
        }
        return Result<ssize_t, std::string>::error(
            std::string("read failed: ") + strerror(errno));
    }

    return Result<ssize_t, std::string>::ok(n);
}

Result<void, std::string> Pty::resize(int cols, int rows) {
    if (master_fd_ < 0) {
        return Result<void, std::string>::error("PTY not open");
    }

    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);

    if (ioctl(master_fd_, TIOCSWINSZ, &ws) != 0) {
        return Result<void, std::string>::error(
            std::string("TIOCSWINSZ failed: ") + strerror(errno));
    }

    // Notify the child of the size change
    if (child_pid_ > 0) {
        kill(child_pid_, SIGWINCH);
    }

    return Result<void, std::string>::ok();
}

bool Pty::is_alive() const {
    if (child_pid_ <= 0) return false;
    int status = 0;
    pid_t ret = waitpid(child_pid_, &status, WNOHANG);
    return ret == 0;
}

void Pty::signal(int sig) {
    if (child_pid_ > 0) {
        kill(child_pid_, sig);
    }
}

void Pty::close() {
    if (closed_) return;
    closed_ = true;

    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }

    if (child_pid_ > 0) {
        int status = 0;
        pid_t ret = waitpid(child_pid_, &status, WNOHANG);
        if (ret == 0) {
            // Child still running, send SIGHUP then wait
            kill(child_pid_, SIGHUP);
            // Brief wait, then force kill
            usleep(100000); // 100ms
            ret = waitpid(child_pid_, &status, WNOHANG);
            if (ret == 0) {
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0);
            }
        }
        child_pid_ = -1;
    }
}

} // namespace straylight::terminal
