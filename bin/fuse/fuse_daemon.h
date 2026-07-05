// bin/fuse/fuse_daemon.h
// DaemonBase subclass wrapping a fuse_session.
// Uses FUSE low-level API (fuse3/fuse_lowlevel.h) via FuseOps from tensor_fs.h.
#pragma once

#define FUSE_USE_VERSION 35
#include <fuse3/fuse_lowlevel.h>

#include "tensor_fs.h"

#include <straylight/daemon.h>
#include <straylight/error.h>
#include <straylight/result.h>

#include <filesystem>
#include <string>

namespace straylight::fuse {

/// Persistent daemon that mounts a tensor compression FUSE filesystem.
///
/// Lifecycle driven by DaemonBase::run():
///   init()     — creates fuse_session, mounts at mountpoint, scans store_dir
///   tick()     — processes one FUSE request buffer (non-blocking, EINTR safe)
///   shutdown() — unmounts filesystem and destroys the session
class FuseDaemon : public straylight::DaemonBase {
public:
    FuseDaemon() = default;
    ~FuseDaemon() override;

    /// Initialise the daemon from config.
    /// Config keys:
    ///   fuse.mountpoint  (default: /var/lib/straylight/tensors)
    ///   fuse.store_dir   (default: /var/lib/straylight/tensor-store)
    ///   fuse.cache_mb    (default: 512)
    Result<void, SLError> init(const straylight::Config& cfg) override;

    /// Process one pending FUSE request. Returns immediately if no request ready.
    Result<void, SLError> tick() override;

    /// Unmount filesystem and destroy FUSE session.
    void shutdown() override;

private:
    FuseOps               ops_;
    struct fuse_session*  session_{nullptr};
    std::filesystem::path mountpoint_;
    std::filesystem::path store_dir_;
};

} // namespace straylight::fuse
