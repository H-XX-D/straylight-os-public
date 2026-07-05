// services/notify/dbus_bridge.cpp
// org.freedesktop.Notifications D-Bus interface via sd-bus.
//
// Spec: https://specifications.freedesktop.org/notification-spec/latest/
//   Method signatures (D-Bus type strings):
//     Notify              → (susssasa{sv}i) → u
//     CloseNotification   → (u)             → ()
//     GetCapabilities     → ()              → as
//     GetServerInformation→ ()              → ssss
//   Signals:
//     NotificationClosed  → (uu)
//     ActionInvoked       → (us)

#include "dbus_bridge.h"
#include <straylight/log.h>

#include <cstring>

namespace straylight {

// ── vtable (D-Bus method/signal declarations) ─────────────────────────

const sd_bus_vtable DBusBridge::kVtable[] = {
    SD_BUS_VTABLE_START(0),

    // Methods
    SD_BUS_METHOD(
        "Notify",
        "susssasa{sv}i",  // in:  app_name, replaces_id, app_icon, summary,
                          //      body, actions[], hints{}, expire_timeout
        "u",              // out: notification_id
        method_notify,
        SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_METHOD(
        "CloseNotification",
        "u",              // in:  id
        "",               // out: (none)
        method_close_notification,
        SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_METHOD(
        "GetCapabilities",
        "",               // in:  (none)
        "as",             // out: capabilities[]
        method_get_capabilities,
        SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_METHOD(
        "GetServerInformation",
        "",               // in:  (none)
        "ssss",           // out: name, vendor, version, spec_version
        method_get_server_information,
        SD_BUS_VTABLE_UNPRIVILEGED),

    // Signals
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_SIGNAL("ActionInvoked",      "us", 0),

    SD_BUS_VTABLE_END
};

// ── Constructor / Destructor ──────────────────────────────────────────

DBusBridge::DBusBridge(NotificationEngine& engine)
    : engine_(engine) {}

DBusBridge::~DBusBridge() {
    close();
}

// ── Lifecycle ─────────────────────────────────────────────────────────

Result<void, SLError> DBusBridge::open() {
    int r;

    // Connect to the session bus.
    r = sd_bus_open_user(&bus_);
    if (r < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("sd_bus_open_user failed: ") + strerror(-r)});
    }

    // Register the object path and vtable.
    r = sd_bus_add_object_vtable(
        bus_,
        &slot_,
        "/org/freedesktop/Notifications",   // object path
        "org.freedesktop.Notifications",    // interface
        kVtable,
        this);                              // userdata (passed to handlers)
    if (r < 0) {
        sd_bus_unref(bus_);
        bus_ = nullptr;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("sd_bus_add_object_vtable failed: ") + strerror(-r)});
    }

    // Acquire the well-known bus name.
    r = sd_bus_request_name(bus_, "org.freedesktop.Notifications", 0);
    if (r < 0) {
        SL_WARN("notify: cannot acquire org.freedesktop.Notifications ({}): "
                "another notification daemon may be running", strerror(-r));
        // Non-fatal: keep going; IPC socket still works for StrayLight tools.
    }

    SL_INFO("notify: D-Bus bridge open on org.freedesktop.Notifications");

    // Wire up engine callbacks to emit D-Bus signals.
    engine_.set_action_callback([this](uint32_t id, const std::string& key) {
        emit_action(id, key);
    });

    return Result<void, SLError>::ok();
}

void DBusBridge::process() {
    if (!bus_) return;
    // Drain all pending messages without blocking.
    for (;;) {
        int r = sd_bus_process(bus_, nullptr);
        if (r <= 0) break;  // 0 = nothing to do, <0 = error
    }
}

void DBusBridge::close() {
    if (slot_) {
        sd_bus_slot_unref(slot_);
        slot_ = nullptr;
    }
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
}

// ── Signal emitters ───────────────────────────────────────────────────

void DBusBridge::emit_closed(uint32_t id, uint32_t reason) {
    if (!bus_) return;
    sd_bus_emit_signal(bus_,
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed",
        "uu", id, reason);
}

void DBusBridge::emit_action(uint32_t id, const std::string& action_key) {
    if (!bus_) return;
    sd_bus_emit_signal(bus_,
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "ActionInvoked",
        "us", id, action_key.c_str());
}

// ── Method handlers ───────────────────────────────────────────────────

int DBusBridge::method_notify(sd_bus_message* m, void* userdata,
                               sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<DBusBridge*>(userdata);

    const char* app_name   = nullptr;
    uint32_t    replaces_id = 0;
    const char* app_icon   = nullptr;
    const char* summary    = nullptr;
    const char* body       = nullptr;
    int32_t     expire_ms  = -1;

    // Read fixed fields: s u s s s
    int r = sd_bus_message_read(m, "susss",
        &app_name, &replaces_id, &app_icon, &summary, &body);
    if (r < 0) {
        // Message malformed — return generic id so caller doesn't crash.
        return sd_bus_reply_method_return(m, "u", (uint32_t)0);
    }

    // Skip actions array (a{s}) — we just need the count for capability
    r = sd_bus_message_skip(m, "as");
    if (r < 0) {
        sd_bus_message_skip(m, nullptr); // best-effort
    }

    // Skip hints dict (a{sv})
    r = sd_bus_message_skip(m, "a{sv}");
    if (r < 0) {
        sd_bus_message_skip(m, nullptr);
    }

    // Read expire_timeout
    sd_bus_message_read(m, "i", &expire_ms);
    if (expire_ms == 0) expire_ms = 5000;  // 0 means "default" per spec

    // Forward to engine
    auto result = self->engine_.notify(
        app_name  ? app_name  : "",
        summary   ? summary   : "",
        body      ? body      : "",
        Urgency::Normal, {}, app_icon ? app_icon : "",
        expire_ms == -1 ? 8000 : expire_ms);

    uint32_t notif_id = result.has_value() ? result.value() : 0;
    SL_DEBUG("notify: D-Bus Notify from '{}' → id {}", app_name ? app_name : "", notif_id);

    return sd_bus_reply_method_return(m, "u", notif_id);
}

int DBusBridge::method_close_notification(sd_bus_message* m, void* userdata,
                                           sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<DBusBridge*>(userdata);
    uint32_t id = 0;
    sd_bus_message_read(m, "u", &id);
    self->engine_.dismiss(id);
    self->emit_closed(id, 3 /*CloseNotification called*/);
    return sd_bus_reply_method_return(m, "");
}

int DBusBridge::method_get_capabilities(sd_bus_message* m,
                                         void* /*userdata*/,
                                         sd_bus_error* /*ret_error*/) {
    // Report capabilities: body text, actions, persistence, DND support
    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    sd_bus_message_open_container(reply, 'a', "s");
    sd_bus_message_append_basic(reply, 's', "body");
    sd_bus_message_append_basic(reply, 's', "body-markup");
    sd_bus_message_append_basic(reply, 's', "actions");
    sd_bus_message_append_basic(reply, 's', "persistence");
    sd_bus_message_append_basic(reply, 's', "icon-static");
    sd_bus_message_close_container(reply);
    int r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

int DBusBridge::method_get_server_information(sd_bus_message* m,
                                               void* /*userdata*/,
                                               sd_bus_error* /*ret_error*/) {
    return sd_bus_reply_method_return(m, "ssss",
        "straylight-notify",   // name
        "StrayLight OS",       // vendor
        "1.0.0",               // version
        "1.2");                // spec version
}

} // namespace straylight
