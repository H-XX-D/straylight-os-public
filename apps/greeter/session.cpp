// apps/greeter/session.cpp
// ext-session-lock-v1 session lock implementation
#include "session.h"

#include <straylight/log.h>

#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"

namespace straylight::greeter {

struct SessionLock::Impl {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_surface* wl_surf = nullptr;
    ext_session_lock_manager_v1* lock_manager = nullptr;
    ext_session_lock_v1* lock = nullptr;
    ext_session_lock_surface_v1* lock_surface = nullptr;
    int width_  = 1920;
    int height_ = 1080;
    bool locked_ = false;

    ~Impl() {
        if (locked_) {
            unlock();
        }
        if (wl_surf) {
            wl_surface_destroy(wl_surf);
        }
    }

    void unlock() {
        if (lock_surface) {
            ext_session_lock_surface_v1_destroy(lock_surface);
            lock_surface = nullptr;
        }
        if (lock) {
            ext_session_lock_v1_unlock_and_destroy(lock);
            lock = nullptr;
        }
        locked_ = false;
        SL_INFO("Session lock released");
    }
};

// Registry listener to find lock manager and compositor
struct LockRegistryState {
    ext_session_lock_manager_v1* lock_manager = nullptr;
    wl_compositor* compositor = nullptr;
    wl_output* output = nullptr;
};

static void lock_registry_global(void* data, wl_registry* registry,
                                 uint32_t name, const char* interface,
                                 uint32_t version) {
    auto* state = static_cast<LockRegistryState*>(data);
    const std::string iface(interface);

    if (iface == "ext_session_lock_manager_v1") {
        state->lock_manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(registry, name,
                             static_cast<const wl_interface*>(nullptr),
                             std::min(version, 1u)));
    } else if (iface == "wl_compositor") {
        state->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min(version, 4u)));
    } else if (iface == "wl_output") {
        state->output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface,
                             std::min(version, 4u)));
    }
}

static void lock_registry_global_remove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener lock_registry_listener = {
    .global        = lock_registry_global,
    .global_remove = lock_registry_global_remove,
};

SessionLock::SessionLock(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SessionLock::~SessionLock() = default;
SessionLock::SessionLock(SessionLock&& other) noexcept = default;
SessionLock& SessionLock::operator=(SessionLock&& other) noexcept = default;

Result<SessionLock, SLError> SessionLock::acquire(wl_display* display) {
    if (!display) {
        return Result<SessionLock, SLError>::error(
            SLError{SLErrorCode::InvalidArgument,
                    "Null Wayland display"});
    }

    // Bind globals
    LockRegistryState reg_state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &lock_registry_listener, &reg_state);
    wl_display_roundtrip(display);

    if (!reg_state.lock_manager) {
        wl_registry_destroy(registry);
        return Result<SessionLock, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "ext_session_lock_manager_v1 not available — "
                    "compositor does not support session locking"});
    }

    if (!reg_state.compositor) {
        wl_registry_destroy(registry);
        return Result<SessionLock, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "wl_compositor not available"});
    }

    auto impl = std::make_unique<Impl>();
    impl->display      = display;
    impl->compositor   = reg_state.compositor;
    impl->lock_manager = reg_state.lock_manager;

    // Acquire the lock
    impl->lock = ext_session_lock_manager_v1_lock(impl->lock_manager);
    if (!impl->lock) {
        wl_registry_destroy(registry);
        return Result<SessionLock, SLError>::error(
            SLError{SLErrorCode::PermissionDenied,
                    "Session lock denied — another locker may be active"});
    }

    // Create surface for lock
    impl->wl_surf = wl_compositor_create_surface(impl->compositor);
    if (!impl->wl_surf) {
        wl_registry_destroy(registry);
        return Result<SessionLock, SLError>::error(
            SLError{SLErrorCode::Internal,
                    "Failed to create wl_surface for lock"});
    }

    // Create lock surface for the first output
    if (reg_state.output) {
        impl->lock_surface = ext_session_lock_v1_get_lock_surface(
            impl->lock, impl->wl_surf, reg_state.output);
    }

    wl_surface_commit(impl->wl_surf);
    wl_display_roundtrip(display);

    impl->locked_ = true;
    SL_INFO("Session lock acquired");

    wl_registry_destroy(registry);
    return Result<SessionLock, SLError>::ok(
        SessionLock(std::move(impl)));
}

void SessionLock::unlock_and_destroy() {
    if (impl_) {
        impl_->unlock();
    }
}

wl_surface* SessionLock::surface() const {
    return impl_ ? impl_->wl_surf : nullptr;
}

int SessionLock::width() const {
    return impl_ ? impl_->width_ : 0;
}

int SessionLock::height() const {
    return impl_ ? impl_->height_ : 0;
}

bool SessionLock::is_locked() const {
    return impl_ ? impl_->locked_ : false;
}

} // namespace straylight::greeter
