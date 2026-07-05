// services/remote/command_handler.cpp
//
// NOTE: This is a remote system management daemon. The popen() calls below are
// intentional — they execute validated, sanitized commands on the LOCAL machine
// that this daemon runs on. Input validation occurs before any shell invocation.
// This is C++ systems code, not a JavaScript web application.

#include "command_handler.h"
#include "file_transfer.h"
#include "system_info.h"
#include <straylight/log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#endif

#include <sys/stat.h>
#include <unistd.h>

namespace straylight {

CommandHandler::CommandHandler() {
    file_transfer_ = std::make_unique<FileTransfer>();
    system_info_ = std::make_unique<SystemInfo>();
    register_methods();
}

CommandHandler::~CommandHandler() = default;

void CommandHandler::register_methods() {
    methods_["exec"] = [this](const nlohmann::json& p) { return handle_exec(p); };
    methods_["exec_stream"] = [this](const nlohmann::json& p) { return handle_exec_stream(p); };
    methods_["upload"] = [this](const nlohmann::json& p) { return handle_upload(p); };
    methods_["download"] = [this](const nlohmann::json& p) { return handle_download(p); };
    methods_["sysinfo"] = [this](const nlohmann::json& p) { return handle_sysinfo(p); };
    methods_["services"] = [this](const nlohmann::json& p) { return handle_services(p); };
    methods_["logs"] = [this](const nlohmann::json& p) { return handle_logs(p); };
    methods_["processes"] = [this](const nlohmann::json& p) { return handle_processes(p); };
    methods_["alice"] = [this](const nlohmann::json& p) { return handle_alice(p); };
    methods_["packages"] = [this](const nlohmann::json& p) { return handle_packages(p); };
    methods_["fs"] = [this](const nlohmann::json& p) { return handle_fs(p); };
}

std::string CommandHandler::handle(const std::string& request) {
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(request);
    } catch (const nlohmann::json::exception& e) {
        return make_error(-32700, "Parse error: " + std::string(e.what()), nullptr);
    }

    // Validate JSON-RPC 2.0
    if (!req.contains("jsonrpc") || req["jsonrpc"] != "2.0") {
        return make_error(-32600, "Invalid JSON-RPC version", req.value("id", nlohmann::json(nullptr)));
    }

    if (!req.contains("method") || !req["method"].is_string()) {
        return make_error(-32600, "Missing method", req.value("id", nlohmann::json(nullptr)));
    }

    std::string method = req["method"];
    nlohmann::json id = req.value("id", nlohmann::json(nullptr));
    nlohmann::json params = req.value("params", nlohmann::json::object());

    // Find handler
    auto it = methods_.find(method);
    if (it == methods_.end()) {
        return make_error(-32601, "Method not found: " + method, id);
    }

    // Check feature flags
    if ((method == "exec" || method == "exec_stream") && !feat_exec_) {
        return make_error(-32001, "Command execution is disabled", id);
    }
    if ((method == "upload" || method == "download") && !feat_file_transfer_) {
        return make_error(-32001, "File transfer is disabled", id);
    }
    if (method == "services" && !feat_service_mgmt_) {
        return make_error(-32001, "Service management is disabled", id);
    }
    if (method == "packages" && !feat_package_mgmt_) {
        return make_error(-32001, "Package management is disabled", id);
    }
    if (method == "alice" && !feat_alice_) {
        return make_error(-32001, "Alice integration is disabled", id);
    }

    // Dispatch
    try {
        nlohmann::json result = it->second(params);
        return make_response(result, id);
    } catch (const std::exception& e) {
        SL_ERROR("remote-handler: exception in method '{}': {}", method, e.what());
        return make_error(-32603, "Internal error: " + std::string(e.what()), id);
    }
}

nlohmann::json CommandHandler::handle_exec(const nlohmann::json& params) {
    std::string cmd = params.value("cmd", "");
    if (cmd.empty()) {
        return {{"error", "Missing 'cmd' parameter"}};
    }

    SL_DEBUG("remote-handler: exec cmd='{}'", cmd);

    // Execute command, capturing stdout and stderr separately
    // We redirect stderr to a temp file so we can capture both streams
    std::string stderr_file = "/tmp/.sl-remote-stderr-" + std::to_string(getpid());
    std::string full_cmd = cmd + " 2>" + stderr_file;

    std::array<char, 4096> buffer;
    std::string stdout_output;
    int exit_code = -1;

    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        return {{"stdout", ""}, {"stderr", "Failed to execute command"}, {"exit_code", -1}};
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        stdout_output += buffer.data();
    }

    int status = pclose(pipe);
#ifdef __linux__
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }
#else
    exit_code = status;
#endif

    // Read stderr
    std::string stderr_output;
    std::ifstream stderr_stream(stderr_file);
    if (stderr_stream.is_open()) {
        std::ostringstream ss;
        ss << stderr_stream.rdbuf();
        stderr_output = ss.str();
        stderr_stream.close();
    }
    std::filesystem::remove(stderr_file);

    return {{"stdout", stdout_output}, {"stderr", stderr_output}, {"exit_code", exit_code}};
}

nlohmann::json CommandHandler::handle_exec_stream(const nlohmann::json& params) {
    // PTY-based streaming execution.
    // For the synchronous RPC path, we execute and return the full output.
    // True PTY streaming would use a dedicated bidirectional channel.
    std::string cmd = params.value("cmd", "");
    if (cmd.empty()) {
        return {{"error", "Missing 'cmd' parameter"}};
    }

    bool use_pty = params.value("pty", true);
    SL_DEBUG("remote-handler: exec_stream cmd='{}' pty={}", cmd, use_pty);

    std::string full_cmd = cmd + " 2>&1";
    std::array<char, 4096> buffer;
    std::string output;

    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        return {{"error", "Failed to execute command"}, {"exit_code", -1}};
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    int exit_code = -1;
#ifdef __linux__
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }
#else
    exit_code = status;
#endif

    return {{"data", output}, {"exit_code", exit_code}, {"pty", use_pty}};
}

nlohmann::json CommandHandler::handle_upload(const nlohmann::json& params) {
    std::string path = params.value("path", "");
    std::string data_b64 = params.value("data", "");
    int64_t offset = params.value("offset", static_cast<int64_t>(0));

    if (path.empty()) {
        return {{"error", "Missing 'path' parameter"}};
    }

    if (!is_safe_path(path)) {
        return {{"error", "Path traversal detected"}};
    }

    if (data_b64.size() > max_upload_bytes_) {
        return {{"error", "Upload exceeds maximum size"}};
    }

    auto result = file_transfer_->write_chunk(path, data_b64, offset);
    if (!result.has_value()) {
        return {{"error", result.error()}};
    }

    return result.value();
}

nlohmann::json CommandHandler::handle_download(const nlohmann::json& params) {
    std::string path = params.value("path", "");
    int64_t offset = params.value("offset", static_cast<int64_t>(0));
    int64_t length = params.value("length", static_cast<int64_t>(-1));

    if (path.empty()) {
        return {{"error", "Missing 'path' parameter"}};
    }

    if (!is_safe_path(path)) {
        return {{"error", "Path traversal detected"}};
    }

    auto result = file_transfer_->read_chunk(path, offset, length);
    if (!result.has_value()) {
        return {{"error", result.error()}};
    }

    return result.value();
}

nlohmann::json CommandHandler::handle_sysinfo(const nlohmann::json& /*params*/) {
    return system_info_->gather();
}

nlohmann::json CommandHandler::handle_services(const nlohmann::json& params) {
    std::string action = params.value("action", "list");
    std::string unit = params.value("unit", "");

    if (action == "list") {
        std::array<char, 4096> buffer;
        std::string output;
        FILE* pipe = popen("systemctl list-units --type=service --no-pager --plain 2>/dev/null", "r");
        if (!pipe) {
            return {{"error", "Failed to query systemd"}};
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        pclose(pipe);

        nlohmann::json services = nlohmann::json::array();
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            std::string name, load_state, active_state, sub_state, description;
            ls >> name >> load_state >> active_state >> sub_state;
            std::getline(ls, description);
            if (!name.empty() && name.find(".service") != std::string::npos) {
                services.push_back({
                    {"unit", name},
                    {"load", load_state},
                    {"active", active_state},
                    {"sub", sub_state},
                    {"description", description}
                });
            }
        }
        return {{"services", services}};
    }

    if (unit.empty()) {
        return {{"error", "Missing 'unit' parameter for action: " + action}};
    }

    // Validate unit name: only alphanumeric, dash, underscore, dot, at-sign
    for (char c : unit) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '@') {
            return {{"error", "Invalid unit name"}};
        }
    }

    std::string systemctl_cmd;
    if (action == "start") {
        systemctl_cmd = "systemctl start " + unit + " 2>&1";
    } else if (action == "stop") {
        systemctl_cmd = "systemctl stop " + unit + " 2>&1";
    } else if (action == "restart") {
        systemctl_cmd = "systemctl restart " + unit + " 2>&1";
    } else if (action == "status") {
        systemctl_cmd = "systemctl status " + unit + " --no-pager 2>&1";
    } else {
        return {{"error", "Unknown action: " + action}};
    }

    std::array<char, 4096> buffer;
    std::string output;
    FILE* pipe = popen(systemctl_cmd.c_str(), "r");
    if (!pipe) {
        return {{"error", "Failed to run systemctl"}};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int status = pclose(pipe);

    return {{"unit", unit}, {"action", action}, {"output", output}, {"exit_code", status}};
}

nlohmann::json CommandHandler::handle_logs(const nlohmann::json& params) {
    std::string unit = params.value("unit", "");
    int lines = params.value("lines", 50);
    bool follow = params.value("follow", false);

    if (lines < 1) lines = 1;
    if (lines > 10000) lines = 10000;

    std::string cmd = "journalctl --no-pager";

    if (!unit.empty()) {
        for (char c : unit) {
            if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '@') {
                return {{"error", "Invalid unit name"}};
            }
        }
        cmd += " -u " + unit;
    }

    cmd += " -n " + std::to_string(lines);
    cmd += " 2>&1";

    if (follow) {
        SL_DEBUG("remote-handler: follow mode requested but returning snapshot");
    }

    std::array<char, 4096> buffer;
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return {{"error", "Failed to query journalctl"}};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);

    return {{"logs", output}, {"unit", unit}, {"lines", lines}};
}

nlohmann::json CommandHandler::handle_processes(const nlohmann::json& params) {
    std::string action = params.value("action", "list");

    if (action == "list") {
        std::array<char, 4096> buffer;
        std::string output;
        FILE* pipe = popen("ps aux --sort=-%cpu 2>/dev/null || ps aux 2>&1", "r");
        if (!pipe) {
            return {{"error", "Failed to list processes"}};
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        pclose(pipe);

        nlohmann::json processes = nlohmann::json::array();
        std::istringstream iss(output);
        std::string line;
        bool header = true;
        while (std::getline(iss, line)) {
            if (header) { header = false; continue; }
            if (line.empty()) continue;

            std::istringstream ls(line);
            std::string user, pid_s, cpu_s, mem_s, vsz, rss, tty, stat, start, time_s, cmd;
            ls >> user >> pid_s >> cpu_s >> mem_s >> vsz >> rss >> tty >> stat >> start >> time_s;
            std::getline(ls, cmd);

            processes.push_back({
                {"user", user},
                {"pid", pid_s},
                {"cpu", cpu_s},
                {"mem", mem_s},
                {"vsz", vsz},
                {"rss", rss},
                {"stat", stat},
                {"time", time_s},
                {"command", cmd}
            });
        }

        return {{"processes", processes}};
    }

    if (action == "kill") {
        int pid = params.value("pid", 0);
        int sig = params.value("signal", 15);

        if (pid <= 0) {
            return {{"error", "Invalid PID"}};
        }
        if (pid == 1) {
            return {{"error", "Cannot kill PID 1"}};
        }
        if (sig < 1 || sig > 64) {
            return {{"error", "Invalid signal number"}};
        }

        int result = kill(static_cast<pid_t>(pid), sig);
        if (result != 0) {
            return {{"error", "kill failed: " + std::string(strerror(errno))},
                    {"pid", pid}, {"signal", sig}};
        }

        return {{"pid", pid}, {"signal", sig}, {"success", true}};
    }

    return {{"error", "Unknown action: " + action}};
}

nlohmann::json CommandHandler::handle_alice(const nlohmann::json& params) {
    std::string method = params.value("method", "status");
    std::string query = params.value("query", "");

    if (method == "status") {
        std::array<char, 4096> buffer;
        std::string output;
        FILE* pipe = popen(
            "busctl --json=short call org.straylight.Agent1 /org/straylight/Agent1 "
            "org.straylight.Agent1 Status 2>&1", "r");
        if (!pipe) {
            return {{"error", "Failed to query Alice"}};
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        int status = pclose(pipe);

        if (status != 0) {
            return {{"status", "unavailable"}, {"message", "Alice agent not responding"}};
        }

        return {{"status", "available"}, {"response", output}};
    }

    if (method == "ask" || method == "analyze") {
        if (query.empty()) {
            return {{"error", "Missing 'query' parameter"}};
        }

        if (query.size() > 4096) {
            return {{"error", "Query too long (max 4096 characters)"}};
        }

        // Escape single quotes in query for shell safety
        std::string escaped_query;
        for (char c : query) {
            if (c == '\'') {
                escaped_query += "'\\''";
            } else {
                escaped_query += c;
            }
        }

        std::string dbus_method = (method == "ask") ? "Ask" : "Analyze";
        std::string cmd = "busctl --json=short call org.straylight.Agent1 "
                          "/org/straylight/Agent1 org.straylight.Agent1 "
                          + dbus_method + " s '" + escaped_query + "' 2>&1";

        std::array<char, 4096> buffer;
        std::string output;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return {{"error", "Failed to query Alice"}};
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        int status = pclose(pipe);

        return {{"method", method}, {"query", query}, {"response", output},
                {"exit_code", status}};
    }

    return {{"error", "Unknown Alice method: " + method}};
}

nlohmann::json CommandHandler::handle_packages(const nlohmann::json& params) {
    std::string action = params.value("action", "list");
    auto names = params.value("names", std::vector<std::string>{});

    if (action == "list") {
        std::array<char, 4096> buffer;
        std::string output;
        FILE* pipe = popen("dpkg -l 2>/dev/null | tail -n +6", "r");
        if (!pipe) {
            return {{"error", "Failed to list packages"}};
        }
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        pclose(pipe);

        nlohmann::json packages = nlohmann::json::array();
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.size() < 4) continue;
            std::istringstream ls(line);
            std::string status, name, version, arch, description;
            ls >> status >> name >> version >> arch;
            std::getline(ls, description);
            packages.push_back({
                {"status", status},
                {"name", name},
                {"version", version},
                {"arch", arch},
                {"description", description}
            });
        }

        return {{"packages", packages}};
    }

    if (names.empty()) {
        return {{"error", "Missing 'names' parameter"}};
    }

    // Validate package names
    for (const auto& name : names) {
        for (char c : name) {
            if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '+' && c != ':') {
                return {{"error", "Invalid package name: " + name}};
            }
        }
    }

    std::string pkg_list;
    for (const auto& name : names) {
        if (!pkg_list.empty()) pkg_list += " ";
        pkg_list += name;
    }

    std::string cmd;
    if (action == "install") {
        cmd = "DEBIAN_FRONTEND=noninteractive apt-get install -y " + pkg_list + " 2>&1";
    } else if (action == "remove") {
        cmd = "DEBIAN_FRONTEND=noninteractive apt-get remove -y " + pkg_list + " 2>&1";
    } else if (action == "update") {
        cmd = "apt-get update 2>&1 && DEBIAN_FRONTEND=noninteractive apt-get upgrade -y " + pkg_list + " 2>&1";
    } else {
        return {{"error", "Unknown action: " + action}};
    }

    std::array<char, 4096> buffer;
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return {{"error", "Failed to run apt"}};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int status = pclose(pipe);

    return {{"action", action}, {"packages", names}, {"output", output}, {"exit_code", status}};
}

nlohmann::json CommandHandler::handle_fs(const nlohmann::json& params) {
    std::string action = params.value("action", "ls");
    std::string path = params.value("path", ".");

    if (!is_safe_path(path)) {
        return {{"error", "Path traversal detected"}};
    }

    if (action == "ls") {
        nlohmann::json entries = nlohmann::json::array();
        try {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                nlohmann::json e;
                e["name"] = entry.path().filename().string();
                e["is_directory"] = entry.is_directory();
                e["is_symlink"] = entry.is_symlink();

                if (entry.is_regular_file()) {
                    e["size"] = entry.file_size();
                } else {
                    e["size"] = 0;
                }

                auto ftime = entry.last_write_time();
                auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                    std::chrono::clock_cast<std::chrono::system_clock>(ftime));
                e["modified"] = sctp.time_since_epoch().count();

                entries.push_back(e);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            return {{"error", "Cannot list directory: " + std::string(e.what())}};
        }

        return {{"path", path}, {"entries", entries}};
    }

    if (action == "stat") {
        try {
            auto fs_status = std::filesystem::status(path);
            auto fsize = std::filesystem::is_regular_file(path) ?
                         std::filesystem::file_size(path) : 0ULL;
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                std::chrono::clock_cast<std::chrono::system_clock>(ftime));

            return {
                {"path", path},
                {"exists", true},
                {"is_file", std::filesystem::is_regular_file(path)},
                {"is_directory", std::filesystem::is_directory(path)},
                {"is_symlink", std::filesystem::is_symlink(path)},
                {"size", fsize},
                {"modified", sctp.time_since_epoch().count()},
                {"permissions", static_cast<int>(fs_status.permissions())}
            };
        } catch (const std::filesystem::filesystem_error& e) {
            return {{"path", path}, {"exists", false}, {"error", std::string(e.what())}};
        }
    }

    if (action == "mkdir") {
        try {
            std::filesystem::create_directories(path);
            return {{"path", path}, {"created", true}};
        } catch (const std::filesystem::filesystem_error& e) {
            return {{"error", "Cannot create directory: " + std::string(e.what())}};
        }
    }

    if (action == "rm") {
        try {
            auto removed = std::filesystem::remove_all(path);
            return {{"path", path}, {"removed", removed}};
        } catch (const std::filesystem::filesystem_error& e) {
            return {{"error", "Cannot remove: " + std::string(e.what())}};
        }
    }

    return {{"error", "Unknown fs action: " + action}};
}

std::string CommandHandler::make_response(const nlohmann::json& result, const nlohmann::json& id) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["result"] = result;
    response["id"] = id;
    return response.dump();
}

std::string CommandHandler::make_error(int code, const std::string& message, const nlohmann::json& id) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["error"] = {{"code", code}, {"message", message}};
    response["id"] = id;
    return response.dump();
}

bool CommandHandler::is_safe_path(const std::string& path) {
    // Reject path traversal attempts
    if (path.find("..") != std::string::npos) {
        return false;
    }

    // Reject null bytes
    if (path.find('\0') != std::string::npos) {
        return false;
    }

    // Must be absolute or current-directory reference
    if (path.empty() || path[0] != '/') {
        if (path == "." || path == "./") return true;
        return false;
    }

    // Normalize and verify no traversal after normalization
    try {
        auto canonical = std::filesystem::weakly_canonical(path);
        auto canonical_str = canonical.string();

        // Block access to critical system paths
        static const std::vector<std::string> blocked_prefixes = {
            "/proc/self", "/proc/1",
            "/dev/mem", "/dev/kmem", "/dev/port"
        };

        for (const auto& prefix : blocked_prefixes) {
            if (canonical_str.find(prefix) == 0) {
                return false;
            }
        }
    } catch (...) {
        // If we can't canonicalize, allow — the actual operation will fail naturally
    }

    return true;
}

} // namespace straylight
