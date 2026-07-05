// tools/trace/tracer.cpp
#include "tracer.h"
#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/user.h>
#include <sys/reg.h>
#include <sys/syscall.h>
#include <linux/ptrace.h>
#endif

namespace straylight {

#ifdef __linux__
static constexpr __ptrace_request ptrace_request(int request) {
    return static_cast<__ptrace_request>(request);
}
#endif

Tracer::Tracer() {
    init_syscall_table();
}

Tracer::~Tracer() {
    stop();
}

void Tracer::set_callback(TraceCallback cb) {
    callback_ = std::move(cb);
}

void Tracer::stop() {
    stop_requested_.store(true);
}

void Tracer::init_syscall_table() {
    // Common Linux syscalls — covers the most frequently seen ones.
    // Full table would be auto-generated from <asm/unistd.h>.
#ifdef __linux__
#ifdef __x86_64__
    syscall_table_[0] = "read";
    syscall_table_[1] = "write";
    syscall_table_[2] = "open";
    syscall_table_[3] = "close";
    syscall_table_[4] = "stat";
    syscall_table_[5] = "fstat";
    syscall_table_[6] = "lstat";
    syscall_table_[7] = "poll";
    syscall_table_[8] = "lseek";
    syscall_table_[9] = "mmap";
    syscall_table_[10] = "mprotect";
    syscall_table_[11] = "munmap";
    syscall_table_[12] = "brk";
    syscall_table_[13] = "rt_sigaction";
    syscall_table_[14] = "rt_sigprocmask";
    syscall_table_[15] = "rt_sigreturn";
    syscall_table_[16] = "ioctl";
    syscall_table_[17] = "pread64";
    syscall_table_[18] = "pwrite64";
    syscall_table_[19] = "readv";
    syscall_table_[20] = "writev";
    syscall_table_[21] = "access";
    syscall_table_[22] = "pipe";
    syscall_table_[23] = "select";
    syscall_table_[24] = "sched_yield";
    syscall_table_[25] = "mremap";
    syscall_table_[32] = "dup";
    syscall_table_[33] = "dup2";
    syscall_table_[35] = "nanosleep";
    syscall_table_[39] = "getpid";
    syscall_table_[41] = "socket";
    syscall_table_[42] = "connect";
    syscall_table_[43] = "accept";
    syscall_table_[44] = "sendto";
    syscall_table_[45] = "recvfrom";
    syscall_table_[46] = "sendmsg";
    syscall_table_[47] = "recvmsg";
    syscall_table_[48] = "shutdown";
    syscall_table_[49] = "bind";
    syscall_table_[50] = "listen";
    syscall_table_[56] = "clone";
    syscall_table_[57] = "fork";
    syscall_table_[58] = "vfork";
    syscall_table_[59] = "execve";
    syscall_table_[60] = "exit";
    syscall_table_[61] = "wait4";
    syscall_table_[62] = "kill";
    syscall_table_[72] = "fcntl";
    syscall_table_[78] = "getdents";
    syscall_table_[79] = "getcwd";
    syscall_table_[80] = "chdir";
    syscall_table_[82] = "rename";
    syscall_table_[83] = "mkdir";
    syscall_table_[84] = "rmdir";
    syscall_table_[85] = "creat";
    syscall_table_[87] = "unlink";
    syscall_table_[89] = "readlink";
    syscall_table_[90] = "chmod";
    syscall_table_[92] = "chown";
    syscall_table_[102] = "getuid";
    syscall_table_[110] = "getppid";
    syscall_table_[186] = "gettid";
    syscall_table_[217] = "getdents64";
    syscall_table_[231] = "exit_group";
    syscall_table_[232] = "epoll_wait";
    syscall_table_[233] = "epoll_ctl";
    syscall_table_[257] = "openat";
    syscall_table_[262] = "newfstatat";
    syscall_table_[288] = "accept4";
    syscall_table_[291] = "epoll_create1";
    syscall_table_[302] = "prlimit64";
    syscall_table_[318] = "getrandom";
    syscall_table_[332] = "statx";
    syscall_table_[435] = "clone3";
#endif
#endif
}

std::string Tracer::syscall_name(int nr) const {
    auto it = syscall_table_.find(nr);
    if (it != syscall_table_.end()) {
        return it->second;
    }
    return "syscall_" + std::to_string(nr);
}

std::string Tracer::fd_to_path(pid_t pid, int fd) const {
    char link_path[64];
    std::snprintf(link_path, sizeof(link_path), "/proc/%d/fd/%d", pid, fd);
    char buf[4096];
    ssize_t len = ::readlink(link_path, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return buf;
    }
    return "fd:" + std::to_string(fd);
}

void Tracer::classify_event(TraceData& data, const SyscallEvent& event) {
    const std::string& name = event.syscall_name;

    data.syscall_counts[name]++;
    data.syscall_total_time_ns[name] += event.duration_ns;
    data.total_syscalls++;

    // File I/O classification
    if (name == "read" || name == "pread64") {
        if (event.return_value > 0 && event.args.size() >= 1) {
            std::string path = fd_to_path(event.pid, static_cast<int>(event.args[0]));
            bool found = false;
            for (auto& fio : data.file_io) {
                if (fio.path == path) {
                    fio.read_bytes += static_cast<uint64_t>(event.return_value);
                    fio.read_calls++;
                    fio.total_latency_ns += event.duration_ns;
                    found = true;
                    break;
                }
            }
            if (!found) {
                FileIORecord rec;
                rec.path = path;
                rec.read_bytes = static_cast<uint64_t>(event.return_value);
                rec.read_calls = 1;
                rec.total_latency_ns = event.duration_ns;
                data.file_io.push_back(rec);
            }
        }
    } else if (name == "write" || name == "pwrite64") {
        if (event.return_value > 0 && event.args.size() >= 1) {
            std::string path = fd_to_path(event.pid, static_cast<int>(event.args[0]));
            bool found = false;
            for (auto& fio : data.file_io) {
                if (fio.path == path) {
                    fio.write_bytes += static_cast<uint64_t>(event.return_value);
                    fio.write_calls++;
                    fio.total_latency_ns += event.duration_ns;
                    found = true;
                    break;
                }
            }
            if (!found) {
                FileIORecord rec;
                rec.path = path;
                rec.write_bytes = static_cast<uint64_t>(event.return_value);
                rec.write_calls = 1;
                rec.total_latency_ns = event.duration_ns;
                data.file_io.push_back(rec);
            }
        }
    } else if (name == "open" || name == "openat" || name == "creat") {
        if (event.return_value >= 0 && event.args.size() >= 2) {
            // We'll track the open count
            std::string path;
            if (name == "openat" && event.args.size() >= 2) {
                // arg[1] is the path pointer — we'd need to read from tracee memory
                // For now, track by the returned fd
                path = fd_to_path(event.pid, static_cast<int>(event.return_value));
            } else {
                path = fd_to_path(event.pid, static_cast<int>(event.return_value));
            }

            bool found = false;
            for (auto& fio : data.file_io) {
                if (fio.path == path) {
                    fio.open_count++;
                    found = true;
                    break;
                }
            }
            if (!found) {
                FileIORecord rec;
                rec.path = path;
                rec.open_count = 1;
                data.file_io.push_back(rec);
            }
        }
    }

    // Network classification
    else if (name == "connect") {
        NetworkRecord rec;
        rec.remote_addr = "unknown"; // Would need to read sockaddr from tracee
        rec.protocol = "tcp";
        rec.connect_latency_ns = event.duration_ns;
        data.network.push_back(rec);
    } else if (name == "sendto" || name == "sendmsg") {
        if (event.return_value > 0 && !data.network.empty()) {
            data.network.back().bytes_sent += static_cast<uint64_t>(event.return_value);
        }
    } else if (name == "recvfrom" || name == "recvmsg") {
        if (event.return_value > 0 && !data.network.empty()) {
            data.network.back().bytes_received += static_cast<uint64_t>(event.return_value);
        }
    }

    // Memory classification
    else if (name == "mmap") {
        data.memory.mmap_calls++;
        if (event.args.size() >= 2) {
            data.memory.mmap_total_bytes += event.args[1]; // length argument
        }
    } else if (name == "munmap") {
        data.memory.munmap_calls++;
    } else if (name == "brk") {
        data.memory.brk_calls++;
        if (event.return_value > 0) {
            uint64_t brk_val = static_cast<uint64_t>(event.return_value);
            if (brk_val > data.memory.peak_brk) {
                data.memory.peak_brk = brk_val;
            }
        }
    } else if (name == "mprotect") {
        data.memory.mprotect_calls++;
    }

    // Signal classification
    else if (name == "rt_sigaction" || name == "kill") {
        if (event.args.size() >= 1) {
            int sig = static_cast<int>(event.args[0]);
            bool found = false;
            for (auto& sr : data.signals) {
                if (sr.signal_nr == sig) {
                    sr.count++;
                    sr.last_timestamp_ns = event.timestamp_ns;
                    found = true;
                    break;
                }
            }
            if (!found) {
                SignalRecord sr;
                sr.signal_nr = sig;
                sr.signal_name = "SIG" + std::to_string(sig);
                sr.count = 1;
                sr.last_timestamp_ns = event.timestamp_ns;
                data.signals.push_back(sr);
            }
        }
    }
}

Result<TraceData, SLError> Tracer::run(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return Result<TraceData, SLError>::error(
            SLError{SLErrorCode::InvalidArgument, "Empty command"});
    }

    stop_requested_.store(false);

    std::string cmd_str;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd_str += " ";
        cmd_str += argv[i];
    }

    pid_t child = ::fork();
    if (child < 0) {
        return Result<TraceData, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("fork() failed: ") + ::strerror(errno)});
    }

    if (child == 0) {
        // Child: request ptrace and exec
#ifdef __linux__
        ::ptrace(ptrace_request(PTRACE_TRACEME), 0, nullptr, nullptr);
#endif
        ::raise(SIGSTOP);

        // Build argv for exec
        std::vector<const char*> c_argv;
        for (const auto& a : argv) {
            c_argv.push_back(a.c_str());
        }
        c_argv.push_back(nullptr);

        ::execvp(c_argv[0], const_cast<char* const*>(c_argv.data()));
        // If exec fails
        ::_exit(127);
    }

    // Parent: wait for child to stop, then trace
    return trace_loop(child, cmd_str);
}

Result<TraceData, SLError> Tracer::attach(pid_t pid) {
    stop_requested_.store(false);

#ifdef __linux__
    if (::ptrace(ptrace_request(PTRACE_ATTACH), pid, nullptr, nullptr) < 0) {
        return Result<TraceData, SLError>::error(
            SLError{SLErrorCode::PermissionDenied,
                    std::string("ptrace attach failed for pid ") + std::to_string(pid) +
                    ": " + ::strerror(errno)});
    }
#else
    return Result<TraceData, SLError>::error(
        SLError{SLErrorCode::Internal, "ptrace not supported on this platform"});
#endif

    std::string cmd_str = "pid:" + std::to_string(pid);

    // Read command name from /proc
    char proc_path[64];
    std::snprintf(proc_path, sizeof(proc_path), "/proc/%d/cmdline", pid);
    std::ifstream f(proc_path);
    if (f.is_open()) {
        std::string cmdline;
        std::getline(f, cmdline, '\0');
        if (!cmdline.empty()) cmd_str = cmdline;
    }

    return trace_loop(pid, cmd_str);
}

Result<TraceData, SLError> Tracer::trace_loop(pid_t child_pid, const std::string& cmd_str) {
    TraceData data;
    data.traced_pid = child_pid;
    data.command = cmd_str;

    // Wait for initial stop
    int status;
    ::waitpid(child_pid, &status, 0);

    if (!WIFSTOPPED(status)) {
        return Result<TraceData, SLError>::error(
            SLError{SLErrorCode::Internal, "Child did not stop as expected"});
    }

#ifdef __linux__
    // Set ptrace options
    ::ptrace(ptrace_request(PTRACE_SETOPTIONS), child_pid, nullptr,
             PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE |
             PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK);

    // Resume child to start executing
    ::ptrace(ptrace_request(PTRACE_SYSCALL), child_pid, nullptr, nullptr);
#endif

    auto trace_start = std::chrono::steady_clock::now();
    data.start_time_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            trace_start.time_since_epoch()).count());

    bool in_syscall = false;
    SyscallEvent pending_event;
    auto syscall_enter_time = std::chrono::steady_clock::now();

    while (!stop_requested_.load()) {
        pid_t wpid = ::waitpid(child_pid, &status, 0);
        if (wpid < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (WIFEXITED(status)) {
            data.exit_code = WEXITSTATUS(status);
            break;
        }

        if (WIFSIGNALED(status)) {
            data.exit_code = -WTERMSIG(status);
            break;
        }

        if (!WIFSTOPPED(status)) continue;

#ifdef __linux__
        int sig = WSTOPSIG(status);

        // Check if this is a syscall stop (SIGTRAP | 0x80)
        if (sig == (SIGTRAP | 0x80)) {
            struct user_regs_struct regs;
            ::ptrace(ptrace_request(PTRACE_GETREGS), child_pid, nullptr, &regs);

            if (!in_syscall) {
                // Syscall entry
                in_syscall = true;
                syscall_enter_time = std::chrono::steady_clock::now();

                pending_event = SyscallEvent{};
                pending_event.pid = child_pid;
                pending_event.tid = child_pid;

#ifdef __x86_64__
                pending_event.syscall_nr = static_cast<int>(regs.orig_rax);
                pending_event.args = {
                    regs.rdi, regs.rsi, regs.rdx,
                    regs.r10, regs.r8, regs.r9
                };
#endif
                pending_event.syscall_name = syscall_name(pending_event.syscall_nr);

            } else {
                // Syscall exit
                in_syscall = false;
                auto now = std::chrono::steady_clock::now();

#ifdef __x86_64__
                pending_event.return_value = static_cast<int64_t>(regs.rax);
#endif
                pending_event.duration_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now - syscall_enter_time).count());

                auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - trace_start).count();
                pending_event.timestamp_ns = static_cast<uint64_t>(elapsed);

                // Classify and record
                classify_event(data, pending_event);

                if (record_events_) {
                    if (max_events_ == 0 || data.events.size() < max_events_) {
                        data.events.push_back(pending_event);
                    }
                }

                // Live callback
                if (callback_) {
                    callback_(pending_event);
                }
            }

            ::ptrace(ptrace_request(PTRACE_SYSCALL), child_pid, nullptr, nullptr);
        } else {
            // Plain SIGTRAP stops are ptrace bookkeeping (for example after
            // execve). Forwarding them would kill otherwise healthy tracees.
            const int resume_sig = (sig == SIGTRAP) ? 0 : sig;
            ::ptrace(ptrace_request(PTRACE_SYSCALL), child_pid, nullptr, resume_sig);
        }
#endif
    }

    auto trace_end = std::chrono::steady_clock::now();
    data.end_time_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            trace_end.time_since_epoch()).count());
    data.total_duration_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            trace_end - trace_start).count());

    // If stop was requested (attach mode), detach
    if (stop_requested_.load()) {
#ifdef __linux__
        ::ptrace(ptrace_request(PTRACE_DETACH), child_pid, nullptr, nullptr);
#endif
    }

    return Result<TraceData, SLError>::ok(std::move(data));
}

} // namespace straylight
