// bin/fuse/fuse_daemon.cpp
// FuseDaemon — DaemonBase subclass wrapping a fuse_session (low-level FUSE3 API).

#include "fuse_daemon.h"

#include <straylight/log.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace straylight::fuse {

FuseDaemon::~FuseDaemon() {
    if (session_) {
        // Guard against destruction without shutdown being called.
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

Result<void, SLError> FuseDaemon::init(const straylight::Config& cfg) {
    mountpoint_ = cfg.get<std::string>(
        "fuse.mountpoint", "/var/lib/straylight/tensors");
    store_dir_ = cfg.get<std::string>(
        "fuse.store_dir", "/var/lib/straylight/tensor-store");
    const size_t cache_mb = static_cast<size_t>(
        cfg.get<int>("fuse.cache_mb", 512));

    // Ensure directories exist.
    std::error_code ec;
    std::filesystem::create_directories(mountpoint_, ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError,
             "Cannot create mountpoint " + mountpoint_.string() + ": " + ec.message()});
    }
    std::filesystem::create_directories(store_dir_, ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError,
             "Cannot create store dir " + store_dir_.string() + ": " + ec.message()});
    }

    // Initialise FuseOps (scans store_dir for .slt files).
    if (auto r = ops_.init(store_dir_, cache_mb * 1024 * 1024); !r.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotInitialized,
             "FuseOps init failed: " + r.error()});
    }

    // Create FUSE low-level session. libfuse expects at least argv[0].
    // This daemon is mounted by systemd as root and serves a shared runtime
    // view, so expose it beyond the mounting process/user.
    char prog[] = "straylight-fuse";
    char opt[] = "-o";
    char allow_other[] = "allow_other";
    char* argv[] = {prog, opt, allow_other};
    struct fuse_args args = FUSE_ARGS_INIT(3, argv);
    session_ = fuse_session_new(
        &args,
        &FuseOps::get_ops(),
        sizeof(fuse_lowlevel_ops),
        nullptr /* userdata; callbacks use global g_ops */);

    if (!session_) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotInitialized, "fuse_session_new returned null"});
    }

    if (fuse_session_mount(session_, mountpoint_.c_str()) != 0) {
        fuse_session_destroy(session_);
        session_ = nullptr;
        return Result<void, SLError>::error(
            {SLErrorCode::IOError,
             "fuse_session_mount failed at " + mountpoint_.string() +
             " (is fusermount3 installed and mountpoint accessible?)"});
    }

    SL_INFO("straylight-fuse: mounted at {} (store={}, cache={}MB)",
            mountpoint_.string(), store_dir_.string(), cache_mb);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// tick — process one FUSE request
// ---------------------------------------------------------------------------

Result<void, SLError> FuseDaemon::tick() {
    if (!session_) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotInitialized, "FUSE session not initialised"});
    }

    // fuse_session_receive_buf blocks until a request arrives, the session
    // exits, or is interrupted.  We call it with FUSE_BUF_SPLICE_MOVE so
    // the buffer is dynamically allocated by libfuse and must be freed.
    struct fuse_buf fbuf{};
    int res = fuse_session_receive_buf(session_, &fbuf);

    if (res == 0) {
        // Session exited cleanly (unmounted by external fusermount3 -u).
        return Result<void, SLError>::ok();
    }
    if (res == -EINTR) {
        // Interrupted by a signal — normal during shutdown.
        return Result<void, SLError>::ok();
    }
    if (res < 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError,
             "fuse_session_receive_buf error: " + std::string(strerror(-res))});
    }

    fuse_session_process_buf(session_, &fbuf);

    // fbuf.mem is heap-allocated by libfuse when FUSE_BUF_IS_FD is not set.
    if (fbuf.mem) {
        free(fbuf.mem);
    }

    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------

void FuseDaemon::shutdown() {
    if (!session_) return;

    SL_INFO("straylight-fuse: unmounting {}", mountpoint_.string());
    fuse_session_unmount(session_);
    fuse_session_destroy(session_);
    session_ = nullptr;
    SL_INFO("straylight-fuse: shutdown complete");
}

} // namespace straylight::fuse
