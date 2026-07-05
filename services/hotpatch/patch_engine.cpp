/**
 * StrayLight Hotpatch — Patch Engine (implementation)
 */

#include "patch_engine.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::hotpatch {

PatchEngine::PatchEngine(PatchRegistry& registry)
    : registry_(registry) {}

// ── Kernel livepatch via sysfs ──────────────────────────────────────

Result<std::string, std::string> PatchEngine::apply_kernel_patch(
        const std::string& module_name,
        const std::string& patch_path) {
    namespace fs = std::filesystem;

    // Verify patch file exists
    if (!fs::exists(patch_path)) {
        return Result<std::string, std::string>::error(
            "patch file not found: " + patch_path);
    }

    // Load the livepatch kernel module via insmod
    // The livepatch module must be compiled as a .ko with klp_patch struct
    std::string cmd = "insmod " + patch_path + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "failed to execute insmod");
    }
    char buf[512];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    int ret = pclose(pipe);

    if (ret != 0) {
        return Result<std::string, std::string>::error(
            "insmod failed: " + output);
    }

    // Verify the livepatch is active via sysfs
    std::string sysfs_path = "/sys/kernel/livepatch/" + module_name;
    bool livepatch_active = false;

    // Poll for up to 2 seconds for livepatch activation
    for (int i = 0; i < 20; ++i) {
        std::string enabled_path = sysfs_path + "/enabled";
        std::ifstream enabled_file(enabled_path);
        if (enabled_file) {
            int val = 0;
            enabled_file >> val;
            if (val == 1) {
                livepatch_active = true;
                break;
            }
        }
        usleep(100000); // 100ms
    }

    // Check transition status
    std::string transition_path = sysfs_path + "/transition";
    std::ifstream transition_file(transition_path);
    if (transition_file) {
        int transitioning = 0;
        transition_file >> transitioning;
        if (transitioning) {
            // Wait for transition to complete (up to 10 seconds)
            for (int i = 0; i < 100; ++i) {
                std::ifstream tf(transition_path);
                int t = 0;
                tf >> t;
                if (t == 0) break;
                usleep(100000);
            }
        }
    }

    // Record in registry
    std::string patch_id = PatchRegistry::generate_id();
    PatchRecord rec{};
    rec.patch_id = patch_id;
    rec.type = PatchType::Kernel;
    rec.target = module_name;
    rec.patch_source = patch_path;
    rec.applied_at = PatchRegistry::now_iso8601();
    rec.status = livepatch_active ? PatchStatus::Applied : PatchStatus::Failed;
    rec.rollback_data = module_name; // module name needed for rmmod
    rec.description = "Kernel livepatch for module " + module_name;

    auto add_res = registry_.add(std::move(rec));
    if (!add_res) {
        return Result<std::string, std::string>::error(
            "patch applied but registry write failed: " + add_res.err());
    }

    if (!livepatch_active) {
        return Result<std::string, std::string>::error(
            "livepatch loaded but not active — check /sys/kernel/livepatch/" +
            module_name);
    }

    return Result<std::string, std::string>::ok(patch_id);
}

// ── Daemon hot-reload via SIGHUP ────────────────────────────────────

Result<std::string, std::string> PatchEngine::apply_daemon_patch(
        const std::string& service_name,
        const std::string& patch_data) {
    // Read the daemon's PID
    auto pid_result = read_daemon_pid(service_name);
    if (!pid_result) {
        return Result<std::string, std::string>::error(pid_result.err());
    }
    pid_t pid = pid_result.value();

    // Verify process is alive
    if (kill(pid, 0) != 0) {
        return Result<std::string, std::string>::error(
            "daemon " + service_name + " (pid " + std::to_string(pid) +
            ") is not running");
    }

    // Write patch data to the daemon's reload spool directory
    std::string spool_dir = "/var/lib/straylight/hotpatch/spool/" + service_name;
    std::string patch_id = PatchRegistry::generate_id();
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(spool_dir, ec);

    std::string spool_file = spool_dir + "/" + patch_id + ".patch";
    {
        std::ofstream out(spool_file, std::ios::trunc);
        if (!out) {
            return Result<std::string, std::string>::error(
                "cannot write spool file: " + spool_file);
        }
        out << patch_data;
    }

    // Send SIGHUP to trigger the daemon's on_reload()
    if (kill(pid, SIGHUP) != 0) {
        return Result<std::string, std::string>::error(
            "failed to send SIGHUP to pid " + std::to_string(pid) +
            ": " + strerror(errno));
    }

    // Give the daemon time to process
    usleep(250000); // 250ms

    // Verify process is still alive (didn't crash on reload)
    bool still_alive = (kill(pid, 0) == 0);

    PatchRecord rec{};
    rec.patch_id = patch_id;
    rec.type = PatchType::Daemon;
    rec.target = service_name;
    rec.patch_source = spool_file;
    rec.applied_at = PatchRegistry::now_iso8601();
    rec.status = still_alive ? PatchStatus::Applied : PatchStatus::Failed;
    rec.rollback_data = service_name; // service to restart on rollback
    rec.description = "Daemon reload for " + service_name;

    registry_.add(std::move(rec));

    if (!still_alive) {
        return Result<std::string, std::string>::error(
            "daemon " + service_name + " crashed after SIGHUP");
    }

    return Result<std::string, std::string>::ok(patch_id);
}

// ── Config live-patch ───────────────────────────────────────────────

Result<std::string, std::string> PatchEngine::apply_config_patch(
        const std::string& config_path,
        const std::string& diff) {
    namespace fs = std::filesystem;

    if (!fs::exists(config_path)) {
        return Result<std::string, std::string>::error(
            "config file not found: " + config_path);
    }

    // Read original content for rollback
    std::ifstream in(config_path);
    if (!in) {
        return Result<std::string, std::string>::error(
            "cannot read config file: " + config_path);
    }
    std::string original((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    in.close();

    // Apply the diff
    auto apply_result = apply_unified_diff(config_path, diff);
    if (!apply_result) {
        return Result<std::string, std::string>::error(apply_result.err());
    }

    // Determine owning service from config path and send SIGHUP
    // Convention: /etc/straylight/<service>/... -> service name
    std::string service_name;
    fs::path cp(config_path);
    if (cp.string().find("/etc/straylight/") == 0) {
        auto rel = fs::relative(cp, "/etc/straylight/");
        auto it = rel.begin();
        if (it != rel.end()) service_name = it->string();
    }

    if (!service_name.empty()) {
        auto pid_result = read_daemon_pid(service_name);
        if (pid_result) {
            kill(pid_result.value(), SIGHUP);
        }
        // Not an error if the service isn't running — config is still patched
    }

    std::string patch_id = PatchRegistry::generate_id();
    PatchRecord rec{};
    rec.patch_id = patch_id;
    rec.type = PatchType::Config;
    rec.target = config_path;
    rec.patch_source = "(inline diff)";
    rec.applied_at = PatchRegistry::now_iso8601();
    rec.status = PatchStatus::Applied;
    rec.rollback_data = original;
    rec.description = "Config patch for " + config_path;

    registry_.add(std::move(rec));
    return Result<std::string, std::string>::ok(patch_id);
}

// ── Rollback ────────────────────────────────────────────────────────

VoidResult<> PatchEngine::rollback(const std::string& patch_id) {
    auto rec_result = registry_.get(patch_id);
    if (!rec_result) {
        return VoidResult<>::error(rec_result.err());
    }

    const auto& rec = rec_result.value();
    if (rec.status != PatchStatus::Applied) {
        return VoidResult<>::error(
            "patch " + patch_id + " is not in 'applied' state");
    }

    VoidResult<> result = VoidResult<>::ok();

    switch (rec.type) {
        case PatchType::Kernel:
            result = remove_kernel_patch(rec.rollback_data);
            break;

        case PatchType::Daemon: {
            // Restart the daemon service to revert to pre-patch state
            auto pid_result = read_daemon_pid(rec.target);
            if (pid_result) {
                // Send SIGTERM then wait for restart via service manager
                kill(pid_result.value(), SIGTERM);
                usleep(500000);
                // Attempt to restart via systemctl or direct exec
                std::string cmd = "systemctl restart straylight-" +
                                  rec.target + " 2>/dev/null";
                int sys_ret = system(cmd.c_str());
                (void)sys_ret;
            }
            // Clean up spool file
            std::error_code ec;
            std::filesystem::remove(rec.patch_source, ec);
            break;
        }

        case PatchType::Config:
            result = restore_file(rec.target, rec.rollback_data);
            if (result) {
                // Notify owning service
                namespace fs = std::filesystem;
                fs::path cp(rec.target);
                std::string service_name;
                if (cp.string().find("/etc/straylight/") == 0) {
                    auto rel = fs::relative(cp, "/etc/straylight/");
                    auto it = rel.begin();
                    if (it != rel.end()) service_name = it->string();
                }
                if (!service_name.empty()) {
                    auto pid_result = read_daemon_pid(service_name);
                    if (pid_result) {
                        kill(pid_result.value(), SIGHUP);
                    }
                }
            }
            break;
    }

    if (!result) return result;

    registry_.update_status(patch_id, PatchStatus::RolledBack);
    return VoidResult<>::ok();
}

// ── List / Status ───────────────────────────────────────────────────

std::vector<PatchRecord> PatchEngine::list(
        const std::string& status_filter) const {
    auto all = registry_.all();
    if (status_filter.empty()) return all;

    PatchStatus filter = patch_status_from_str(status_filter);
    std::vector<PatchRecord> filtered;
    std::copy_if(all.begin(), all.end(), std::back_inserter(filtered),
        [&](const PatchRecord& r) { return r.status == filter; });
    return filtered;
}

Result<PatchRecord, std::string> PatchEngine::status(
        const std::string& patch_id) const {
    return registry_.get(patch_id);
}

// ── Private helpers ─────────────────────────────────────────────────

Result<pid_t, std::string> PatchEngine::read_daemon_pid(
        const std::string& service_name) {
    std::string pid_path = "/var/run/straylight/" + service_name + ".pid";
    std::ifstream pf(pid_path);
    if (!pf) {
        return Result<pid_t, std::string>::error(
            "cannot read pid file: " + pid_path);
    }
    pid_t pid = 0;
    pf >> pid;
    if (pid <= 0) {
        return Result<pid_t, std::string>::error(
            "invalid pid in " + pid_path);
    }
    return Result<pid_t, std::string>::ok(pid);
}

Result<std::string, std::string> PatchEngine::apply_unified_diff(
        const std::string& file_path, const std::string& diff) {
    // Write diff to a temp file
    std::string tmp_diff = "/tmp/straylight_hotpatch_" +
                           PatchRegistry::generate_id() + ".diff";
    {
        std::ofstream out(tmp_diff, std::ios::trunc);
        if (!out) {
            return Result<std::string, std::string>::error(
                "cannot write temp diff file");
        }
        out << diff;
    }

    // Apply using patch(1)
    std::string cmd = "patch --backup --posix \"" + file_path +
                      "\" < \"" + tmp_diff + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        unlink(tmp_diff.c_str());
        return Result<std::string, std::string>::error(
            "failed to execute patch(1)");
    }
    char buf[512];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    int ret = pclose(pipe);
    unlink(tmp_diff.c_str());

    if (ret != 0) {
        return Result<std::string, std::string>::error(
            "patch(1) failed: " + output);
    }

    return Result<std::string, std::string>::ok(output);
}

VoidResult<> PatchEngine::restore_file(
        const std::string& path, const std::string& original_content) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return VoidResult<>::error("cannot write to " + path);
    }
    out << original_content;
    return VoidResult<>::ok();
}

VoidResult<> PatchEngine::remove_kernel_patch(
        const std::string& module_name) {
    // Disable the livepatch via sysfs first
    std::string enabled_path = "/sys/kernel/livepatch/" +
                               module_name + "/enabled";
    {
        std::ofstream ef(enabled_path);
        if (ef) ef << "0";
    }

    // Wait for transition to complete
    std::string transition_path = "/sys/kernel/livepatch/" +
                                  module_name + "/transition";
    for (int i = 0; i < 100; ++i) {
        std::ifstream tf(transition_path);
        int t = 1;
        if (tf) tf >> t;
        if (t == 0) break;
        usleep(100000);
    }

    // Remove the module
    std::string cmd = "rmmod " + module_name + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return VoidResult<>::error("failed to execute rmmod");
    char buf[256];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    int ret = pclose(pipe);

    if (ret != 0) {
        return VoidResult<>::error("rmmod failed: " + output);
    }

    return VoidResult<>::ok();
}

} // namespace straylight::hotpatch
