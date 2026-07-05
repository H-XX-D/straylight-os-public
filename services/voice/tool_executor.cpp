// services/voice/tool_executor.cpp
// StrayLight tool registry and execution engine.

#include "tool_executor.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace straylight::voice {

// ─── JSON helpers ───────────────────────────────────────────────────────────

std::string ToolExecutor::json_str(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return "";
    ++pos;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
    }
    return result;
}

int ToolExecutor::json_int(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return 0;
    try { return std::stoi(json.substr(pos)); } catch (...) { return 0; }
}

float ToolExecutor::json_float(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0f;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0.0f;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return 0.0f;
    try { return std::stof(json.substr(pos)); } catch (...) { return 0.0f; }
}

// ─── Initialization ─────────────────────────────────────────────────────────

Result<void, std::string> ToolExecutor::init(const VoiceConfig& cfg) {
    config_ = cfg;
    register_tools();
    fprintf(stdout, "[voice:tools] registered %zu tools\n", tools_.size());
    return Result<void, std::string>::ok();
}

// ─── Tool registry ──────────────────────────────────────────────────────────

void ToolExecutor::register_tools() {
    tools_.clear();

    // run_command — Execute a shell command and return stdout.
    tools_.push_back({
        "run_command",
        "Execute a shell command on the system and return its stdout output.",
        {{"cmd", "string", "The shell command to execute", true}},
        true // dangerous
    });

    // snapshot_create — Create a filesystem snapshot.
    tools_.push_back({
        "snapshot_create",
        "Create a filesystem snapshot of the current system state.",
        {{"name", "string", "Optional snapshot name", false}},
        false
    });

    // health_check — Get system health score.
    tools_.push_back({
        "health_check",
        "Run a system health check and return the overall health score and component status.",
        {},
        false
    });

    // bench_run — Run a benchmark.
    tools_.push_back({
        "bench_run",
        "Run a system benchmark in the specified category (cpu, memory, disk, gpu, network).",
        {{"category", "string", "Benchmark category: cpu, memory, disk, gpu, network", true}},
        false
    });

    // vault_get — Get a secret.
    tools_.push_back({
        "vault_get",
        "Retrieve a secret value from the StrayLight vault by key name.",
        {{"key", "string", "The secret key to retrieve", true}},
        false
    });

    // probe_scan — Network scan.
    tools_.push_back({
        "probe_scan",
        "Scan a network target (IP, hostname, or CIDR range) for open ports and services.",
        {{"target", "string", "Target IP, hostname, or CIDR range", true}},
        false
    });

    // shield_audit — Security audit.
    tools_.push_back({
        "shield_audit",
        "Run a security audit on the system and return findings with severity levels.",
        {},
        false
    });

    // sandbox_create — Create a sandbox.
    tools_.push_back({
        "sandbox_create",
        "Create an isolated sandbox environment with the given name.",
        {{"name", "string", "Name for the sandbox", true}},
        false
    });

    // intent_resolve — Resolve natural language to action.
    tools_.push_back({
        "intent_resolve",
        "Resolve a natural language instruction into structured system commands.",
        {{"text", "string", "The natural language instruction", true}},
        false
    });

    // service_control — Start/stop/restart a service.
    tools_.push_back({
        "service_control",
        "Control a StrayLight system service (start, stop, restart, status).",
        {{"name", "string", "Service name (e.g., straylight-health)", true},
         {"action", "string", "Action: start, stop, restart, status", true}},
        true // dangerous for stop/restart
    });

    // display_set — Change display settings.
    tools_.push_back({
        "display_set",
        "Change display output resolution or configuration.",
        {{"output", "string", "Display output name (e.g., HDMI-1, eDP-1)", true},
         {"resolution", "string", "Resolution (e.g., 1920x1080, 2560x1440)", true}},
        false
    });

    // network_connect — Connect to WiFi.
    tools_.push_back({
        "network_connect",
        "Connect to a WiFi network by SSID.",
        {{"ssid", "string", "WiFi network name (SSID)", true},
         {"password", "string", "WiFi password (optional for open networks)", false}},
        false
    });

    // update_check — Check for updates.
    tools_.push_back({
        "update_check",
        "Check for available system updates and list them.",
        {},
        false
    });

    // thermal_status — Get thermal state.
    tools_.push_back({
        "thermal_status",
        "Get current thermal sensor readings and throttling status.",
        {},
        false
    });

    // fabric_path — Query device topology.
    tools_.push_back({
        "fabric_path",
        "Query the mesh fabric for the path between two devices.",
        {{"from", "string", "Source device name or ID", true},
         {"to", "string", "Destination device name or ID", true}},
        false
    });

    // quota_set — Set resource quota.
    tools_.push_back({
        "quota_set",
        "Set resource usage limits (CPU, memory, disk) for an application.",
        {{"app", "string", "Application name or PID", true},
         {"cpu_percent", "int", "Max CPU usage percentage (0-100)", false},
         {"memory_mb", "int", "Max memory in megabytes", false}},
        false
    });

    // volume_set — Set audio volume.
    tools_.push_back({
        "volume_set",
        "Set the audio volume level for a stream or the master output.",
        {{"stream", "string", "Stream name: master, media, notification, call", false},
         {"level", "int", "Volume level 0-100", true}},
        false
    });

    // brightness_set — Set display brightness.
    tools_.push_back({
        "brightness_set",
        "Set the display brightness level.",
        {{"level", "int", "Brightness level 0-100", true}},
        false
    });
}

const ToolDefinition* ToolExecutor::find_tool(const std::string& name) const {
    for (const auto& t : tools_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

bool ToolExecutor::requires_confirmation(
    const std::string& tool_name, const std::string& args_json) const
{
    const auto* tool = find_tool(tool_name);
    if (!tool) return false;

    // Explicit dangerous flag.
    if (tool->dangerous) {
        // For run_command, check if the command matches safety keywords.
        if (tool_name == "run_command") {
            std::string cmd = json_str(args_json, "cmd");
            return config_.needs_confirmation(cmd);
        }
        // For service_control, check if action is stop or restart.
        if (tool_name == "service_control") {
            std::string action = json_str(args_json, "action");
            return (action == "stop" || action == "restart");
        }
        return true;
    }
    return false;
}

// ─── Tool prompt for LLM ───────────────────────────────────────────────────

std::string ToolExecutor::tools_prompt() const {
    std::ostringstream out;
    out << "You have access to the following tools. To use a tool, respond with "
           "a JSON object in this format:\n"
           "{\"tool\": \"tool_name\", \"args\": {\"param1\": \"value1\"}}\n\n"
           "Available tools:\n\n";

    for (const auto& t : tools_) {
        out << "- " << t.name << ": " << t.description << "\n";
        if (!t.parameters.empty()) {
            out << "  Parameters:\n";
            for (const auto& p : t.parameters) {
                out << "    - " << p.name << " (" << p.type;
                if (p.required) out << ", required";
                out << "): " << p.description << "\n";
            }
        }
        out << "\n";
    }

    out << "If no tool is needed, just respond with plain text.\n"
           "If you need to use a tool, output ONLY the JSON object, nothing else.\n"
           "After receiving tool results, provide a natural language summary.\n";

    return out.str();
}

// ─── Execution ──────────────────────────────────────────────────────────────

Result<ToolResult, std::string> ToolExecutor::execute(
    const std::string& tool_name, const std::string& args_json)
{
    const auto* tool = find_tool(tool_name);
    if (!tool) {
        return Result<ToolResult, std::string>::error(
            "unknown tool: " + tool_name);
    }

    // Safety check: blocked commands.
    if (tool_name == "run_command") {
        std::string cmd = json_str(args_json, "cmd");
        if (config_.is_blocked(cmd)) {
            ToolResult r;
            r.success = false;
            r.error = "command blocked by safety policy: " + cmd;
            return Result<ToolResult, std::string>::ok(r);
        }
    }

    // Confirmation check.
    if (!auto_confirm_ && requires_confirmation(tool_name, args_json)) {
        if (confirm_cb_) {
            std::string desc = "Tool '" + tool_name + "' with args: " + args_json;
            if (!confirm_cb_(desc)) {
                ToolResult r;
                r.success = false;
                r.error = "user declined confirmation";
                return Result<ToolResult, std::string>::ok(r);
            }
        }
    }

    // Route to the appropriate executor.
    if (tool_name == "run_command") {
        std::string cmd = json_str(args_json, "cmd");
        return Result<ToolResult, std::string>::ok(run_command(cmd));
    }

    if (tool_name == "snapshot_create") {
        std::string name = json_str(args_json, "name");
        std::string cmd = "straylight-snapshot create";
        if (!name.empty()) cmd += " --name " + name;
        return Result<ToolResult, std::string>::ok(run_command(cmd));
    }

    if (tool_name == "health_check") {
        return Result<ToolResult, std::string>::ok(
            ipc_call("/run/straylight/health.sock", "health.report", "{}"));
    }

    if (tool_name == "bench_run") {
        std::string cat = json_str(args_json, "category");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-bench run " + cat));
    }

    if (tool_name == "vault_get") {
        std::string key = json_str(args_json, "key");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-vault get " + key));
    }

    if (tool_name == "probe_scan") {
        std::string target = json_str(args_json, "target");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-probe scan " + target));
    }

    if (tool_name == "shield_audit") {
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-shield audit"));
    }

    if (tool_name == "sandbox_create") {
        std::string name = json_str(args_json, "name");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-sandbox create " + name));
    }

    if (tool_name == "intent_resolve") {
        std::string text = json_str(args_json, "text");
        // Escape for JSON-RPC.
        std::string escaped;
        for (char c : text) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }
        std::string params = "{\"text\": \"" + escaped + "\"}";
        return Result<ToolResult, std::string>::ok(
            ipc_call("/run/straylight/intent.sock", "intent.resolve", params));
    }

    if (tool_name == "service_control") {
        std::string name = json_str(args_json, "name");
        std::string action = json_str(args_json, "action");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-service " + action + " " + name));
    }

    if (tool_name == "display_set") {
        std::string output = json_str(args_json, "output");
        std::string res = json_str(args_json, "resolution");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-display set " + output + " " + res));
    }

    if (tool_name == "network_connect") {
        std::string ssid = json_str(args_json, "ssid");
        std::string pw = json_str(args_json, "password");
        std::string cmd = "straylight-wifi connect " + ssid;
        if (!pw.empty()) cmd += " --password " + pw;
        return Result<ToolResult, std::string>::ok(run_command(cmd));
    }

    if (tool_name == "update_check") {
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-update check"));
    }

    if (tool_name == "thermal_status") {
        return Result<ToolResult, std::string>::ok(
            ipc_call("/run/straylight/thermal.sock", "thermal.status", "{}"));
    }

    if (tool_name == "fabric_path") {
        std::string from = json_str(args_json, "from");
        std::string to = json_str(args_json, "to");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-fabric path " + from + " " + to));
    }

    if (tool_name == "quota_set") {
        std::string app = json_str(args_json, "app");
        int cpu = json_int(args_json, "cpu_percent");
        int mem = json_int(args_json, "memory_mb");
        std::string cmd = "straylight-quota set " + app;
        if (cpu > 0) cmd += " --cpu " + std::to_string(cpu);
        if (mem > 0) cmd += " --memory " + std::to_string(mem);
        return Result<ToolResult, std::string>::ok(run_command(cmd));
    }

    if (tool_name == "volume_set") {
        std::string stream = json_str(args_json, "stream");
        int level = json_int(args_json, "level");
        if (stream.empty()) stream = "master";
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-audio volume " + stream + " " +
                        std::to_string(level)));
    }

    if (tool_name == "brightness_set") {
        int level = json_int(args_json, "level");
        return Result<ToolResult, std::string>::ok(
            run_command("straylight-display brightness " + std::to_string(level)));
    }

    return Result<ToolResult, std::string>::error("unhandled tool: " + tool_name);
}

// ─── Shell command executor ─────────────────────────────────────────────────

ToolResult ToolExecutor::run_command(const std::string& cmd) {
    ToolResult result;
    auto t0 = std::chrono::steady_clock::now();

    // Redirect stderr to a temp file so we can capture it separately.
    std::string err_file = "/tmp/straylight-tool-err-" + std::to_string(getpid());
    std::string full_cmd = cmd + " 2>" + err_file;

    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        result.success = false;
        result.error = "popen failed: " + std::string(strerror(errno));
        return result;
    }

    // Read stdout.
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        result.output += buf.data();
    }

    int status = pclose(pipe);
    result.exit_code = WEXITSTATUS(status);
    result.success = (result.exit_code == 0);

    // Read stderr.
    FILE* err_fp = fopen(err_file.c_str(), "r");
    if (err_fp) {
        while (fgets(buf.data(), buf.size(), err_fp) != nullptr) {
            result.error += buf.data();
        }
        fclose(err_fp);
    }
    std::remove(err_file.c_str());

    auto t1 = std::chrono::steady_clock::now();
    result.duration_s = std::chrono::duration<double>(t1 - t0).count();

    // Trim trailing whitespace.
    auto trim = [](std::string& s) {
        auto end = s.find_last_not_of(" \t\n\r");
        if (end != std::string::npos) s = s.substr(0, end + 1);
    };
    trim(result.output);
    trim(result.error);

    return result;
}

// ─── IPC call to StrayLight daemon ──────────────────────────────────────────

ToolResult ToolExecutor::ipc_call(
    const std::string& socket_path,
    const std::string& method,
    const std::string& params_json)
{
    ToolResult result;
    auto t0 = std::chrono::steady_clock::now();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        result.success = false;
        result.error = "socket() failed: " + std::string(strerror(errno));
        return result;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        // Fall back to CLI tool.
        std::string tool_name = method.substr(0, method.find('.'));
        std::string action = method.substr(method.find('.') + 1);
        result = run_command("straylight-" + tool_name + " " + action);
        return result;
    }

    // Build JSON-RPC request.
    std::string request = "{\"jsonrpc\": \"2.0\", \"method\": \"" + method +
                          "\", \"params\": " + params_json + ", \"id\": 1}\n";

    ssize_t written = write(fd, request.c_str(), request.size());
    if (written < 0) {
        close(fd);
        result.success = false;
        result.error = "write failed: " + std::string(strerror(errno));
        return result;
    }

    // Read response (with timeout).
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        result.success = false;
        result.error = "read failed or timeout";
        return result;
    }
    buf[n] = '\0';

    result.output = std::string(buf, n);
    result.success = true;

    // Check for JSON-RPC error.
    if (result.output.find("\"error\"") != std::string::npos &&
        result.output.find("\"result\"") == std::string::npos) {
        result.success = false;
        result.error = result.output;
    }

    auto t1 = std::chrono::steady_clock::now();
    result.duration_s = std::chrono::duration<double>(t1 - t0).count();

    return result;
}

} // namespace straylight::voice
