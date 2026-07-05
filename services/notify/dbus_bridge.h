// services/notify/dbus_bridge.h
// Implements org.freedesktop.Notifications on the session bus via sd-bus.
// Owns the bus connection and vtable slot; calls into NotificationEngine
// for all business logic.
//
// Usage:
//   DBusBridge bridge(engine_);
//   bridge.open();           // call once after engine is ready
//   bridge.process();        // call every tick() iteration
//   bridge.close();          // call in shutdown()
//
// Signals emitted back to callers:
//   NotificationClosed(id, reason)  — reason: 1=expired,2=dismissed,3=requested,4=undef
//   ActionInvoked(id, action_key)   — when user clicks an action button
#pragma once

#include "notification_engine.h"
#include <straylight/result.h>
#include <straylight/error.h>

#include <systemd/sd-bus.h>
#include <string>

namespace straylight {

class DBusBridge {
public:
    explicit DBusBridge(NotificationEngine& engine);
    ~DBusBridge();

    // Non-copyable, non-movable (raw C pointer ownership)
    DBusBridge(const DBusBridge&) = delete;
    DBusBridge& operator=(const DBusBridge&) = delete;

    /// Acquire bus name and register the vtable. Safe to call without a
    /// running bus — returns an error but does not abort the daemon.
    Result<void, SLError> open();

    /// Process pending bus messages (call in every tick). Non-blocking.
    void process();

    /// Release the vtable slot and unref the bus connection.
    void close();

    /// Emit NotificationClosed signal.
    void emit_closed(uint32_t id, uint32_t reason);

    /// Emit ActionInvoked signal.
    void emit_action(uint32_t id, const std::string& action_key);

    bool is_open() const noexcept { return bus_ != nullptr; }

private:
    // ── sd-bus vtable method handlers ─────────────────────────────

    static int method_notify(sd_bus_message* m, void* userdata,
                             sd_bus_error* ret_error);

    static int method_close_notification(sd_bus_message* m, void* userdata,
                                         sd_bus_error* ret_error);

    static int method_get_capabilities(sd_bus_message* m, void* userdata,
                                       sd_bus_error* ret_error);

    static int method_get_server_information(sd_bus_message* m,
                                              void* userdata,
                                              sd_bus_error* ret_error);

    static const sd_bus_vtable kVtable[];

    NotificationEngine& engine_;
    sd_bus*      bus_  = nullptr;
    sd_bus_slot* slot_ = nullptr;
};

} // namespace straylight
