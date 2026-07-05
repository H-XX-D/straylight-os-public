// tools/sandbox/namespace_setup.cpp
// Full implementation of Linux namespace / cgroup / overlayfs setup.

#include "namespace_setup.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Internal helper: run a shell command, return stdout or error.
// ---------------------------------------------------------------------------
static Result<std::string, std::string> shell(const std::string& cmd) {
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

static std::string sandbox_network_value(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    std::string candidate(value);
    if (candidate.find_first_not_of("0123456789abcdefABCDEF.:/") != std::string::npos) {
        return fallback;
    }
    return candidate;
}

// ---------------------------------------------------------------------------
// Mount namespace with overlayfs + pivot_root
// ---------------------------------------------------------------------------
Result<void, std::string> setup_mount_namespace(const std::string& lower,
                                                 const std::string& upper,
                                                 const std::string& work,
                                                 const std::string& merged) {
    // Ensure directories exist.
    std::error_code ec;
    fs::create_directories(upper, ec);
    fs::create_directories(work, ec);
    fs::create_directories(merged, ec);

    // Unshare the mount namespace so mounts are private.
    if (unshare(CLONE_NEWNS) != 0) {
        return Result<void, std::string>::error(
            "unshare(CLONE_NEWNS) failed: " + std::string(strerror(errno)));
    }

    // Make all existing mounts private so changes don't propagate.
    if (mount("", "/", "", MS_PRIVATE | MS_REC, nullptr) != 0) {
        return Result<void, std::string>::error(
            "mount --make-rprivate / failed: " + std::string(strerror(errno)));
    }

    // Mount overlayfs.
    std::string opts = "lowerdir=" + lower +
                       ",upperdir=" + upper +
                       ",workdir=" + work;
    if (mount("overlay", merged.c_str(), "overlay", 0, opts.c_str()) != 0) {
        return Result<void, std::string>::error(
            "mount overlay failed: " + std::string(strerror(errno)) +
            " opts=" + opts);
    }

    // Bind-mount essential filesystems into the merged root.
    struct BindMount {
        const char* src;
        const char* rel;
        unsigned long flags;
    };
    BindMount binds[] = {
        {"/proc", "/proc", MS_BIND | MS_REC},
        {"/sys", "/sys", MS_BIND | MS_REC},
        {"/dev", "/dev", MS_BIND | MS_REC},
        {"/run", "/run", MS_BIND | MS_REC},
    };
    for (auto& bm : binds) {
        std::string target = merged + bm.rel;
        fs::create_directories(target, ec);
        if (mount(bm.src, target.c_str(), "", bm.flags, nullptr) != 0) {
            // Non-fatal — some may not exist in minimal environments.
        }
    }

    // pivot_root into the merged directory.
    std::string old_root = merged + "/.old_root";
    fs::create_directories(old_root, ec);

    if (syscall(SYS_pivot_root, merged.c_str(), old_root.c_str()) != 0) {
        return Result<void, std::string>::error(
            "pivot_root failed: " + std::string(strerror(errno)));
    }

    if (chdir("/") != 0) {
        return Result<void, std::string>::error(
            "chdir(/) failed: " + std::string(strerror(errno)));
    }

    // Lazily unmount the old root.
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        // Non-fatal — may already be unmounted.
    }
    fs::remove("/.old_root", ec);

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// PID namespace
// ---------------------------------------------------------------------------
Result<int, std::string> setup_pid_namespace() {
    // Clone into a new PID namespace.
    pid_t pid = static_cast<pid_t>(
        syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, nullptr));
    if (pid < 0) {
        return Result<int, std::string>::error(
            "clone(CLONE_NEWPID) failed: " + std::string(strerror(errno)));
    }
    return Result<int, std::string>::ok(static_cast<int>(pid));
}

// ---------------------------------------------------------------------------
// Network namespace with veth pair
// ---------------------------------------------------------------------------
Result<void, std::string> setup_net_namespace(pid_t pid,
                                               const std::string& inner_ip,
                                               bool share_host) {
    if (share_host) {
        // Nothing to do — sandbox shares the host network stack.
        return Result<void, std::string>::ok();
    }

    std::string veth_host = "veth-h-" + std::to_string(pid);
    std::string veth_sandbox = "veth-s-" + std::to_string(pid);

    // Truncate to max interface name length (15 chars).
    if (veth_host.size() > 15) veth_host.resize(15);
    if (veth_sandbox.size() > 15) veth_sandbox.resize(15);

    // Create the veth pair.
    auto r1 = shell("ip link add " + veth_host + " type veth peer name " +
                     veth_sandbox + " 2>&1");
    if (!r1.has_value()) {
        return Result<void, std::string>::error("veth create: " + r1.error());
    }

    // Move one end into the sandbox's network namespace.
    auto r2 = shell("ip link set " + veth_sandbox + " netns " +
                     std::to_string(pid) + " 2>&1");
    if (!r2.has_value()) {
        return Result<void, std::string>::error("veth move: " + r2.error());
    }

    const std::string host_cidr = sandbox_network_value(
        "STRAYLIGHT_SANDBOX_HOST_CIDR", "198.18.0.1/24");
    const std::string gateway = sandbox_network_value(
        "STRAYLIGHT_SANDBOX_GATEWAY", "198.18.0.1");
    const std::string nat_cidr = sandbox_network_value(
        "STRAYLIGHT_SANDBOX_NAT_CIDR", "198.18.0.0/24");

    // Configure host end with a point-to-point address.
    auto r3 = shell("ip addr add " + host_cidr + " dev " + veth_host + " 2>&1");
    if (!r3.has_value()) {
        return Result<void, std::string>::error(
            "host veth addr: " + r3.error());
    }
    auto r4 = shell("ip link set " + veth_host + " up 2>&1");
    if (!r4.has_value()) {
        return Result<void, std::string>::error(
            "host veth up: " + r4.error());
    }

    // Configure sandbox end via nsenter.
    auto r5 = shell("nsenter -t " + std::to_string(pid) +
                     " -n ip addr add " + inner_ip + " dev " +
                     veth_sandbox + " 2>&1");
    if (!r5.has_value()) {
        return Result<void, std::string>::error(
            "sandbox veth addr: " + r5.error());
    }
    auto r6 = shell("nsenter -t " + std::to_string(pid) +
                     " -n ip link set " + veth_sandbox + " up 2>&1");
    if (!r6.has_value()) {
        return Result<void, std::string>::error(
            "sandbox veth up: " + r6.error());
    }
    auto r7 = shell("nsenter -t " + std::to_string(pid) +
                     " -n ip link set lo up 2>&1");
    if (!r7.has_value()) {
        // Non-fatal.
    }

    // Set default route inside the sandbox.
    auto r8 = shell("nsenter -t " + std::to_string(pid) +
                     " -n ip route add default via " + gateway + " 2>&1");
    if (!r8.has_value()) {
        // Non-fatal.
    }

    // Enable IP forwarding and NAT on the host.
    shell("sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1");
    shell("iptables -t nat -A POSTROUTING -s " + nat_cidr + " -j MASQUERADE 2>&1");

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Cgroup v2
// ---------------------------------------------------------------------------
Result<std::string, std::string> setup_cgroup(const std::string& name,
                                               size_t memory_limit_mb,
                                               size_t cpu_shares) {
    std::string cgroup_base = "/sys/fs/cgroup";
    std::string cgroup_path = cgroup_base + "/straylight-" + name;

    std::error_code ec;
    fs::create_directories(cgroup_path, ec);
    if (ec) {
        return Result<std::string, std::string>::error(
            "cannot create cgroup dir: " + ec.message());
    }

    // Enable memory and cpu controllers in the parent.
    {
        std::ofstream ctl(cgroup_base + "/cgroup.subtree_control");
        if (ctl.is_open()) {
            ctl << "+memory +cpu +pids";
        }
    }

    // Set memory limit.
    {
        std::ofstream mem(cgroup_path + "/memory.max");
        if (mem.is_open()) {
            mem << (memory_limit_mb * 1024ULL * 1024ULL);
        } else {
            return Result<std::string, std::string>::error(
                "cannot write memory.max for cgroup " + cgroup_path);
        }
    }

    // Set memory swap limit equal to memory limit (no swap).
    {
        std::ofstream swap(cgroup_path + "/memory.swap.max");
        if (swap.is_open()) {
            swap << (memory_limit_mb * 1024ULL * 1024ULL);
        }
    }

    // Set CPU weight (cgroup v2 uses "weight" 1-10000, default 100).
    // Map the traditional "shares" (default 1024) proportionally.
    size_t weight = (cpu_shares * 100) / 1024;
    if (weight < 1) weight = 1;
    if (weight > 10000) weight = 10000;
    {
        std::ofstream cpu(cgroup_path + "/cpu.weight");
        if (cpu.is_open()) {
            cpu << weight;
        }
    }

    // Set a generous PID limit.
    {
        std::ofstream pids(cgroup_path + "/pids.max");
        if (pids.is_open()) {
            pids << 4096;
        }
    }

    return Result<std::string, std::string>::ok(cgroup_path);
}

// ---------------------------------------------------------------------------
// GPU passthrough
// ---------------------------------------------------------------------------
Result<void, std::string> setup_gpu_passthrough(const std::string& merged_root) {
    std::error_code ec;

    // Bind-mount /dev/dri/* devices.
    std::string dri_src = "/dev/dri";
    std::string dri_dst = merged_root + "/dev/dri";
    if (fs::exists(dri_src)) {
        fs::create_directories(dri_dst, ec);
        if (mount(dri_src.c_str(), dri_dst.c_str(), "", MS_BIND | MS_REC, nullptr) != 0) {
            return Result<void, std::string>::error(
                "bind mount /dev/dri failed: " + std::string(strerror(errno)));
        }
    }

    // Bind-mount NVIDIA devices if present.
    std::vector<std::string> nvidia_devs;
    if (fs::exists("/dev")) {
        for (auto& entry : fs::directory_iterator("/dev", ec)) {
            std::string fname = entry.path().filename().string();
            if (fname.rfind("nvidia", 0) == 0) {
                nvidia_devs.push_back(entry.path().string());
            }
        }
    }
    for (const auto& dev : nvidia_devs) {
        std::string dst = merged_root + dev;
        // Ensure parent directory exists.
        fs::create_directories(fs::path(dst).parent_path(), ec);
        // Create the device node placeholder if needed.
        {
            std::ofstream touch(dst);
        }
        if (mount(dev.c_str(), dst.c_str(), "", MS_BIND, nullptr) != 0) {
            // Non-fatal for individual devices.
        }
    }

    // Bind-mount NVIDIA driver libraries so CUDA / Vulkan work.
    std::vector<std::string> lib_dirs = {
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib64",
        "/usr/lib",
    };
    for (const auto& dir : lib_dirs) {
        if (!fs::exists(dir)) continue;
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            std::string fname = entry.path().filename().string();
            if (fname.find("libnvidia") != std::string::npos ||
                fname.find("libcuda") != std::string::npos ||
                fname.find("libEGL_nvidia") != std::string::npos ||
                fname.find("libGLESv2_nvidia") != std::string::npos) {
                std::string src = entry.path().string();
                std::string dst = merged_root + src;
                fs::create_directories(fs::path(dst).parent_path(), ec);
                {
                    std::ofstream touch(dst);
                }
                mount(src.c_str(), dst.c_str(), "", MS_BIND, nullptr);
            }
        }
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Teardown helpers
// ---------------------------------------------------------------------------
Result<void, std::string> teardown_cgroup(const std::string& name) {
    std::string cgroup_path = "/sys/fs/cgroup/straylight-" + name;

    // Kill all processes in the cgroup first.
    std::string procs_file = cgroup_path + "/cgroup.procs";
    std::ifstream procs(procs_file);
    if (procs.is_open()) {
        std::string pid_str;
        while (std::getline(procs, pid_str)) {
            if (pid_str.empty()) continue;
            pid_t p = static_cast<pid_t>(std::atoi(pid_str.c_str()));
            if (p > 1) {
                kill(p, SIGKILL);
            }
        }
        procs.close();
    }

    // Wait briefly for processes to die.
    usleep(100000); // 100ms

    // Remove the cgroup directory (kernel does actual cleanup).
    std::error_code ec;
    fs::remove_all(cgroup_path, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "cgroup cleanup failed: " + ec.message());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> teardown_mounts(const std::string& merged) {
    // Lazy-unmount the overlayfs and sub-mounts.
    if (umount2(merged.c_str(), MNT_DETACH) != 0) {
        return Result<void, std::string>::error(
            "umount2 failed for " + merged + ": " + std::string(strerror(errno)));
    }
    return Result<void, std::string>::ok();
}

} // namespace straylight
