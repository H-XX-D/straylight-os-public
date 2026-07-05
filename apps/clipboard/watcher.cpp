// apps/clipboard/watcher.cpp
// Wayland clipboard watcher implementation.
#include "watcher.h"

#include <straylight/log.h>

#include <wayland-client.h>

#include <cstring>
#include <unistd.h>  // pipe, read, close
#include <fcntl.h>   // fcntl
#include <algorithm>

namespace straylight::clipboard {

// ---------------------------------------------------------------------------
// Internal Wayland listener structures
// ---------------------------------------------------------------------------

namespace {

// wl_data_offer listener
struct OfferContext {
    ClipboardWatcher*        watcher = nullptr;
    wl_data_offer*           offer   = nullptr;
    std::vector<std::string> mimes;
};

static void offer_offer(void* data, wl_data_offer*, const char* mime_type) {
    auto* ctx = static_cast<OfferContext*>(data);
    ctx->mimes.emplace_back(mime_type);
}
static void offer_source_actions(void*, wl_data_offer*, uint32_t) {}
static void offer_action(void*, wl_data_offer*, uint32_t) {}

static const wl_data_offer_listener offer_listener = {
    offer_offer,
    offer_source_actions,
    offer_action,
};

// wl_data_device listener — per-device state
struct DeviceContext {
    ClipboardWatcher*        watcher    = nullptr;
    OfferContext*            cur_offer  = nullptr;
    std::vector<OfferContext*> all_offers;
};

static void device_data_offer(void* data, wl_data_device*, wl_data_offer* offer) {
    auto* dctx = static_cast<DeviceContext*>(data);
    auto* octx = new OfferContext;
    octx->watcher = dctx->watcher;
    octx->offer   = offer;
    wl_data_offer_add_listener(offer, &offer_listener, octx);
    dctx->all_offers.push_back(octx);
}

static void device_enter(void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                          wl_data_offer*) {}
static void device_leave(void*, wl_data_device*) {}
static void device_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
static void device_drop(void*, wl_data_device*) {}

static void device_selection(void* data, wl_data_device*, wl_data_offer* offer) {
    auto* dctx = static_cast<DeviceContext*>(data);

    if (!offer) return; // clipboard cleared

    // Find the context for this offer
    for (auto* octx : dctx->all_offers) {
        if (octx->offer == offer) {
            dctx->watcher->on_selection(offer);
            // Store reference to current offer context — read_offer will use mimes
            dctx->cur_offer = octx;
            dctx->watcher->read_offer(offer, octx->mimes);
            break;
        }
    }

    // Clean up old offer contexts (not the current one)
    auto new_end = std::remove_if(dctx->all_offers.begin(), dctx->all_offers.end(),
        [offer](OfferContext* o) {
            if (o->offer != offer) {
                wl_data_offer_destroy(o->offer);
                delete o;
                return true;
            }
            return false;
        });
    dctx->all_offers.erase(new_end, dctx->all_offers.end());
}

static const wl_data_device_listener device_listener = {
    device_data_offer,
    device_enter,
    device_leave,
    device_motion,
    device_drop,
    device_selection,
};

// Registry
static void registry_global_static(void* data, wl_registry* reg,
                                    uint32_t name, const char* iface, uint32_t ver) {
    auto* w = static_cast<ClipboardWatcher*>(data);
    w->registry_global(data, reg, name, iface, ver);
}
static void registry_global_remove_static(void* data, wl_registry* reg, uint32_t name) {
    auto* w = static_cast<ClipboardWatcher*>(data);
    w->registry_global_remove(data, reg, name);
}

static const wl_registry_listener registry_listener_impl = {
    registry_global_static,
    registry_global_remove_static,
};

} // namespace

// ---------------------------------------------------------------------------
// ClipboardWatcher
// ---------------------------------------------------------------------------

ClipboardWatcher::ClipboardWatcher() = default;

ClipboardWatcher::~ClipboardWatcher() { stop(); }

Result<void, SLError> ClipboardWatcher::connect() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "Cannot connect to Wayland display"});
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener_impl, this);
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_); // Second roundtrip to get seat capabilities

    if (!ddm_ || !seat_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "wl_data_device_manager or wl_seat not available"});
    }

    // Create data device for our seat
    device_ = wl_data_device_manager_get_data_device(ddm_, seat_);

    // Create a context for the device listener
    auto* dctx = new DeviceContext;
    dctx->watcher = this;
    wl_data_device_add_listener(device_, &device_listener, dctx);

    return Result<void, SLError>::ok();
}

void ClipboardWatcher::start(NewEntryCallback cb) {
    callback_ = std::move(cb);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { watch_loop(); });
}

void ClipboardWatcher::stop() {
    running_.store(false, std::memory_order_relaxed);
    // Interrupt the wl_display_dispatch() call by sending a no-op
    if (display_) wl_display_cancel_read(display_);
    if (thread_.joinable()) thread_.join();
    if (device_)   { wl_data_device_destroy(device_);   device_   = nullptr; }
    if (ddm_)      { wl_data_device_manager_destroy(ddm_); ddm_   = nullptr; }
    if (seat_)     { wl_seat_destroy(seat_);               seat_   = nullptr; }
    if (registry_) { wl_registry_destroy(registry_);       registry_ = nullptr; }
    if (display_)  { wl_display_disconnect(display_);       display_  = nullptr; }
}

void ClipboardWatcher::watch_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        if (wl_display_dispatch(display_) < 0) break;
    }
}

void ClipboardWatcher::read_offer(wl_data_offer* offer,
                                   const std::vector<std::string>& mimes) {
    // Prefer: text/plain;charset=utf-8 > text/plain > image/png > image/jpeg
    std::string preferred_mime;
    bool is_image = false;

    for (const std::string& m : mimes) {
        if (m == "text/plain;charset=utf-8") { preferred_mime = m; is_image = false; break; }
        if (m == "text/plain" && preferred_mime.empty()) { preferred_mime = m; is_image = false; }
        if ((m == "image/png" || m == "image/jpeg") && preferred_mime.empty()) {
            preferred_mime = m; is_image = true;
        }
    }

    if (preferred_mime.empty()) return;

    // Create a pipe and receive the data
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) return;

    wl_data_offer_receive(offer, preferred_mime.c_str(), fds[1]);
    ::close(fds[1]);
    wl_display_flush(display_);

    // Read from read end
    std::vector<uint8_t> buf;
    {
        char tmp[4096];
        ssize_t n;
        while ((n = ::read(fds[0], tmp, sizeof(tmp))) > 0) {
            buf.insert(buf.end(), tmp, tmp + n);
        }
    }
    ::close(fds[0]);

    if (buf.empty()) return;

    ClipEntry e;
    e.timestamp = std::chrono::system_clock::now();
    e.mime      = preferred_mime;

    if (is_image) {
        e.kind       = EntryKind::Image;
        e.image_data = std::move(buf);
    } else {
        e.kind = EntryKind::Text;
        e.text = std::string(buf.begin(), buf.end());
    }

    if (callback_) callback_(std::move(e));
}

void ClipboardWatcher::on_selection(wl_data_offer* /*offer*/) {
    // Notification before read_offer — nothing to do here currently
}

// ---------------------------------------------------------------------------
// Static registry callbacks
// ---------------------------------------------------------------------------

void ClipboardWatcher::registry_global(void* /*data*/, wl_registry* reg,
                                        uint32_t name, const char* iface, uint32_t ver) {
    if (!std::strcmp(iface, wl_seat_interface.name) && !seat_) {
        seat_ = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 7u)));
    } else if (!std::strcmp(iface, wl_data_device_manager_interface.name) && !ddm_) {
        ddm_ = static_cast<wl_data_device_manager*>(
            wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                             std::min(ver, 3u)));
    }
}

void ClipboardWatcher::registry_global_remove(void* /*data*/, wl_registry* /*reg*/,
                                               uint32_t /*name*/) {}

} // namespace straylight::clipboard
