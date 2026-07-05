// tools/capsule/capsule_runner.cpp
#include "capsule_runner.h"
#include "capsule_installer.h"

#include <straylight/ipc_client.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace straylight {

// VPU ioctl definitions — StrayLight custom VPU driver interface
#define VPU_IOCTL_MAGIC 'V'
#define VPU_ALLOC_VRAM _IOW(VPU_IOCTL_MAGIC, 1, uint64_t)
#define VPU_FREE_VRAM  _IOW(VPU_IOCTL_MAGIC, 2, uint64_t)

Result<void, std::string> CapsuleRunner::preflight_check(const CapsuleManifest& manifest) {
    const auto& rc = manifest.resource_contract;

    // Check available RAM (not just total — check free)
    if (rc.min_ram_mb > 0) {
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        uint64_t available_kb = 0;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemAvailable:", 0) == 0) {
                std::sscanf(line.c_str(), "MemAvailable: %lu kB", &available_kb);
                break;
            }
        }
        uint32_t available_mb = static_cast<uint32_t>(available_kb / 1024);
        if (available_mb > 0 && available_mb < rc.min_ram_mb) {
            return Result<void, std::string>::error(
                "Insufficient RAM: need " + std::to_string(rc.min_ram_mb) +
                " MB, available " + std::to_string(available_mb) + " MB");
        }
    }

    // Check VRAM availability via VPU sysfs
    if (rc.min_vram_mb > 0) {
        uint32_t available_vram = 0;
        std::ifstream vram_file("/sys/class/vpu/vpu0/vram_available_mb");
        if (vram_file.is_open()) {
            vram_file >> available_vram;
        }
        if (available_vram > 0 && available_vram < rc.min_vram_mb) {
            return Result<void, std::string>::error(
                "Insufficient VRAM: need " + std::to_string(rc.min_vram_mb) +
                " MB, available " + std::to_string(available_vram) + " MB");
        }
    }

    // Check GPU compute availability via quota service
    if (rc.gpu_compute_percent > 0) {
        IpcJsonClient client;
        auto conn = client.connect("/run/straylight/quota.sock");
        if (conn.has_value()) {
            nlohmann::json req;
            req["jsonrpc"] = "2.0";
            req["method"] = "get_gpu_available";
            req["id"] = 1;
            auto resp = client.request(req);
            if (resp.has_value() && resp.value().contains("result")) {
                uint32_t available = resp.value()["result"].value("gpu_percent", 100u);
                if (available < rc.gpu_compute_percent) {
                    return Result<void, std::string>::error(
                        "Insufficient GPU: need " + std::to_string(rc.gpu_compute_percent) +
                        "%, available " + std::to_string(available) + "%");
                }
            }
        }
    }

    // Check mesh availability
    if (rc.requires_mesh) {
        IpcJsonClient client;
        auto conn = client.connect("/run/straylight/mesh.sock");
        if (!conn.has_value()) {
            return Result<void, std::string>::error(
                "Mesh service required but not available");
        }
    }

    // Check network
    if (rc.requires_network) {
        // Simple check: can we resolve DNS?
        std::ifstream resolv("/etc/resolv.conf");
        bool has_nameserver = false;
        std::string line;
        while (std::getline(resolv, line)) {
            if (line.rfind("nameserver", 0) == 0) {
                has_nameserver = true;
                break;
            }
        }
        if (!has_nameserver) {
            return Result<void, std::string>::error(
                "Network required but no nameservers configured");
        }
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CapsuleRunner::preallocate_vram(
    const std::string& name, uint32_t vram_mb) {
    if (vram_mb == 0) return Result<void, std::string>::ok();

    int fd = ::open("/dev/vpu0", O_RDWR);
    if (fd < 0) {
        return Result<void, std::string>::error(
            std::string("Cannot open /dev/vpu0: ") + ::strerror(errno) +
            " — VRAM pre-allocation unavailable");
    }

    uint64_t bytes = static_cast<uint64_t>(vram_mb) * 1024 * 1024;
    if (::ioctl(fd, VPU_ALLOC_VRAM, &bytes) < 0) {
        int e = errno;
        ::close(fd);
        return Result<void, std::string>::error(
            "VPU VRAM allocation failed for " + std::to_string(vram_mb) +
            " MB: " + ::strerror(e));
    }

    ::close(fd);

    // Record the allocation for later cleanup
    auto alloc_dir = std::filesystem::path("/var/lib/straylight/capsule/vram");
    std::filesystem::create_directories(alloc_dir);
    std::ofstream ofs(alloc_dir / (name + ".alloc"));
    ofs << vram_mb << "\n";

    return Result<void, std::string>::ok();
}

void CapsuleRunner::release_vram(const std::string& name) {
    auto alloc_file = std::filesystem::path("/var/lib/straylight/capsule/vram") /
        (name + ".alloc");
    if (!std::filesystem::exists(alloc_file)) return;

    std::ifstream ifs(alloc_file);
    uint32_t vram_mb = 0;
    ifs >> vram_mb;
    ifs.close();

    if (vram_mb > 0) {
        int fd = ::open("/dev/vpu0", O_RDWR);
        if (fd >= 0) {
            uint64_t bytes = static_cast<uint64_t>(vram_mb) * 1024 * 1024;
            ::ioctl(fd, VPU_FREE_VRAM, &bytes);
            ::close(fd);
        }
    }

    std::filesystem::remove(alloc_file);
}

Result<std::string, std::string> CapsuleRunner::create_cgroup(
    const std::string& name,
    const ResourceContract& contract) {
    std::string cgroup_path = "/sys/fs/cgroup/straylight-capsule-" + name;

    // Create cgroup directory
    std::filesystem::create_directories(cgroup_path);

    // Set memory limit
    if (contract.min_ram_mb > 0) {
        // Use min_ram_mb as the hard limit — the contract says "I need this much",
        // we give exactly that much and no more
        uint64_t limit_bytes = static_cast<uint64_t>(contract.min_ram_mb) * 1024 * 1024;
        std::ofstream mem_max(cgroup_path + "/memory.max");
        if (mem_max.is_open()) {
            mem_max << limit_bytes;
        }
    }

    // Set CPU limit
    if (contract.min_cpu_cores > 0) {
        // Set cpu.max: period is 100000us, quota is cores * period
        uint64_t period = 100000;
        uint64_t quota = contract.min_cpu_cores * period;
        std::ofstream cpu_max(cgroup_path + "/cpu.max");
        if (cpu_max.is_open()) {
            cpu_max << quota << " " << period;
        }
    }

    // Set IO weight (lower priority than system services)
    std::ofstream io_weight(cgroup_path + "/io.weight");
    if (io_weight.is_open()) {
        io_weight << "default 50";
    }

    return Result<std::string, std::string>::ok(std::move(cgroup_path));
}

void CapsuleRunner::remove_cgroup(const std::string& name) {
    std::string cgroup_path = "/sys/fs/cgroup/straylight-capsule-" + name;
    std::error_code ec;
    std::filesystem::remove_all(cgroup_path, ec);
}

Result<pid_t, std::string> CapsuleRunner::launch_in_cgroup(
    const std::string& cgroup_path,
    const std::string& binary_path,
    const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) {
        return Result<pid_t, std::string>::error(
            std::string("fork() failed: ") + ::strerror(errno));
    }

    if (pid == 0) {
        // Child process — add ourselves to the cgroup
        std::string procs_path = cgroup_path + "/cgroup.procs";
        std::ofstream procs(procs_path);
        if (procs.is_open()) {
            procs << ::getpid();
            procs.close();
        }

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(binary_path.c_str());
        for (const auto& arg : args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        // Create a new session
        ::setsid();

        // Redirect stdout/stderr to capsule log
        std::string log_dir = "/var/log/straylight/capsule";
        std::filesystem::create_directories(log_dir);
        std::string log_path = log_dir + "/" + std::filesystem::path(binary_path).stem().string() + ".log";
        int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }

        ::execv(binary_path.c_str(), const_cast<char* const*>(argv.data()));
        // If execv returns, it failed
        ::_exit(127);
    }

    // Parent — verify the child started successfully
    int status = 0;
    pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid && WIFEXITED(status)) {
        return Result<pid_t, std::string>::error(
            "Capsule process exited immediately with code " +
            std::to_string(WEXITSTATUS(status)));
    }

    return Result<pid_t, std::string>::ok(pid);
}

void CapsuleRunner::record_running(const std::string& name, pid_t pid,
                                    const ResourceContract& contract) {
    auto state_dir = std::filesystem::path(STATE_FILE).parent_path();
    std::filesystem::create_directories(state_dir);

    nlohmann::json state;
    if (std::filesystem::exists(STATE_FILE)) {
        std::ifstream ifs(STATE_FILE);
        try {
            ifs >> state;
        } catch (...) {
            state = nlohmann::json::object();
        }
    }

    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["pid"] = pid;
    entry["start_time"] = epoch;
    entry["contract"]["min_ram_mb"] = contract.min_ram_mb;
    entry["contract"]["min_vram_mb"] = contract.min_vram_mb;
    entry["contract"]["gpu_compute_percent"] = contract.gpu_compute_percent;
    entry["contract"]["min_cpu_cores"] = contract.min_cpu_cores;
    entry["contract"]["max_disk_mb"] = contract.max_disk_mb;
    entry["contract"]["requires_mesh"] = contract.requires_mesh;
    entry["contract"]["requires_network"] = contract.requires_network;

    state[name] = entry;

    std::ofstream ofs(STATE_FILE);
    ofs << state.dump(2);
}

void CapsuleRunner::unrecord_running(const std::string& name) {
    if (!std::filesystem::exists(STATE_FILE)) return;

    std::ifstream ifs(STATE_FILE);
    nlohmann::json state;
    try {
        ifs >> state;
    } catch (...) {
        return;
    }
    ifs.close();

    state.erase(name);

    std::ofstream ofs(STATE_FILE);
    ofs << state.dump(2);
}

RunningCapsule CapsuleRunner::read_usage(const std::string& name, pid_t pid,
                                          const ResourceContract& contract,
                                          uint64_t start_time) {
    RunningCapsule rc;
    rc.name = name;
    rc.pid = pid;
    rc.contract = contract;
    rc.current_ram_mb = 0;
    rc.current_vram_mb = 0;
    rc.cpu_usage_percent = 0.0f;

    auto now = std::chrono::system_clock::now();
    auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    rc.uptime_seconds = static_cast<uint64_t>(now_epoch) - start_time;

    // Read RSS from /proc/pid/statm
    std::string statm_path = "/proc/" + std::to_string(pid) + "/statm";
    std::ifstream statm(statm_path);
    if (statm.is_open()) {
        uint64_t size_pages = 0, rss_pages = 0;
        statm >> size_pages >> rss_pages;
        long page_size = ::sysconf(_SC_PAGESIZE);
        rc.current_ram_mb = static_cast<uint32_t>(
            rss_pages * static_cast<uint64_t>(page_size) / (1024 * 1024));
    }

    // Read CPU usage from /proc/pid/stat
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream stat_file(stat_path);
    if (stat_file.is_open()) {
        std::string line;
        std::getline(stat_file, line);
        // Skip past the comm field (which may contain spaces and parens)
        auto close_paren = line.rfind(')');
        if (close_paren != std::string::npos) {
            std::istringstream iss(line.substr(close_paren + 2));
            std::string state;
            uint64_t fields[50] = {};
            iss >> state;
            for (int i = 0; i < 50 && iss; ++i) {
                iss >> fields[i];
            }
            // fields[11] = utime, fields[12] = stime (0-indexed from after state)
            uint64_t total_ticks = fields[11] + fields[12];
            long hz = ::sysconf(_SC_CLK_TCK);
            if (hz > 0 && rc.uptime_seconds > 0) {
                double cpu_seconds = static_cast<double>(total_ticks) / hz;
                rc.cpu_usage_percent = static_cast<float>(
                    cpu_seconds / static_cast<double>(rc.uptime_seconds) * 100.0);
            }
        }
    }

    // Read VRAM usage from VPU sysfs if applicable
    if (contract.min_vram_mb > 0) {
        std::string vram_path = "/sys/class/vpu/vpu0/proc/" + std::to_string(pid) + "/vram_mb";
        std::ifstream vram_file(vram_path);
        if (vram_file.is_open()) {
            vram_file >> rc.current_vram_mb;
        }
    }

    return rc;
}

Result<pid_t, std::string> CapsuleRunner::run(
    const std::string& name,
    const std::vector<std::string>& args) {
    // Get installed capsule info
    auto installed = CapsuleInstaller::get_installed(name);
    if (!installed.has_value()) {
        return Result<pid_t, std::string>::error(installed.error());
    }

    auto install_dir = std::filesystem::path(installed.value().install_path);

    // Load full manifest for binary path
    auto manifest_path = install_dir / "capsule.json";
    std::ifstream mf(manifest_path);
    if (!mf.is_open()) {
        return Result<pid_t, std::string>::error(
            "Cannot read manifest for installed capsule: " + name);
    }
    std::string manifest_str((std::istreambuf_iterator<char>(mf)),
                             std::istreambuf_iterator<char>());
    auto manifest_result = CapsuleManifestParser::parse(manifest_str);
    if (!manifest_result.has_value()) {
        return Result<pid_t, std::string>::error(manifest_result.error());
    }
    const auto& manifest = manifest_result.value();

    std::cout << "capsule: launching " << name << " v" << manifest.version << "\n";

    // Pre-flight resource check
    auto preflight = preflight_check(manifest);
    if (!preflight.has_value()) {
        return Result<pid_t, std::string>::error(
            "Pre-flight failed: " + preflight.error());
    }

    // Pre-allocate VRAM if specified
    if (manifest.resource_contract.min_vram_mb > 0) {
        std::cout << "capsule: pre-allocating " << manifest.resource_contract.min_vram_mb
                  << " MB VRAM\n";
        auto vram_result = preallocate_vram(name, manifest.resource_contract.min_vram_mb);
        if (!vram_result.has_value()) {
            // VRAM pre-allocation failure is fatal — fail fast
            return Result<pid_t, std::string>::error(
                "VRAM pre-allocation failed: " + vram_result.error());
        }
    }

    // Create cgroup with resource limits
    auto cgroup_result = create_cgroup(name, manifest.resource_contract);
    if (!cgroup_result.has_value()) {
        release_vram(name);
        return Result<pid_t, std::string>::error(
            "Cgroup creation failed: " + cgroup_result.error());
    }

    auto binary_path = (install_dir / manifest.binary_path).string();

    // Launch inside cgroup
    auto launch_result = launch_in_cgroup(cgroup_result.value(), binary_path, args);
    if (!launch_result.has_value()) {
        release_vram(name);
        remove_cgroup(name);
        return Result<pid_t, std::string>::error(launch_result.error());
    }

    pid_t pid = launch_result.value();

    // Record the running capsule
    record_running(name, pid, manifest.resource_contract);

    std::cout << "capsule: " << name << " running (pid " << pid << ")\n";

    // Print contract enforcement summary
    const auto& rc = manifest.resource_contract;
    std::cout << "capsule: enforcing — "
              << "RAM≤" << rc.min_ram_mb << "MB"
              << " CPU≤" << rc.min_cpu_cores << " cores";
    if (rc.min_vram_mb > 0) {
        std::cout << " VRAM=" << rc.min_vram_mb << "MB(pre-allocated)";
    }
    std::cout << "\n";

    return Result<pid_t, std::string>::ok(pid);
}

Result<void, std::string> CapsuleRunner::stop(const std::string& name) {
    if (!std::filesystem::exists(STATE_FILE)) {
        return Result<void, std::string>::error("No running capsules");
    }

    std::ifstream ifs(STATE_FILE);
    nlohmann::json state;
    try {
        ifs >> state;
    } catch (...) {
        return Result<void, std::string>::error("Cannot read running state");
    }
    ifs.close();

    if (!state.contains(name)) {
        return Result<void, std::string>::error("Capsule not running: " + name);
    }

    pid_t pid = state[name].value("pid", 0);
    if (pid <= 0) {
        unrecord_running(name);
        return Result<void, std::string>::error("Invalid PID for capsule: " + name);
    }

    std::cout << "capsule: stopping " << name << " (pid " << pid << ")\n";

    // Send SIGTERM first
    if (::kill(pid, SIGTERM) < 0) {
        if (errno == ESRCH) {
            // Process already dead
            unrecord_running(name);
            release_vram(name);
            remove_cgroup(name);
            return Result<void, std::string>::ok();
        }
    }

    // Wait up to 5 seconds for graceful shutdown
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            break;
        }
        ::usleep(100000); // 100ms
    }

    // Force kill if still running
    if (::kill(pid, 0) == 0) {
        std::cout << "capsule: force-killing " << name << "\n";
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
    }

    // Cleanup
    release_vram(name);
    remove_cgroup(name);
    unrecord_running(name);

    std::cout << "capsule: " << name << " stopped\n";
    return Result<void, std::string>::ok();
}

std::vector<RunningCapsule> CapsuleRunner::list_running() {
    std::vector<RunningCapsule> result;

    if (!std::filesystem::exists(STATE_FILE)) return result;

    std::ifstream ifs(STATE_FILE);
    nlohmann::json state;
    try {
        ifs >> state;
    } catch (...) {
        return result;
    }

    std::vector<std::string> dead_names;

    for (auto& [name, entry] : state.items()) {
        pid_t pid = entry.value("pid", 0);
        if (pid <= 0) continue;

        // Check if process is still alive
        if (::kill(pid, 0) < 0 && errno == ESRCH) {
            dead_names.push_back(name);
            continue;
        }

        ResourceContract contract;
        if (entry.contains("contract")) {
            const auto& c = entry["contract"];
            contract.min_ram_mb = c.value("min_ram_mb", 0u);
            contract.min_vram_mb = c.value("min_vram_mb", 0u);
            contract.gpu_compute_percent = c.value("gpu_compute_percent", 0u);
            contract.min_cpu_cores = c.value("min_cpu_cores", 1u);
            contract.max_disk_mb = c.value("max_disk_mb", 0u);
            contract.requires_mesh = c.value("requires_mesh", false);
            contract.requires_network = c.value("requires_network", false);
        }

        uint64_t start_time = entry.value("start_time", static_cast<uint64_t>(0));

        auto running = read_usage(name, pid, contract, start_time);
        result.push_back(std::move(running));
    }

    // Clean up dead entries
    for (const auto& dead : dead_names) {
        unrecord_running(dead);
        release_vram(dead);
        remove_cgroup(dead);
    }

    return result;
}

} // namespace straylight
