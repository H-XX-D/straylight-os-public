// services/replay/recorder.cpp
// Event recording engine implementation.
#include "recorder.h"

#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Linux-specific headers (expected to fail on macOS)
#ifdef __linux__
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <dirent.h>
#endif

namespace straylight {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static uint64_t monotonic_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static std::string read_file_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f.is_open()) {
        std::getline(f, line);
    }
    return line;
}

static std::string proc_name_for_pid(uint32_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    std::string name = read_file_line(path);
    // Strip trailing newline
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    return name.empty() ? "<unknown>" : name;
}

// --------------------------------------------------------------------------
// EventRecorder
// --------------------------------------------------------------------------

EventRecorder::EventRecorder() = default;

EventRecorder::~EventRecorder() {
    stop();
}

Result<void, std::string> EventRecorder::start(size_t buffer_size_mb) {
    if (running_.load()) {
        return Result<void, std::string>::error("Recorder already running");
    }

    size_t total_size = buffer_size_mb * 1024 * 1024;
    auto init_result = init_buffer(total_size);
    if (!init_result.has_value()) {
        return init_result;
    }

    running_.store(true);

    // Launch source threads — each one is fire-and-forget, guarded by running_
    source_threads_.emplace_back([this] { source_kmsg(); });
    source_threads_.emplace_back([this] { source_proc_events(); });
    source_threads_.emplace_back([this] { source_network_poll(); });
    source_threads_.emplace_back([this] { source_gpu_poll(); });
    source_threads_.emplace_back([this] { source_dbus_signals(); });

    SL_INFO("replay: recorder started, buffer {}MB at {}",
            buffer_size_mb, buffer_path_);

    return Result<void, std::string>::ok();
}

void EventRecorder::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    SL_INFO("replay: stopping recorder");

    for (auto& t : source_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    source_threads_.clear();

    if (mapped_ && mapped_size_ > 0) {
        msync(mapped_, mapped_size_, MS_SYNC);
        munmap(mapped_, mapped_size_);
        mapped_ = nullptr;
        mapped_size_ = 0;
        header_ = nullptr;
        data_region_ = nullptr;
        data_capacity_ = 0;
    }

    SL_INFO("replay: recorder stopped");
}

void EventRecorder::record(SystemEvent event) {
    if (!running_.load() || !header_) {
        return;
    }

    if (event.timestamp_ns == 0) {
        event.timestamp_ns = monotonic_ns();
    }

    write_record(event);
}

uint64_t EventRecorder::event_count() const {
    if (!header_) return 0;
    return header_->event_count.load(std::memory_order_relaxed);
}

std::vector<SystemEvent> EventRecorder::snapshot() const {
    return read_all_records();
}

std::vector<SystemEvent> EventRecorder::query(
    std::function<bool(const SystemEvent&)> predicate) const {
    auto all = read_all_records();
    std::vector<SystemEvent> result;
    result.reserve(all.size());
    for (auto& ev : all) {
        if (predicate(ev)) {
            result.push_back(std::move(ev));
        }
    }
    return result;
}

std::vector<SystemEvent> EventRecorder::events_in_range(
    uint64_t start_ns, uint64_t end_ns) const {
    return query([start_ns, end_ns](const SystemEvent& ev) {
        return ev.timestamp_ns >= start_ns && ev.timestamp_ns <= end_ns;
    });
}

// --------------------------------------------------------------------------
// Ring buffer management
// --------------------------------------------------------------------------

Result<void, std::string> EventRecorder::init_buffer(size_t total_size) {
    // Ensure directory exists
    std::string dir = buffer_path_.substr(0, buffer_path_.rfind('/'));
    std::string mkdir_cmd = "mkdir -p " + dir;
    if (::system(mkdir_cmd.c_str()) != 0) {
        // Non-fatal: directory may already exist
    }

    bool fresh = false;
    int fd = ::open(buffer_path_.c_str(), O_RDWR | O_CREAT, 0640);
    if (fd < 0) {
        return Result<void, std::string>::error(
            std::string("Failed to open ring buffer: ") + ::strerror(errno));
    }

    struct stat st{};
    ::fstat(fd, &st);

    if (static_cast<size_t>(st.st_size) < total_size) {
        if (::ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
            int e = errno;
            ::close(fd);
            return Result<void, std::string>::error(
                std::string("ftruncate failed: ") + ::strerror(e));
        }
        fresh = true;
    }

    void* ptr = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);

    if (ptr == MAP_FAILED) {
        return Result<void, std::string>::error(
            std::string("mmap failed: ") + ::strerror(errno));
    }

    mapped_ = static_cast<uint8_t*>(ptr);
    mapped_size_ = total_size;
    header_ = reinterpret_cast<RingBufferHeader*>(mapped_);

    // Calculate data region
    size_t header_size = sizeof(RingBufferHeader);
    // Align to 64 bytes
    header_size = (header_size + 63) & ~size_t(63);
    data_region_ = mapped_ + header_size;
    data_capacity_ = total_size - header_size;

    if (fresh || header_->magic != RING_BUFFER_MAGIC) {
        // Initialize fresh buffer
        std::memset(mapped_, 0, total_size);
        header_->magic = RING_BUFFER_MAGIC;
        header_->version = RING_BUFFER_VERSION;
        header_->buffer_size = total_size;
        header_->data_offset = header_size;
        header_->write_offset.store(0, std::memory_order_release);
        header_->event_count.store(0, std::memory_order_release);
        header_->oldest_timestamp_ns = 0;
        SL_INFO("replay: initialized fresh ring buffer ({}MB)", total_size / (1024 * 1024));
    } else {
        SL_INFO("replay: resuming existing ring buffer, {} events",
                header_->event_count.load(std::memory_order_relaxed));
    }

    // Advise kernel on access pattern
    ::madvise(mapped_, total_size, MADV_SEQUENTIAL);

    return Result<void, std::string>::ok();
}

void EventRecorder::write_record(const SystemEvent& event) {
    // Truncate oversized fields
    std::string name = event.process_name;
    std::string detail = event.detail;
    if (name.size() > MAX_EVENT_NAME_LEN) name.resize(MAX_EVENT_NAME_LEN);
    if (detail.size() > MAX_EVENT_DETAIL_LEN) detail.resize(MAX_EVENT_DETAIL_LEN);

    // Calculate record size
    uint32_t rec_size = static_cast<uint32_t>(
        sizeof(EventRecord) + name.size() + detail.size());

    // Align to 8 bytes
    rec_size = (rec_size + 7) & ~uint32_t(7);

    if (rec_size > data_capacity_ / 2) {
        // Event too large, drop it
        return;
    }

    std::lock_guard<std::mutex> lock(record_mutex_);

    uint64_t offset = header_->write_offset.load(std::memory_order_relaxed);

    // Wrap around if needed
    if (offset + rec_size > data_capacity_) {
        // Write a sentinel (zero-sized record) to mark wrap point, then wrap
        if (offset + sizeof(uint32_t) <= data_capacity_) {
            uint32_t zero = 0;
            std::memcpy(data_region_ + offset, &zero, sizeof(zero));
        }
        offset = 0;
    }

    uint8_t* dest = data_region_ + offset;

    // Build the record
    EventRecord rec{};
    rec.record_size = rec_size;
    rec.timestamp_ns = event.timestamp_ns;
    rec.type = event.type;
    rec.pid = event.pid;
    rec.uid = event.uid;
    rec.name_len = static_cast<uint16_t>(name.size());
    rec.detail_len = static_cast<uint16_t>(detail.size());

    std::memcpy(dest, &rec, sizeof(EventRecord));
    std::memcpy(dest + sizeof(EventRecord), name.data(), name.size());
    std::memcpy(dest + sizeof(EventRecord) + name.size(), detail.data(), detail.size());

    // Zero pad alignment bytes
    size_t used = sizeof(EventRecord) + name.size() + detail.size();
    if (used < rec_size) {
        std::memset(dest + used, 0, rec_size - used);
    }

    // Update header atomically
    header_->write_offset.store(offset + rec_size, std::memory_order_release);
    header_->event_count.fetch_add(1, std::memory_order_relaxed);

    // Track oldest timestamp
    if (header_->oldest_timestamp_ns == 0 ||
        event.timestamp_ns < header_->oldest_timestamp_ns) {
        header_->oldest_timestamp_ns = event.timestamp_ns;
    }
}

std::vector<SystemEvent> EventRecorder::read_all_records() const {
    std::vector<SystemEvent> events;
    if (!data_region_ || data_capacity_ == 0) {
        return events;
    }

    uint64_t write_off = header_->write_offset.load(std::memory_order_acquire);

    // Scan from the beginning up to write_offset, handling wraps.
    // We read linearly; the buffer may have wrapped, so we read in two passes:
    // 1) From write_off to end (older data that hasn't been overwritten)
    // 2) From 0 to write_off (newer data)
    // But for simplicity and safety, just scan 0..data_capacity_ and skip invalid.

    auto read_events_in_range = [&](uint64_t start, uint64_t end) {
        uint64_t pos = start;
        while (pos + sizeof(EventRecord) <= end) {
            EventRecord rec{};
            std::memcpy(&rec, data_region_ + pos, sizeof(EventRecord));

            if (rec.record_size == 0) {
                // Sentinel or uninitialized — skip to try from beginning
                break;
            }

            if (rec.record_size < sizeof(EventRecord) || pos + rec.record_size > end) {
                break;
            }

            SystemEvent ev;
            ev.timestamp_ns = rec.timestamp_ns;
            ev.type = rec.type;
            ev.pid = rec.pid;
            ev.uid = rec.uid;

            if (rec.name_len > 0 && sizeof(EventRecord) + rec.name_len <= rec.record_size) {
                ev.process_name.assign(
                    reinterpret_cast<const char*>(data_region_ + pos + sizeof(EventRecord)),
                    rec.name_len);
            }

            if (rec.detail_len > 0 &&
                sizeof(EventRecord) + rec.name_len + rec.detail_len <= rec.record_size) {
                ev.detail.assign(
                    reinterpret_cast<const char*>(
                        data_region_ + pos + sizeof(EventRecord) + rec.name_len),
                    rec.detail_len);
            }

            if (ev.timestamp_ns > 0) {
                events.push_back(std::move(ev));
            }

            pos += rec.record_size;
        }
    };

    // Read older events (after write pointer, from previous wrap)
    if (write_off < data_capacity_) {
        read_events_in_range(write_off, data_capacity_);
    }
    // Read newer events (from start to write pointer)
    read_events_in_range(0, write_off);

    // Sort by timestamp
    std::sort(events.begin(), events.end(),
              [](const SystemEvent& a, const SystemEvent& b) {
                  return a.timestamp_ns < b.timestamp_ns;
              });

    return events;
}

// --------------------------------------------------------------------------
// Event source: kernel messages (/dev/kmsg)
// --------------------------------------------------------------------------

void EventRecorder::source_kmsg() {
#ifdef __linux__
    int fd = ::open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        SL_WARN("replay: cannot open /dev/kmsg: {}", ::strerror(errno));
        return;
    }

    // Seek to end to only get new messages
    ::lseek(fd, 0, SEEK_END);

    char buf[8192];
    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, 500);  // 500ms timeout

        if (ret <= 0) continue;

        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';

        // Parse kmsg format: "priority,sequence,timestamp,flags;message"
        std::string raw(buf, static_cast<size_t>(n));
        std::string message;
        EventType etype = EventType::DmesgEntry;

        auto semi = raw.find(';');
        if (semi != std::string::npos) {
            message = raw.substr(semi + 1);
            // Trim trailing newline
            while (!message.empty() && message.back() == '\n') message.pop_back();
        } else {
            message = raw;
        }

        // Detect OOM kill and panic
        if (message.find("Out of memory") != std::string::npos ||
            message.find("oom-kill") != std::string::npos ||
            message.find("Killed process") != std::string::npos) {
            etype = EventType::OomKill;
        } else if (message.find("Kernel panic") != std::string::npos) {
            etype = EventType::Panic;
        }

        SystemEvent ev;
        ev.timestamp_ns = monotonic_ns();
        ev.type = etype;
        ev.pid = 0;
        ev.uid = 0;
        ev.process_name = "kernel";
        ev.detail = "{\"message\":\"" + message + "\"}";
        record(std::move(ev));
    }

    ::close(fd);
#else
    // macOS stub — no /dev/kmsg
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

// --------------------------------------------------------------------------
// Event source: process events via netlink PROC_EVENT
// --------------------------------------------------------------------------

void EventRecorder::source_proc_events() {
#ifdef __linux__
    // Create netlink socket for process events
    int nl_sock = ::socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (nl_sock < 0) {
        SL_WARN("replay: cannot create netlink socket for proc events: {}",
                ::strerror(errno));
        return;
    }

    struct sockaddr_nl sa_nl{};
    sa_nl.nl_family = AF_NETLINK;
    sa_nl.nl_groups = CN_IDX_PROC;
    sa_nl.nl_pid = static_cast<__u32>(getpid());

    if (::bind(nl_sock, reinterpret_cast<struct sockaddr*>(&sa_nl), sizeof(sa_nl)) < 0) {
        SL_WARN("replay: cannot bind netlink proc socket: {}", ::strerror(errno));
        ::close(nl_sock);
        return;
    }

    // Subscribe to proc events. cn_msg ends with a flexible data[] member, so
    // build the payload in a raw aligned buffer instead of nesting it in a C++
    // struct with another field after cn_msg.
    alignas(NLMSG_ALIGNTO) char msg_buf[
        sizeof(struct nlmsghdr) + sizeof(struct cn_msg) + sizeof(enum proc_cn_mcast_op)
    ]{};
    auto* nl_hdr = reinterpret_cast<struct nlmsghdr*>(msg_buf);
    auto* cn_msg = reinterpret_cast<struct cn_msg*>(NLMSG_DATA(nl_hdr));
    auto* cn_mcast = reinterpret_cast<enum proc_cn_mcast_op*>(cn_msg->data);

    nl_hdr->nlmsg_len = sizeof(msg_buf);
    nl_hdr->nlmsg_pid = static_cast<__u32>(getpid());
    nl_hdr->nlmsg_type = NLMSG_DONE;
    cn_msg->id.idx = CN_IDX_PROC;
    cn_msg->id.val = CN_VAL_PROC;
    cn_msg->len = sizeof(enum proc_cn_mcast_op);
    *cn_mcast = PROC_CN_MCAST_LISTEN;

    if (::send(nl_sock, msg_buf, sizeof(msg_buf), 0) < 0) {
        SL_WARN("replay: cannot subscribe to proc events: {}", ::strerror(errno));
        ::close(nl_sock);
        return;
    }

    // Buffer for receiving events
    char recv_buf[4096];

    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = nl_sock;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        ssize_t len = ::recv(nl_sock, recv_buf, sizeof(recv_buf), 0);
        if (len <= 0) continue;

        auto* nlh = reinterpret_cast<struct nlmsghdr*>(recv_buf);
        if (!NLMSG_OK(nlh, static_cast<size_t>(len))) continue;

        auto* cn = static_cast<struct cn_msg*>(NLMSG_DATA(nlh));
        auto* ev = reinterpret_cast<struct proc_event*>(cn->data);

        SystemEvent sev;
        sev.timestamp_ns = monotonic_ns();
        sev.uid = 0;

        switch (ev->what) {
            case PROC_EVENT_EXEC:
                sev.type = EventType::ProcessStart;
                sev.pid = ev->event_data.exec.process_pid;
                sev.process_name = proc_name_for_pid(sev.pid);
                sev.detail = "{\"event\":\"exec\",\"tgid\":" +
                             std::to_string(ev->event_data.exec.process_tgid) + "}";
                record(std::move(sev));
                break;

            case PROC_EVENT_EXIT:
                sev.type = EventType::ProcessExit;
                sev.pid = ev->event_data.exit.process_pid;
                sev.process_name = proc_name_for_pid(sev.pid);
                sev.detail = "{\"event\":\"exit\",\"exit_code\":" +
                             std::to_string(ev->event_data.exit.exit_code) +
                             ",\"exit_signal\":" +
                             std::to_string(ev->event_data.exit.exit_signal) + "}";
                record(std::move(sev));
                break;

            case PROC_EVENT_FORK:
                sev.type = EventType::ProcessStart;
                sev.pid = ev->event_data.fork.child_pid;
                sev.process_name = proc_name_for_pid(ev->event_data.fork.parent_pid);
                sev.detail = "{\"event\":\"fork\",\"parent_pid\":" +
                             std::to_string(ev->event_data.fork.parent_pid) + "}";
                record(std::move(sev));
                break;

            default:
                break;
        }
    }

    // Unsubscribe
    *cn_mcast = PROC_CN_MCAST_IGNORE;
    ::send(nl_sock, msg_buf, sizeof(msg_buf), 0);
    ::close(nl_sock);
#else
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

// --------------------------------------------------------------------------
// Event source: network connections
// --------------------------------------------------------------------------

void EventRecorder::source_network_poll() {
#ifdef __linux__
    // Track known connections to detect new ones
    struct ConnKey {
        std::string local;
        std::string remote;
        bool operator==(const ConnKey& o) const {
            return local == o.local && remote == o.remote;
        }
    };
    std::vector<ConnKey> known_tcp;
    std::vector<ConnKey> known_listen;

    auto parse_hex_addr = [](const std::string& hex_addr) -> std::string {
        // Format: "0100007F:1F90" (127.0.0.1:8080)
        auto colon = hex_addr.find(':');
        if (colon == std::string::npos) return hex_addr;
        std::string ip_hex = hex_addr.substr(0, colon);
        std::string port_hex = hex_addr.substr(colon + 1);
        if (ip_hex.size() != 8) return hex_addr;
        unsigned long ip = std::strtoul(ip_hex.c_str(), nullptr, 16);
        unsigned long port = std::strtoul(port_hex.c_str(), nullptr, 16);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu:%lu",
                      ip & 0xFF, (ip >> 8) & 0xFF,
                      (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, port);
        return buf;
    };

    auto scan_connections = [&](const std::string& path, bool is_tcp) {
        std::ifstream f(path);
        if (!f.is_open()) return;

        std::string line;
        std::getline(f, line);  // Skip header

        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string idx, local, remote, state_hex;
            iss >> idx >> local >> remote >> state_hex;
            if (local.empty() || remote.empty()) continue;

            unsigned long state = std::strtoul(state_hex.c_str(), nullptr, 16);

            std::string local_str = parse_hex_addr(local);
            std::string remote_str = parse_hex_addr(remote);
            ConnKey key{local_str, remote_str};

            if (is_tcp && state == 0x0A) {  // TCP_LISTEN
                bool found = false;
                for (const auto& k : known_listen) {
                    if (k == key) { found = true; break; }
                }
                if (!found) {
                    known_listen.push_back(key);
                    SystemEvent ev;
                    ev.timestamp_ns = monotonic_ns();
                    ev.type = EventType::NetworkListen;
                    ev.pid = 0;
                    ev.uid = 0;
                    ev.process_name = "network";
                    ev.detail = "{\"local\":\"" + local_str + "\",\"protocol\":\"tcp\"}";
                    record(std::move(ev));
                }
            } else if (is_tcp && state == 0x01) {  // TCP_ESTABLISHED
                bool found = false;
                for (const auto& k : known_tcp) {
                    if (k == key) { found = true; break; }
                }
                if (!found) {
                    known_tcp.push_back(key);
                    SystemEvent ev;
                    ev.timestamp_ns = monotonic_ns();
                    ev.type = EventType::NetworkConnect;
                    ev.pid = 0;
                    ev.uid = 0;
                    ev.process_name = "network";
                    ev.detail = "{\"local\":\"" + local_str +
                                "\",\"remote\":\"" + remote_str +
                                "\",\"protocol\":\"tcp\"}";
                    record(std::move(ev));
                }
            }
        }
    };

    while (running_.load()) {
        scan_connections("/proc/net/tcp", true);
        scan_connections("/proc/net/udp", false);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
#else
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

// --------------------------------------------------------------------------
// Event source: GPU events via sysfs
// --------------------------------------------------------------------------

void EventRecorder::source_gpu_poll() {
#ifdef __linux__
    const std::string gpu_sysfs = "/sys/kernel/straylight-vpu/";
    uint64_t last_alloc_bytes = 0;
    uint64_t last_submit_count = 0;

    while (running_.load()) {
        // Read current GPU memory allocation
        std::string alloc_str = read_file_line(gpu_sysfs + "memory_allocated");
        std::string submit_str = read_file_line(gpu_sysfs + "submit_count");

        if (!alloc_str.empty()) {
            uint64_t alloc_bytes = std::strtoull(alloc_str.c_str(), nullptr, 10);
            if (alloc_bytes != last_alloc_bytes) {
                EventType etype = (alloc_bytes > last_alloc_bytes)
                                      ? EventType::GpuAlloc
                                      : EventType::GpuFree;
                int64_t delta = static_cast<int64_t>(alloc_bytes) -
                                static_cast<int64_t>(last_alloc_bytes);

                SystemEvent ev;
                ev.timestamp_ns = monotonic_ns();
                ev.type = etype;
                ev.pid = 0;
                ev.uid = 0;
                ev.process_name = "vpu";
                ev.detail = "{\"allocated_bytes\":" + std::to_string(alloc_bytes) +
                            ",\"delta_bytes\":" + std::to_string(delta) + "}";
                record(std::move(ev));

                last_alloc_bytes = alloc_bytes;
            }
        }

        if (!submit_str.empty()) {
            uint64_t submit_count = std::strtoull(submit_str.c_str(), nullptr, 10);
            if (submit_count != last_submit_count && last_submit_count > 0) {
                uint64_t new_submits = submit_count - last_submit_count;

                SystemEvent ev;
                ev.timestamp_ns = monotonic_ns();
                ev.type = EventType::GpuSubmit;
                ev.pid = 0;
                ev.uid = 0;
                ev.process_name = "vpu";
                ev.detail = "{\"new_submits\":" + std::to_string(new_submits) +
                            ",\"total_submits\":" + std::to_string(submit_count) + "}";
                record(std::move(ev));
            }
            last_submit_count = submit_count;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#else
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

// --------------------------------------------------------------------------
// Event source: systemd D-Bus service state changes
// --------------------------------------------------------------------------

void EventRecorder::source_dbus_signals() {
    // D-Bus integration requires libdbus or sd-bus; we use a lightweight approach:
    // shell out to busctl monitor in a subprocess and parse its output.
#ifdef __linux__
    // Use /proc/1/comm to verify systemd is PID 1
    std::string init_name = read_file_line("/proc/1/comm");
    if (init_name.find("systemd") == std::string::npos) {
        SL_WARN("replay: systemd not detected, skipping service event source");
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return;
    }

    // Monitor systemd unit property changes by polling unit states
    // We track unit active states and emit events on transitions
    struct UnitState {
        std::string name;
        std::string active_state;
    };
    std::vector<UnitState> known_units;

    auto poll_units = [&]() {
        FILE* fp = popen("systemctl list-units --type=service --no-legend --no-pager "
                         "--plain -o json 2>/dev/null || "
                         "systemctl list-units --type=service --no-legend --no-pager "
                         "--plain 2>/dev/null", "r");
        if (!fp) return;

        char buf[4096];
        std::string output;
        while (fgets(buf, sizeof(buf), fp)) {
            output += buf;
        }
        pclose(fp);

        // Parse plain output: "unit.service loaded active running description"
        std::istringstream iss(output);
        std::string line;
        std::vector<UnitState> current_units;

        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::istringstream lss(line);
            std::string unit, load, active, sub;
            lss >> unit >> load >> active >> sub;
            if (unit.empty() || active.empty()) continue;
            current_units.push_back({unit, active});
        }

        // Detect state changes
        for (const auto& cu : current_units) {
            std::string prev_state;
            for (const auto& ku : known_units) {
                if (ku.name == cu.name) {
                    prev_state = ku.active_state;
                    break;
                }
            }

            if (prev_state.empty()) {
                // New unit appeared
                if (cu.active_state == "active") {
                    SystemEvent ev;
                    ev.timestamp_ns = monotonic_ns();
                    ev.type = EventType::ServiceStart;
                    ev.pid = 0;
                    ev.uid = 0;
                    ev.process_name = cu.name;
                    ev.detail = "{\"state\":\"" + cu.active_state + "\"}";
                    record(std::move(ev));
                }
            } else if (prev_state != cu.active_state) {
                EventType etype;
                if (cu.active_state == "active") {
                    etype = EventType::ServiceStart;
                } else if (cu.active_state == "failed") {
                    etype = EventType::ServiceFail;
                } else {
                    etype = EventType::ServiceStop;
                }

                SystemEvent ev;
                ev.timestamp_ns = monotonic_ns();
                ev.type = etype;
                ev.pid = 0;
                ev.uid = 0;
                ev.process_name = cu.name;
                ev.detail = "{\"state\":\"" + cu.active_state +
                            "\",\"previous\":\"" + prev_state + "\"}";
                record(std::move(ev));
            }
        }

        known_units = std::move(current_units);
    };

    while (running_.load()) {
        poll_units();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
#else
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

} // namespace straylight
