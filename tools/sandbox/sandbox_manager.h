// tools/sandbox/sandbox_manager.h
// Lightweight isolated environments using Linux namespaces + overlayfs.
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <string>
#include <vector>

namespace straylight {

struct SandboxConfig {
    std::string name;
    bool gpu_passthrough = false;     // Bind /dev/dri/* and /dev/nvidia*
    bool network = true;              // Create veth pair or share host
    size_t memory_limit_mb = 4096;
    size_t cpu_shares = 1024;
    std::string base_image = "/";     // Default: overlay on host root
    std::vector<std::string> bind_mounts;  // Additional host paths to mount
};

struct SandboxInfo {
    std::string name;
    std::string state;               // "running" | "stopped" | "created"
    pid_t pid = 0;                   // init PID inside sandbox (0 if stopped)
    size_t memory_limit_mb = 0;
    bool gpu = false;
    bool network = false;
    std::string upper_path;          // writable layer path
    std::string merged_path;         // merged root path
};

class SandboxManager {
public:
    SandboxManager();
    ~SandboxManager();

    /// Create a new sandbox with the given configuration.
    Result<void, std::string> create(const SandboxConfig& config);

    /// Enter a running sandbox interactively (/bin/bash).
    Result<void, std::string> enter(const std::string& name);

    /// Run a single command inside the sandbox and return its output.
    Result<std::string, std::string> run_in(const std::string& name,
                                             const std::string& cmd);

    /// List all sandboxes.
    std::vector<SandboxInfo> list() const;

    /// Destroy a sandbox (kill processes, unmount, remove files).
    Result<void, std::string> destroy(const std::string& name);

    /// Snapshot the sandbox's writable layer.
    Result<void, std::string> snapshot(const std::string& name,
                                       const std::string& snap_name);

    /// Export the sandbox as a tar.gz archive.
    Result<void, std::string> export_tar(const std::string& name,
                                          const std::string& path);

private:
    static constexpr const char* kBaseDir = "/var/lib/straylight/sandboxes";

    std::string sandbox_dir(const std::string& name) const;
    std::string upper_dir(const std::string& name) const;
    std::string work_dir(const std::string& name) const;
    std::string merged_dir(const std::string& name) const;
    std::string config_path(const std::string& name) const;
    std::string pid_path(const std::string& name) const;

    void write_config(const SandboxConfig& config) const;
    Result<SandboxConfig, std::string> read_config(const std::string& name) const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    bool is_running(const std::string& name) const;
    pid_t read_pid(const std::string& name) const;
};

} // namespace straylight
