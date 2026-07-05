// apps/clipboard/watcher.h
// Wayland clipboard watcher: monitors wl_data_device_manager for new clipboard
// content and fires callbacks when a new entry is available.
#pragma once

#include "history.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

// Forward declarations for Wayland types
struct wl_display;
struct wl_registry;
struct wl_seat;
struct wl_data_device_manager;
struct wl_data_device;
struct wl_data_offer;

namespace straylight::clipboard {

/// Fired when a new clipboard entry arrives (called from the watcher thread).
using NewEntryCallback = std::function<void(ClipEntry)>;

/// Monitors the Wayland clipboard via wl_data_device_manager.
/// Runs an internal thread that keeps a wl_display event loop alive.
class ClipboardWatcher {
public:
    ClipboardWatcher();
    ~ClipboardWatcher();

    ClipboardWatcher(const ClipboardWatcher&) = delete;
    ClipboardWatcher& operator=(const ClipboardWatcher&) = delete;

    /// Connect to the running Wayland compositor.
    /// Must be called before start().
    Result<void, SLError> connect();

    /// Start the background monitoring thread.
    void start(NewEntryCallback cb);

    /// Stop the monitoring thread and disconnect.
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }

private:
    wl_display*              display_   = nullptr;
    wl_registry*             registry_  = nullptr;
    wl_seat*                 seat_      = nullptr;
    wl_data_device_manager*  ddm_       = nullptr;
    wl_data_device*          device_    = nullptr;
    wl_data_offer*           current_offer_ = nullptr;

    std::vector<std::string> offer_mimes_;  ///< MIME types offered by current selection

    NewEntryCallback         callback_;
    std::thread              thread_;
    std::atomic<bool>        running_{false};

    /// Background loop that calls wl_display_dispatch() repeatedly.
    void watch_loop();

    /// Request clipboard content from an offer, selecting the best MIME type.
    void read_offer(wl_data_offer* offer, const std::vector<std::string>& mimes);

    /// Callback invoked (from the event thread) when a new data offer is available.
    void on_selection(wl_data_offer* offer);

    /// Static Wayland listener callbacks — delegate to `this`.
    static void registry_global(void* data, wl_registry* reg,
                                 uint32_t name, const char* iface, uint32_t ver);
    static void registry_global_remove(void* data, wl_registry* reg, uint32_t name);
};

} // namespace straylight::clipboard
