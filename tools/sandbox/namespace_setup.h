// tools/sandbox/namespace_setup.h
// Linux namespace, cgroup, and overlayfs helpers for StrayLight sandboxes.
#pragma once

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight {

/// Set up a mount namespace with overlayfs rooted at the sandbox directory.
/// lower  = base image path (usually "/")
/// upper  = writable upper layer
/// work   = overlayfs work directory
/// merged = final mount point
Result<void, std::string> setup_mount_namespace(const std::string& lower,
                                                 const std::string& upper,
                                                 const std::string& work,
                                                 const std::string& merged);

/// Fork into a new PID namespace.  Returns child PID to the parent, 0 to the child.
/// On error returns -1 with an error message.
Result<int, std::string> setup_pid_namespace();

/// Create a veth pair and move one end into the network namespace of `pid`.
/// Configures the inner interface with the given IP (CIDR notation, e.g. "198.18.0.2/24").
/// If `share_host` is true, skip namespace isolation and share the host stack.
Result<void, std::string> setup_net_namespace(pid_t pid,
                                               const std::string& inner_ip,
                                               bool share_host);

/// Create a cgroup v2 hierarchy for the sandbox and apply limits.
/// Returns the cgroup path (e.g. /sys/fs/cgroup/straylight-<name>).
Result<std::string, std::string> setup_cgroup(const std::string& name,
                                               size_t memory_limit_mb,
                                               size_t cpu_shares);

/// Bind-mount GPU devices (/dev/dri/*, /dev/nvidia*) and driver libraries
/// into the sandbox merged root.
Result<void, std::string> setup_gpu_passthrough(const std::string& merged_root);

/// Clean up a cgroup hierarchy for a sandbox.
Result<void, std::string> teardown_cgroup(const std::string& name);

/// Unmount overlayfs and clean up mount points.
Result<void, std::string> teardown_mounts(const std::string& merged);

} // namespace straylight
