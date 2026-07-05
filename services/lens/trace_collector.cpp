// services/lens/trace_collector.cpp
// Multi-source trace event collection from compositor, IPC, VPU, GPU, and kernel.

#include "trace_collector.h"

#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace straylight {

TraceCollector::TraceCollector() = default;

TraceCollector::~TraceCollector() {
    if (collecting_) {
        // Force stop
        {
            std::lock_guard lock(mutex_);
            collecting_ = false;
        }
        if (compositor_thread_.joinable()) compositor_thread_.join();
        if (ipc_thread_.joinable()) ipc_thread_.join();
        if (vpu_thread_.joinable()) vpu_thread_.join();
        if (gpu_thread_.joinable()) gpu_thread_.join();
        if (kernel_thread_.joinable()) kernel_thread_.join();
    }
}

uint64_t TraceCollector::now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

Result<void, SLError> TraceCollector::start(const std::string& correlation_id) {
    std::lock_guard lock(mutex_);

    if (collecting_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::AlreadyExists, "Already collecting a trace"});
    }

    current_trace_ = Trace{};
    current_trace_.trace_id = "trace-" + std::to_string(now_ns());
    current_trace_.correlation_id = correlation_id.empty()
        ? current_trace_.trace_id : correlation_id;
    current_trace_.state = TraceState::Collecting;
    current_trace_.start_ns = now_ns();
    current_trace_.events.clear();

    collecting_ = true;

    // Enable kernel tracepoints for StrayLight subsystems
    {
        std::ofstream tracing_on("/sys/kernel/debug/tracing/tracing_on");
        if (tracing_on.is_open()) {
            tracing_on << "1";
        }

        // Enable relevant tracepoints
        for (const char* tp : {
            "/sys/kernel/debug/tracing/events/straylight_vpu/enable",
            "/sys/kernel/debug/tracing/events/straylight_compositor/enable",
            "/sys/kernel/debug/tracing/events/sched/sched_switch/enable"
        }) {
            std::ofstream f(tp);
            if (f.is_open()) f << "1";
        }
    }

    // Launch collector threads
    compositor_thread_ = std::thread(&TraceCollector::collect_compositor_events, this);
    ipc_thread_ = std::thread(&TraceCollector::collect_ipc_events, this);
    vpu_thread_ = std::thread(&TraceCollector::collect_vpu_events, this);
    gpu_thread_ = std::thread(&TraceCollector::collect_gpu_events, this);
    kernel_thread_ = std::thread(&TraceCollector::collect_kernel_events, this);

    SL_INFO("lens: started trace collection (id={}, corr={})",
            current_trace_.trace_id, current_trace_.correlation_id);

    return Result<void, SLError>::ok();
}

Result<Trace, SLError> TraceCollector::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!collecting_) {
            return Result<Trace, SLError>::error(
                SLError{SLErrorCode::NotInitialized, "Not currently collecting"});
        }
        collecting_ = false;
    }

    // Wait for all collector threads to finish
    if (compositor_thread_.joinable()) compositor_thread_.join();
    if (ipc_thread_.joinable()) ipc_thread_.join();
    if (vpu_thread_.joinable()) vpu_thread_.join();
    if (gpu_thread_.joinable()) gpu_thread_.join();
    if (kernel_thread_.joinable()) kernel_thread_.join();

    // Disable kernel tracepoints
    {
        for (const char* tp : {
            "/sys/kernel/debug/tracing/events/straylight_vpu/enable",
            "/sys/kernel/debug/tracing/events/straylight_compositor/enable",
            "/sys/kernel/debug/tracing/events/sched/sched_switch/enable"
        }) {
            std::ofstream f(tp);
            if (f.is_open()) f << "0";
        }
    }

    std::lock_guard lock(mutex_);
    current_trace_.end_ns = now_ns();
    current_trace_.state = TraceState::Complete;

    // Sort events by timestamp
    std::sort(current_trace_.events.begin(), current_trace_.events.end());

    SL_INFO("lens: stopped trace collection ({} events in {:.3f}ms)",
            current_trace_.events.size(),
            static_cast<double>(current_trace_.end_ns - current_trace_.start_ns) / 1e6);

    return Result<Trace, SLError>::ok(current_trace_);
}

bool TraceCollector::is_collecting() const {
    std::lock_guard lock(mutex_);
    return collecting_;
}

Trace TraceCollector::current_snapshot() const {
    std::lock_guard lock(mutex_);
    return current_trace_;
}

void TraceCollector::inject_event(TraceEvent event) {
    std::lock_guard lock(mutex_);
    if (event.timestamp_ns == 0) {
        event.timestamp_ns = now_ns();
    }
    if (event.correlation_id.empty()) {
        event.correlation_id = current_trace_.correlation_id;
    }
    current_trace_.events.push_back(std::move(event));
}

void TraceCollector::collect_compositor_events() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SL_WARN("lens: cannot create compositor trace socket: {}", ::strerror(errno));
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, compositor_trace_sock_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        SL_DEBUG("lens: compositor trace socket not available: {}", ::strerror(errno));
        ::close(fd);
        return;
    }

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    char buf[4096];
    std::string buffer;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!collecting_) break;
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 100); // 100ms timeout

        if (rc <= 0) continue;

        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break; // Connection closed or error
        }

        buffer.append(buf, static_cast<size_t>(n));

        // Process newline-delimited JSON events
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            try {
                auto j = nlohmann::json::parse(line);

                TraceEvent ev;
                ev.timestamp_ns = j.value("timestamp_ns", now_ns());
                ev.layer = TraceLayer::Compositor;
                ev.event_type = j.value("type", "compositor_event");
                ev.pid = j.value("pid", 0);
                ev.duration_ns = j.value("duration_ns", uint64_t{0});

                if (j.contains("data") && j["data"].is_object()) {
                    for (auto& [k, v] : j["data"].items()) {
                        ev.data[k] = v.is_string() ? v.get<std::string>() : v.dump();
                    }
                }

                {
                    std::lock_guard lock(mutex_);
                    ev.correlation_id = current_trace_.correlation_id;
                    current_trace_.events.push_back(std::move(ev));
                }
            } catch (const nlohmann::json::exception&) {
                // Non-JSON line — parse as text
                TraceEvent ev;
                ev.timestamp_ns = now_ns();
                ev.layer = TraceLayer::Compositor;
                ev.event_type = "compositor_raw";
                ev.data["raw"] = line;

                std::lock_guard lock(mutex_);
                ev.correlation_id = current_trace_.correlation_id;
                current_trace_.events.push_back(std::move(ev));
            }
        }
    }

    ::close(fd);
}

void TraceCollector::collect_ipc_events() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        SL_WARN("lens: cannot create IPC debug socket: {}", ::strerror(errno));
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, whisper_debug_sock_.c_str(),
                 sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        SL_DEBUG("lens: whisper debug socket not available: {}", ::strerror(errno));
        ::close(fd);
        return;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Send subscribe command to get all IPC events
    const char* subscribe = "{\"cmd\":\"subscribe\",\"filter\":\"all\"}\n";
    ::write(fd, subscribe, std::strlen(subscribe));

    char buf[4096];
    std::string buffer;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!collecting_) break;
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 100);

        if (rc <= 0) continue;

        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }

        buffer.append(buf, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            try {
                auto j = nlohmann::json::parse(line);

                TraceEvent ev;
                ev.timestamp_ns = j.value("timestamp_ns", now_ns());
                ev.layer = TraceLayer::Ipc;
                ev.event_type = j.value("type", "ipc_message");
                ev.pid = j.value("src_pid", 0);
                ev.duration_ns = j.value("duration_ns", uint64_t{0});

                ev.data["src_pid"] = std::to_string(j.value("src_pid", 0));
                ev.data["dst_pid"] = std::to_string(j.value("dst_pid", 0));
                ev.data["channel"] = j.value("channel", "");
                ev.data["msg_size"] = std::to_string(j.value("size", 0));

                std::lock_guard lock(mutex_);
                ev.correlation_id = current_trace_.correlation_id;
                current_trace_.events.push_back(std::move(ev));
            } catch (const nlohmann::json::exception&) {
                // Skip malformed lines
            }
        }
    }

    ::close(fd);
}

void TraceCollector::collect_vpu_events() {
    // Read VPU trace events from sysfs
    std::string trace_path = vpu_sysfs_;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!collecting_) break;
        }

        std::ifstream trace_file(trace_path);
        if (!trace_file.is_open()) {
            // VPU tracing not available — wait and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        std::string line;
        while (std::getline(trace_file, line)) {
            {
                std::lock_guard lock(mutex_);
                if (!collecting_) return;
            }

            if (line.empty() || line[0] == '#') continue;

            // Parse VPU sysfs trace format:
            // timestamp_ns event_type slab_order block_idx size pid
            TraceEvent ev;
            ev.layer = TraceLayer::Vpu;

            std::istringstream iss(line);
            std::string ts_str, etype;
            int slab_order = 0, block_idx = 0;
            uint64_t size_val = 0;
            pid_t pid = 0;

            if (iss >> ts_str >> etype >> slab_order >> block_idx >> size_val >> pid) {
                try {
                    ev.timestamp_ns = std::stoull(ts_str);
                } catch (...) {
                    ev.timestamp_ns = now_ns();
                }
                ev.event_type = "vpu_" + etype;
                ev.pid = pid;
                ev.data["slab_order"] = std::to_string(slab_order);
                ev.data["block_idx"] = std::to_string(block_idx);
                ev.data["size"] = std::to_string(size_val);

                std::lock_guard lock(mutex_);
                ev.correlation_id = current_trace_.correlation_id;
                current_trace_.events.push_back(std::move(ev));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void TraceCollector::collect_gpu_events() {
    // Read GPU trace events from debugfs
    int fd = ::open(gpu_debugfs_.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        SL_DEBUG("lens: GPU debugfs trace not available: {}", ::strerror(errno));
        return;
    }

    char buf[4096];
    std::string buffer;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!collecting_) break;
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 100);

        if (rc <= 0) continue;

        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }

        buffer.append(buf, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            // Parse GPU trace format: timestamp_ns type context_id duration_ns
            TraceEvent ev;
            ev.layer = TraceLayer::Gpu;

            std::istringstream iss(line);
            std::string ts_str, etype, ctx_str, dur_str;
            if (iss >> ts_str >> etype >> ctx_str >> dur_str) {
                try { ev.timestamp_ns = std::stoull(ts_str); } catch (...) { ev.timestamp_ns = now_ns(); }
                try { ev.duration_ns = std::stoull(dur_str); } catch (...) {}
                ev.event_type = "gpu_" + etype;
                ev.data["context_id"] = ctx_str;

                std::lock_guard lock(mutex_);
                ev.correlation_id = current_trace_.correlation_id;
                current_trace_.events.push_back(std::move(ev));
            }
        }
    }

    ::close(fd);
}

void TraceCollector::collect_kernel_events() {
    int fd = ::open(ftrace_pipe_.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        SL_DEBUG("lens: ftrace pipe not available: {}", ::strerror(errno));
        return;
    }

    char buf[8192];
    std::string buffer;

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (!collecting_) break;
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 100);

        if (rc <= 0) continue;

        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }

        buffer.append(buf, static_cast<size_t>(n));

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty() || line[0] == '#') continue;

            std::lock_guard lock(mutex_);
            auto result = parse_ftrace_line(line, current_trace_.correlation_id);
            if (result.has_value()) {
                current_trace_.events.push_back(std::move(result.value()));
            }
        }
    }

    ::close(fd);
}

Result<TraceEvent, std::string>
TraceCollector::parse_ftrace_line(const std::string& line,
                                   const std::string& correlation_id) {
    // ftrace format: <task>-<pid> [cpu] timestamp: <event>: <data>
    // Example: compositor-1234 [002] 123456.789012: straylight_vpu_alloc: order=4 block=7
    TraceEvent ev;
    ev.layer = TraceLayer::Kernel;
    ev.correlation_id = correlation_id;

    // Find PID
    size_t dash = line.find('-');
    if (dash == std::string::npos) {
        return Result<TraceEvent, std::string>::error("No PID separator");
    }

    size_t space = line.find(' ', dash);
    if (space == std::string::npos) {
        return Result<TraceEvent, std::string>::error("Malformed ftrace line");
    }

    std::string task = line.substr(0, dash);
    std::string pid_str = line.substr(dash + 1, space - dash - 1);
    try { ev.pid = std::stoi(pid_str); } catch (...) { ev.pid = 0; }

    // Find timestamp
    size_t ts_start = line.find(']', space);
    if (ts_start == std::string::npos) {
        return Result<TraceEvent, std::string>::error("No timestamp bracket");
    }
    ts_start += 2; // skip "] "

    size_t ts_end = line.find(':', ts_start);
    if (ts_end == std::string::npos) {
        return Result<TraceEvent, std::string>::error("No timestamp colon");
    }

    std::string ts_str = line.substr(ts_start, ts_end - ts_start);
    // Parse seconds.microseconds to nanoseconds
    size_t dot = ts_str.find('.');
    if (dot != std::string::npos) {
        try {
            uint64_t sec = std::stoull(ts_str.substr(0, dot));
            uint64_t usec = std::stoull(ts_str.substr(dot + 1));
            ev.timestamp_ns = sec * 1000000000ULL + usec * 1000ULL;
        } catch (...) {
            ev.timestamp_ns = now_ns();
        }
    }

    // Find event type and data
    size_t event_start = ts_end + 2; // skip ": "
    size_t event_end = line.find(':', event_start);
    if (event_end != std::string::npos) {
        ev.event_type = line.substr(event_start, event_end - event_start);
        // Trim whitespace
        while (!ev.event_type.empty() && ev.event_type.back() == ' ')
            ev.event_type.pop_back();

        // Parse key=value data after the event type
        std::string data_str = line.substr(event_end + 2);
        std::istringstream iss(data_str);
        std::string token;
        while (iss >> token) {
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                ev.data[token.substr(0, eq)] = token.substr(eq + 1);
            }
        }
    } else {
        ev.event_type = line.substr(event_start);
    }

    ev.data["task"] = task;

    return Result<TraceEvent, std::string>::ok(std::move(ev));
}

} // namespace straylight
