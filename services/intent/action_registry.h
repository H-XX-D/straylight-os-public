/**
 * StrayLight Action Registry — Maps action names to system commands.
 *
 * Provides built-in actions for all StrayLight tools and supports
 * extension via JSON files in /etc/straylight/intent.d/.
 */
#pragma once

#include "intent_engine.h"
#include "straylight/result.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::intent {

struct ActionDef {
    std::string name;
    ActionType type = ActionType::system_command;
    std::vector<std::string> commands;
    std::string description;
    bool destructive = false;
    std::vector<std::string> aliases;
};

class ActionRegistry {
public:
    static ActionRegistry& instance() {
        static ActionRegistry reg;
        return reg;
    }

    ActionRegistry(const ActionRegistry&) = delete;
    ActionRegistry& operator=(const ActionRegistry&) = delete;

    /** Look up an action by name or alias. */
    Result<ActionDef, std::string> lookup(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = actions_.find(name);
        if (it != actions_.end()) {
            return Result<ActionDef, std::string>::ok(it->second);
        }
        // Check aliases
        auto ait = aliases_.find(name);
        if (ait != aliases_.end()) {
            auto it2 = actions_.find(ait->second);
            if (it2 != actions_.end()) {
                return Result<ActionDef, std::string>::ok(it2->second);
            }
        }
        return Result<ActionDef, std::string>::error("action not found: " + name);
    }

    /** Register a new action. */
    void register_action(const ActionDef& def) {
        std::lock_guard<std::mutex> lock(mtx_);
        actions_[def.name] = def;
        for (const auto& alias : def.aliases) {
            aliases_[alias] = def.name;
        }
    }

    /** Load extension actions from /etc/straylight/intent.d/ */
    void load_extensions(const std::string& dir = "/etc/straylight/intent.d/") {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;

        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".json") {
                load_extension_file(entry.path().string());
            }
        }
    }

    /** List all registered action names. */
    std::vector<std::string> list_actions() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::string> names;
        names.reserve(actions_.size());
        for (const auto& [name, _] : actions_) {
            names.push_back(name);
        }
        return names;
    }

    /** Reload built-in actions and extensions. */
    void reload() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            actions_.clear();
            aliases_.clear();
        }
        register_builtins();
        load_extensions();
    }

private:
    ActionRegistry() {
        register_builtins();
        load_extensions();
    }

    mutable std::mutex mtx_;
    std::map<std::string, ActionDef> actions_;
    std::map<std::string, std::string> aliases_;  // alias -> canonical name

    void register_builtins() {
        // Screen recording
        register_action({
            "record_screen",
            ActionType::compositor_action,
            {"wf-recorder -g \"$(slurp)\" -f /tmp/recording-$(date +%s).mp4"},
            "Record a region of the screen to an MP4 file",
            false,
            {"screen_record", "capture_screen_video"}
        });

        register_action({
            "record_screen_fullscreen",
            ActionType::compositor_action,
            {"wf-recorder -f /tmp/recording-$(date +%s).mp4"},
            "Record the full screen to an MP4 file",
            false,
            {"record_full_screen"}
        });

        register_action({
            "record_screen_4k",
            ActionType::compositor_action,
            {"wf-recorder -p preset=medium -f /tmp/recording-$(date +%s).mp4"},
            "Record the screen in high quality",
            false,
            {}
        });

        // Screenshot
        register_action({
            "screenshot",
            ActionType::compositor_action,
            {"grim /tmp/screenshot-$(date +%s).png"},
            "Take a full-screen screenshot",
            false,
            {"take_screenshot", "screen_capture", "capture_screen"}
        });

        register_action({
            "screenshot_region",
            ActionType::compositor_action,
            {"grim -g \"$(slurp)\" /tmp/screenshot-$(date +%s).png"},
            "Take a screenshot of a selected region",
            false,
            {"capture_region"}
        });

        // Network
        register_action({
            "wifi_off",
            ActionType::system_command,
            {"nmcli radio wifi off"},
            "Turn off Wi-Fi radio",
            false,
            {"turn_off_wifi", "disable_wifi"}
        });

        register_action({
            "wifi_on",
            ActionType::system_command,
            {"nmcli radio wifi on"},
            "Turn on Wi-Fi radio",
            false,
            {"turn_on_wifi", "enable_wifi"}
        });

        register_action({
            "wifi_status",
            ActionType::system_command,
            {"nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list"},
            "List available Wi-Fi networks",
            false,
            {"list_wifi", "scan_wifi"}
        });

        register_action({
            "network_status",
            ActionType::system_command,
            {"networkctl status --no-pager"},
            "Show network interface status",
            false,
            {"show_network", "check_network"}
        });

        // StrayLight tools
        register_action({
            "run_benchmark",
            ActionType::system_command,
            {"straylight-bench --full"},
            "Run the full StrayLight benchmark suite",
            false,
            {"benchmark", "bench"}
        });

        register_action({
            "check_health",
            ActionType::system_command,
            {"straylight-core doctor"},
            "Run system health diagnostics",
            false,
            {"system_health", "health_check", "doctor"}
        });

        register_action({
            "encrypt_file",
            ActionType::file_operation,
            {"straylight-enclave seal {0}"},
            "Encrypt a file using the StrayLight enclave",
            false,
            {"seal_file", "file_encrypt"}
        });

        register_action({
            "decrypt_file",
            ActionType::file_operation,
            {"straylight-enclave unseal {0}"},
            "Decrypt a file using the StrayLight enclave",
            false,
            {"unseal_file", "file_decrypt"}
        });

        register_action({
            "scan_network",
            ActionType::system_command,
            {"straylight-agent scan --json"},
            "Scan the local network for nodes and services",
            false,
            {"network_scan", "probe_network", "discover_network"}
        });

        register_action({
            "show_gpu_usage",
            ActionType::vpu_action,
            {"cat /sys/class/straylight-vpu/vpu0/utilization 2>/dev/null || echo 'VPU sysfs not available'"},
            "Show GPU/VPU utilization",
            false,
            {"gpu_usage", "vpu_usage", "gpu_status"}
        });

        register_action({
            "create_sandbox",
            ActionType::system_command,
            {"straylight-enclave sandbox --name {0}"},
            "Create an isolated sandbox environment",
            false,
            {"sandbox", "new_sandbox"}
        });

        register_action({
            "snapshot_system",
            ActionType::system_command,
            {"straylight-core pipeline full /etc/straylight/snapshot.conf localhost"},
            "Create a full system snapshot",
            true,
            {"system_snapshot", "create_snapshot", "take_snapshot"}
        });

        // Compositor actions
        register_action({
            "toggle_fullscreen",
            ActionType::compositor_action,
            {"swaymsg fullscreen toggle"},
            "Toggle fullscreen for the focused window",
            false,
            {"fullscreen"}
        });

        register_action({
            "close_window",
            ActionType::compositor_action,
            {"swaymsg kill"},
            "Close the focused window",
            false,
            {"kill_window"}
        });

        register_action({
            "move_workspace",
            ActionType::compositor_action,
            {"swaymsg move container to workspace {0}"},
            "Move focused window to another workspace",
            false,
            {}
        });

        // Service control
        register_action({
            "restart_desktop_session",
            ActionType::service_control,
            {"systemctl --user restart gnome-session-manager.target"},
            "Restart the GNOME desktop session manager",
            true,
            {"restart_session"}
        });

        register_action({
            "restart_alice",
            ActionType::service_control,
            {"systemctl --user restart straylight-alice"},
            "Restart the Alice AI monitor",
            true,
            {}
        });

        // System commands
        register_action({
            "system_update",
            ActionType::system_command,
            {"sudo apt update && sudo apt upgrade -y"},
            "Update system packages",
            true,
            {"update_system", "upgrade_system"}
        });

        register_action({
            "show_processes",
            ActionType::system_command,
            {"ps aux --sort=-%cpu | head -20"},
            "Show top 20 processes by CPU usage",
            false,
            {"top_processes", "list_processes"}
        });

        register_action({
            "disk_usage",
            ActionType::system_command,
            {"df -h --output=source,size,used,avail,pcent,target | grep -v tmpfs"},
            "Show disk usage for all mounted filesystems",
            false,
            {"show_disk", "check_disk", "storage_status"}
        });

        register_action({
            "memory_usage",
            ActionType::system_command,
            {"free -h"},
            "Show memory usage",
            false,
            {"show_memory", "check_memory", "ram_usage"}
        });

        register_action({
            "show_uptime",
            ActionType::system_command,
            {"uptime"},
            "Show system uptime and load average",
            false,
            {"uptime", "system_uptime"}
        });

        register_action({
            "show_temperature",
            ActionType::system_command,
            {"cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | awk '{printf \"%.1f C\\n\", $1/1000}'"},
            "Show CPU/system temperatures",
            false,
            {"cpu_temp", "temperature", "thermal_status"}
        });

        // File operations
        register_action({
            "find_large_files",
            ActionType::file_operation,
            {"find / -xdev -type f -size +100M -exec ls -lh {} + 2>/dev/null | sort -k5 -rh | head -20"},
            "Find the 20 largest files on the system",
            false,
            {"large_files", "biggest_files"}
        });

        register_action({
            "compress_file",
            ActionType::file_operation,
            {"zstd -T0 {0}"},
            "Compress a file using zstd",
            false,
            {"zip_file"}
        });

        // Pipeline actions
        register_action({
            "entropy_seal_transport",
            ActionType::pipeline,
            {"straylight-core pipeline full {0} {1}"},
            "Full entropy-seal-transport pipeline for secure file transfer",
            false,
            {"secure_transfer", "seal_and_send"}
        });
    }

    /** Parse a single JSON extension file and register its actions.
     *  Expected format:
     *  {
     *    "actions": [
     *      {
     *        "name": "my_action",
     *        "type": "system_command",
     *        "commands": ["cmd1", "cmd2"],
     *        "description": "...",
     *        "destructive": false,
     *        "aliases": ["alias1"]
     *      }
     *    ]
     *  }
     */
    void load_extension_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) return;

        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

        // Very basic JSON array-of-objects parser for the "actions" array.
        // Each action object is delimited by { ... }.
        auto actions_pos = content.find("\"actions\"");
        if (actions_pos == std::string::npos) return;

        auto arr_start = content.find('[', actions_pos);
        if (arr_start == std::string::npos) return;

        // Find each top-level object in the array
        size_t pos = arr_start + 1;
        while (pos < content.size()) {
            auto obj_start = content.find('{', pos);
            if (obj_start == std::string::npos) break;

            // Find matching close brace
            int depth = 0;
            size_t obj_end = obj_start;
            for (size_t i = obj_start; i < content.size(); ++i) {
                if (content[i] == '{') ++depth;
                if (content[i] == '}') {
                    --depth;
                    if (depth == 0) { obj_end = i; break; }
                }
            }

            if (obj_end <= obj_start) break;

            std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

            ActionDef def;
            def.name = extract_field(obj, "name");
            if (def.name.empty()) { pos = obj_end + 1; continue; }

            def.type = action_type_from_str(extract_field(obj, "type"));
            def.description = extract_field(obj, "description");
            def.destructive = (extract_field(obj, "destructive") == "true");
            def.commands = extract_string_array(obj, "commands");
            def.aliases = extract_string_array(obj, "aliases");

            register_action(def);
            pos = obj_end + 1;
        }
    }

    static std::string extract_field(const std::string& obj, const std::string& key) {
        std::string search = "\"" + key + "\"";
        auto pos = obj.find(search);
        if (pos == std::string::npos) return "";

        pos = obj.find(':', pos + search.size());
        if (pos == std::string::npos) return "";

        pos = obj.find_first_not_of(" \t\n\r", pos + 1);
        if (pos == std::string::npos) return "";

        if (obj[pos] == '"') {
            auto end = obj.find('"', pos + 1);
            if (end == std::string::npos) return "";
            return obj.substr(pos + 1, end - pos - 1);
        }

        // Non-string value (boolean, number)
        auto end = obj.find_first_of(",}\n", pos);
        if (end == std::string::npos) end = obj.size();
        std::string val = obj.substr(pos, end - pos);
        // Trim whitespace
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        return val;
    }

    static std::vector<std::string> extract_string_array(const std::string& obj,
                                                         const std::string& key) {
        std::vector<std::string> result;
        std::string search = "\"" + key + "\"";
        auto pos = obj.find(search);
        if (pos == std::string::npos) return result;

        auto arr_start = obj.find('[', pos);
        if (arr_start == std::string::npos) return result;

        auto arr_end = obj.find(']', arr_start);
        if (arr_end == std::string::npos) return result;

        std::string arr = obj.substr(arr_start + 1, arr_end - arr_start - 1);
        size_t p = 0;
        while (p < arr.size()) {
            auto q1 = arr.find('"', p);
            if (q1 == std::string::npos) break;
            auto q2 = arr.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
            p = q2 + 1;
        }
        return result;
    }
};

} // namespace straylight::intent
