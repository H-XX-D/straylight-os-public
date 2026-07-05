// tools/sandbox/sandbox_manager.cpp
// Full implementation of namespace-based sandbox management.

#include "sandbox_manager.h"
#include "namespace_setup.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <signal.h>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

SandboxManager::SandboxManager() {
    std::error_code ec;
    fs::create_directories(kBaseDir, ec);
}

SandboxManager::~SandboxManager() = default;

std::string SandboxManager::sandbox_dir(const std::string& name) const {
    return std::string(kBaseDir) + "/" + name;
}
std::string SandboxManager::upper_dir(const std::string& name) const {
    return sandbox_dir(name) + "/upper";
}
std::string SandboxManager::work_dir(const std::string& name) const {
    return sandbox_dir(name) + "/work";
}
std::string SandboxManager::merged_dir(const std::string& name) const {
    return sandbox_dir(name) + "/merged";
}
std::string SandboxManager::config_path(const std::string& name) const {
    return sandbox_dir(name) + "/config.json";
}
std::string SandboxManager::pid_path(const std::string& name) const {
    return sandbox_dir(name) + "/init.pid";
}

// ---------------------------------------------------------------------------
// Config persistence (hand-rolled JSON, no external dependency)
// ---------------------------------------------------------------------------

void SandboxManager::write_config(const SandboxConfig& config) const {
    std::string path = config_path(config.name);
    std::ofstream out(path);
    if (!out.is_open()) return;

    out << "{\n";
    out << "  \"name\": \"" << config.name << "\",\n";
    out << "  \"gpu_passthrough\": " << (config.gpu_passthrough ? "true" : "false") << ",\n";
    out << "  \"network\": " << (config.network ? "true" : "false") << ",\n";
    out << "  \"memory_limit_mb\": " << config.memory_limit_mb << ",\n";
    out << "  \"cpu_shares\": " << config.cpu_shares << ",\n";
    out << "  \"base_image\": \"" << config.base_image << "\",\n";
    out << "  \"bind_mounts\": [";
    for (size_t i = 0; i < config.bind_mounts.size(); ++i) {
        if (i > 0) out << ", ";
        out << "\"" << config.bind_mounts[i] << "\"";
    }
    out << "]\n";
    out << "}\n";
}

Result<SandboxConfig, std::string>
SandboxManager::read_config(const std::string& name) const {
    std::string path = config_path(name);
    std::ifstream in(path);
    if (!in.is_open()) {
        return Result<SandboxConfig, std::string>::error(
            "config not found for sandbox '" + name + "'");
    }

    SandboxConfig cfg;
    cfg.name = name;

    std::string line;
    while (std::getline(in, line)) {
        auto extract_str = [&](const std::string& key) -> std::string {
            auto pos = line.find("\"" + key + "\"");
            if (pos == std::string::npos) return {};
            auto colon = line.find(':', pos);
            if (colon == std::string::npos) return {};
            auto q1 = line.find('"', colon + 1);
            auto q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) return {};
            return line.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_long = [&](const std::string& key) -> long long {
            auto pos = line.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            auto colon = line.find(':', pos);
            if (colon == std::string::npos) return -1;
            return std::atoll(line.c_str() + colon + 1);
        };
        auto extract_bool = [&](const std::string& key) -> int {
            auto pos = line.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            if (line.find("true", pos) != std::string::npos) return 1;
            return 0;
        };

        if (!extract_str("base_image").empty()) {
            cfg.base_image = extract_str("base_image");
        }
        auto mem = extract_long("memory_limit_mb");
        if (mem > 0) cfg.memory_limit_mb = static_cast<size_t>(mem);
        auto cpu = extract_long("cpu_shares");
        if (cpu > 0) cfg.cpu_shares = static_cast<size_t>(cpu);
        auto gpu = extract_bool("gpu_passthrough");
        if (gpu >= 0) cfg.gpu_passthrough = (gpu == 1);
        auto net = extract_bool("network");
        if (net >= 0) cfg.network = (net == 1);
    }

    return Result<SandboxConfig, std::string>::ok(std::move(cfg));
}

Result<std::string, std::string>
SandboxManager::run_cmd(const std::string& cmd) const {
    std::array<char, 4096> buf{};
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buf.data(), static_cast<int>(buf.size()), p)) {
        out += buf.data();
    }
    int rc = pclose(p);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd +
            "\n" + out);
    }
    return Result<std::string, std::string>::ok(out);
}

bool SandboxManager::is_running(const std::string& name) const {
    pid_t pid = read_pid(name);
    if (pid <= 0) return false;
    return kill(pid, 0) == 0;
}

pid_t SandboxManager::read_pid(const std::string& name) const {
    std::ifstream pf(pid_path(name));
    if (!pf.is_open()) return 0;
    pid_t pid = 0;
    pf >> pid;
    return pid;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<void, std::string>
SandboxManager::create(const SandboxConfig& config) {
    if (config.name.empty()) {
        return Result<void, std::string>::error("sandbox name must not be empty");
    }
    if (config.name.find('/') != std::string::npos ||
        config.name.find("..") != std::string::npos) {
        return Result<void, std::string>::error("invalid sandbox name");
    }

    std::string base = sandbox_dir(config.name);
    if (fs::exists(base)) {
        return Result<void, std::string>::error(
            "sandbox '" + config.name + "' already exists");
    }

    // Create directory structure.
    std::error_code ec;
    fs::create_directories(upper_dir(config.name), ec);
    fs::create_directories(work_dir(config.name), ec);
    fs::create_directories(merged_dir(config.name), ec);

    // Set up cgroup.
    auto cg = setup_cgroup(config.name, config.memory_limit_mb, config.cpu_shares);
    if (!cg.has_value()) {
        fs::remove_all(base, ec);
        return Result<void, std::string>::error(
            "cgroup setup failed: " + cg.error());
    }

    // Mount overlayfs (prepare the filesystem layers).
    std::string opts = "lowerdir=" + config.base_image +
                       ",upperdir=" + upper_dir(config.name) +
                       ",workdir=" + work_dir(config.name);
    if (mount("overlay", merged_dir(config.name).c_str(), "overlay", 0,
              opts.c_str()) != 0) {
        teardown_cgroup(config.name);
        fs::remove_all(base, ec);
        return Result<void, std::string>::error(
            "overlayfs mount failed: " + std::string(strerror(errno)));
    }

    // Apply additional bind mounts.
    for (const auto& bm : config.bind_mounts) {
        std::string target = merged_dir(config.name) + bm;
        fs::create_directories(target, ec);
        mount(bm.c_str(), target.c_str(), "", MS_BIND | MS_REC, nullptr);
    }

    // GPU passthrough if requested.
    if (config.gpu_passthrough) {
        auto gpu_res = setup_gpu_passthrough(merged_dir(config.name));
        if (!gpu_res.has_value()) {
            // Non-fatal — warn but continue.
        }
    }

    // Set hostname inside the sandbox.
    {
        std::string hostname_file = merged_dir(config.name) + "/etc/hostname";
        std::ofstream hf(hostname_file);
        if (hf.is_open()) {
            hf << config.name << "\n";
        }
    }

    // Fork an init process inside the sandbox namespaces.
    pid_t child = fork();
    if (child < 0) {
        teardown_mounts(merged_dir(config.name));
        teardown_cgroup(config.name);
        fs::remove_all(base, ec);
        return Result<void, std::string>::error(
            "fork failed: " + std::string(strerror(errno)));
    }

    if (child == 0) {
        // Child: unshare into new namespaces.
        if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS) != 0) {
            _exit(1);
        }

        // Set the UTS hostname.
        sethostname(config.name.c_str(), config.name.size());

        // Move into the cgroup.
        std::string cgroup_procs = cg.value() + "/cgroup.procs";
        std::ofstream cg_procs(cgroup_procs);
        if (cg_procs.is_open()) {
            cg_procs << getpid();
        }

        // Chroot into the merged root.
        if (chroot(merged_dir(config.name).c_str()) != 0) {
            _exit(1);
        }
        if (chdir("/") != 0) {
            _exit(1);
        }

        // Mount a fresh /proc for the new PID namespace.
        mount("proc", "/proc", "proc", 0, nullptr);

        // Act as init: sleep forever, reaping children.
        while (true) {
            int status = 0;
            pid_t w = waitpid(-1, &status, 0);
            if (w < 0 && errno == ECHILD) {
                pause();
            }
        }
        _exit(0);
    }

    // Parent: record the PID.
    {
        std::ofstream pf(pid_path(config.name));
        if (pf.is_open()) {
            pf << child;
        }
    }

    write_config(config);

    return Result<void, std::string>::ok();
}

Result<void, std::string>
SandboxManager::enter(const std::string& name) {
    if (!fs::exists(sandbox_dir(name))) {
        return Result<void, std::string>::error(
            "sandbox '" + name + "' does not exist");
    }

    pid_t pid = read_pid(name);
    if (pid <= 0 || kill(pid, 0) != 0) {
        return Result<void, std::string>::error(
            "sandbox '" + name + "' is not running");
    }

    // Use nsenter to join all namespaces of the init process, then run bash.
    std::string nsenter_cmd = "nsenter -t " + std::to_string(pid) +
                              " -m -p -u -r -w /bin/bash";
    int rc = ::system(nsenter_cmd.c_str());
    if (rc != 0) {
        return Result<void, std::string>::error(
            "nsenter failed with code " + std::to_string(rc));
    }

    return Result<void, std::string>::ok();
}

Result<std::string, std::string>
SandboxManager::run_in(const std::string& name, const std::string& cmd) {
    if (!fs::exists(sandbox_dir(name))) {
        return Result<std::string, std::string>::error(
            "sandbox '" + name + "' does not exist");
    }

    pid_t pid = read_pid(name);
    if (pid <= 0 || kill(pid, 0) != 0) {
        return Result<std::string, std::string>::error(
            "sandbox '" + name + "' is not running");
    }

    std::string full_cmd = "nsenter -t " + std::to_string(pid) +
                           " -m -p -u -r -w -- " + cmd + " 2>&1";
    return run_cmd(full_cmd);
}

std::vector<SandboxInfo> SandboxManager::list() const {
    std::vector<SandboxInfo> result;
    std::error_code ec;

    if (!fs::exists(kBaseDir)) return result;

    for (auto& entry : fs::directory_iterator(kBaseDir, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;

        SandboxInfo info;
        info.name = name;
        info.upper_path = upper_dir(name);
        info.merged_path = merged_dir(name);

        auto cfg = read_config(name);
        if (cfg.has_value()) {
            info.memory_limit_mb = cfg.value().memory_limit_mb;
            info.gpu = cfg.value().gpu_passthrough;
            info.network = cfg.value().network;
        }

        pid_t pid = read_pid(name);
        if (pid > 0 && kill(pid, 0) == 0) {
            info.state = "running";
            info.pid = pid;
        } else if (fs::exists(merged_dir(name))) {
            info.state = "stopped";
        } else {
            info.state = "created";
        }

        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(),
              [](const SandboxInfo& a, const SandboxInfo& b) {
                  return a.name < b.name;
              });

    return result;
}

Result<void, std::string>
SandboxManager::destroy(const std::string& name) {
    std::string base = sandbox_dir(name);
    if (!fs::exists(base)) {
        return Result<void, std::string>::error(
            "sandbox '" + name + "' does not exist");
    }

    // Kill the init process if running.
    pid_t pid = read_pid(name);
    if (pid > 0 && kill(pid, 0) == 0) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
    }

    // Tear down cgroup (kills remaining processes).
    teardown_cgroup(name);

    // Unmount overlayfs.
    teardown_mounts(merged_dir(name));

    // Remove the directory tree.
    std::error_code ec;
    fs::remove_all(base, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "failed to remove sandbox directory: " + ec.message());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string>
SandboxManager::snapshot(const std::string& name, const std::string& snap_name) {
    std::string upper = upper_dir(name);
    if (!fs::exists(upper)) {
        return Result<void, std::string>::error(
            "sandbox '" + name + "' does not exist or has no upper layer");
    }

    std::string snap_dir = sandbox_dir(name) + "/snapshots/" + snap_name;
    std::error_code ec;
    fs::create_directories(snap_dir, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "cannot create snapshot directory: " + ec.message());
    }

    std::string cmd = "cp -a " + upper + "/. " + snap_dir + "/ 2>&1";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error(
            "snapshot copy failed: " + res.error());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string>
SandboxManager::export_tar(const std::string& name, const std::string& path) {
    std::string merged = merged_dir(name);
    if (!fs::exists(merged)) {
        return Result<void, std::string>::error(
            "sandbox '" + name + "' has no merged root — is it created?");
    }

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::string cmd = "tar czf " + path + " -C " + merged + " . 2>&1";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error(
            "tar export failed: " + res.error());
    }

    return Result<void, std::string>::ok();
}

} // namespace straylight
