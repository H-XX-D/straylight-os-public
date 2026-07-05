/**
 * StrayLight Fabric — udev Monitor
 *
 * Watches for device hotplug events via netlink, rebuilds the affected
 * portion of the topology graph, and notifies subscribers.
 */
#pragma once

#include "topology.h"
#include "straylight/result.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace straylight::fabric {

// ── Event types ─────────────────────────────────────────────────────

enum class UdevAction {
    Add,
    Remove,
    Change,
    Move,
    Bind,
    Unbind,
    Unknown
};

inline std::string udev_action_str(UdevAction a) {
    switch (a) {
        case UdevAction::Add:    return "add";
        case UdevAction::Remove: return "remove";
        case UdevAction::Change: return "change";
        case UdevAction::Move:   return "move";
        case UdevAction::Bind:   return "bind";
        case UdevAction::Unbind: return "unbind";
        case UdevAction::Unknown: return "unknown";
    }
    return "unknown";
}

struct UdevEvent {
    UdevAction  action = UdevAction::Unknown;
    std::string subsystem;   // e.g., "pci", "usb", "net", "block"
    std::string devpath;     // sysfs path
    std::string devtype;     // e.g., "usb_device", "pci_device"
    std::string driver;
};

using TopologyChangeCallback = std::function<void(const UdevEvent&)>;

// ── udev Monitor ────────────────────────────────────────────────────

class UdevMonitor {
public:
    explicit UdevMonitor(Topology& topo) : topo_(topo) {}

    ~UdevMonitor() {
        stop();
    }

    /** Start monitoring udev events in a background thread. */
    VoidResult<> start() {
        if (running_.load()) return VoidResult<>::error("already running");

#ifdef __linux__
        // Create netlink socket for udev events
        sock_fd_ = socket(PF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK,
                          NETLINK_KOBJECT_UEVENT);
        if (sock_fd_ < 0) {
            return VoidResult<>::error(
                "cannot create netlink socket: " + std::string(strerror(errno)));
        }

        struct sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        addr.nl_pid = getpid();
        addr.nl_groups = 1; // KOBJECT_UEVENT multicast group

        if (bind(sock_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(sock_fd_);
            sock_fd_ = -1;
            return VoidResult<>::error(
                "cannot bind netlink socket: " + std::string(strerror(errno)));
        }
#endif

        running_.store(true);
        monitor_thread_ = std::thread([this]() { monitor_loop(); });

        return VoidResult<>::ok();
    }

    /** Stop monitoring. */
    void stop() {
        running_.store(false);
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
#ifdef __linux__
        if (sock_fd_ >= 0) {
            close(sock_fd_);
            sock_fd_ = -1;
        }
#endif
    }

    /** Register a callback for topology changes. */
    void on_change(TopologyChangeCallback cb) {
        std::lock_guard lock(mu_);
        callbacks_.push_back(std::move(cb));
    }

    /** Get the number of events processed since start. */
    uint64_t events_processed() const {
        return event_count_.load();
    }

    /** Check if monitoring is active. */
    bool is_running() const {
        return running_.load();
    }

private:
    Topology& topo_;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    int sock_fd_ = -1;
    std::atomic<uint64_t> event_count_{0};
    std::mutex mu_;
    std::vector<TopologyChangeCallback> callbacks_;

    void monitor_loop() {
        char buf[4096];

        while (running_.load()) {
#ifdef __linux__
            if (sock_fd_ < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            ssize_t len = recv(sock_fd_, buf, sizeof(buf) - 1, 0);
            if (len <= 0) {
                // EAGAIN on non-blocking socket — no event ready
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            buf[len] = '\0';

            auto event = parse_uevent(buf, static_cast<size_t>(len));
            if (event.action != UdevAction::Unknown) {
                handle_event(event);
                event_count_.fetch_add(1);
            }
#else
            // Non-Linux: poll for sysfs changes
            std::this_thread::sleep_for(std::chrono::seconds(5));
            // Rebuild full topology periodically on non-Linux (development)
            topo_.build_topology();
            event_count_.fetch_add(1);
#endif
        }
    }

    void handle_event(const UdevEvent& event) {
        fprintf(stdout, "[fabric-udev] %s: %s (%s/%s)\n",
                udev_action_str(event.action).c_str(),
                event.devpath.c_str(),
                event.subsystem.c_str(),
                event.devtype.c_str());

        switch (event.action) {
            case UdevAction::Add:
            case UdevAction::Bind:
                handle_device_add(event);
                break;
            case UdevAction::Remove:
            case UdevAction::Unbind:
                handle_device_remove(event);
                break;
            case UdevAction::Change:
                handle_device_change(event);
                break;
            default:
                break;
        }

        // Notify subscribers
        notify(event);
    }

    void handle_device_add(const UdevEvent& event) {
        // For new devices, rebuild the relevant portion of topology
        if (event.subsystem == "pci" || event.subsystem == "usb" ||
            event.subsystem == "net" || event.subsystem == "block" ||
            event.subsystem == "nvme") {
            // Full rebuild is simplest and correct — individual device
            // scanning would be more efficient but complex
            topo_.build_topology();
        }
    }

    void handle_device_remove(const UdevEvent& event) {
        // Try to find and remove the device from topology
        // The devpath gives us a hint about which device was removed
        auto nodes = topo_.get_all_nodes();
        for (auto& node : nodes) {
            // Check if node's sysfs path matches the removed devpath
            if (node.properties.count("bdf")) {
                if (event.devpath.find(node.properties.at("bdf")) != std::string::npos) {
                    topo_.remove_node(node.id);
                    return;
                }
            }
        }
        // Fallback: full rebuild
        topo_.build_topology();
    }

    void handle_device_change(const UdevEvent& event) {
        // Device properties changed (e.g., NIC link speed change)
        // Rebuild the affected device
        (void)event;
        topo_.build_topology();
    }

    void notify(const UdevEvent& event) {
        std::lock_guard lock(mu_);
        for (auto& cb : callbacks_) {
            cb(event);
        }
    }

    // ── uevent parser ───────────────────────────────────────────────

    static UdevEvent parse_uevent(const char* buf, size_t len) {
        UdevEvent event;

        // uevent format: key=value pairs separated by null bytes
        // First line is: ACTION@DEVPATH
        std::string first_line;
        size_t pos = 0;
        while (pos < len && buf[pos] != '\0') {
            first_line += buf[pos];
            ++pos;
        }
        ++pos; // skip null

        // Parse ACTION@DEVPATH
        auto at = first_line.find('@');
        if (at != std::string::npos) {
            auto action_str = first_line.substr(0, at);
            event.devpath = first_line.substr(at + 1);

            if (action_str == "add")         event.action = UdevAction::Add;
            else if (action_str == "remove")  event.action = UdevAction::Remove;
            else if (action_str == "change")  event.action = UdevAction::Change;
            else if (action_str == "move")    event.action = UdevAction::Move;
            else if (action_str == "bind")    event.action = UdevAction::Bind;
            else if (action_str == "unbind")  event.action = UdevAction::Unbind;
        }

        // Parse remaining key=value pairs
        while (pos < len) {
            std::string kv;
            while (pos < len && buf[pos] != '\0') {
                kv += buf[pos];
                ++pos;
            }
            ++pos; // skip null

            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            auto key = kv.substr(0, eq);
            auto val = kv.substr(eq + 1);

            if (key == "SUBSYSTEM")  event.subsystem = val;
            else if (key == "DEVTYPE") event.devtype = val;
            else if (key == "DRIVER")  event.driver = val;
            else if (key == "DEVPATH" && event.devpath.empty()) event.devpath = val;
            else if (key == "ACTION") {
                if (val == "add")         event.action = UdevAction::Add;
                else if (val == "remove") event.action = UdevAction::Remove;
                else if (val == "change") event.action = UdevAction::Change;
            }
        }

        return event;
    }
};

} // namespace straylight::fabric
