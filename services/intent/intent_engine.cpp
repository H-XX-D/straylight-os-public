/**
 * StrayLight Intent Engine — Implementation.
 *
 * Resolution flow:
 *   1. Try Alice AI via /run/straylight/alice.sock JSON-RPC
 *   2. If Alice unavailable, fall back to PatternMatcher
 *   3. Return structured IntentResult with commands, confidence, etc.
 */

#include "intent_engine.h"
#include "pattern_matcher.h"
#include "action_registry.h"

#include <cstring>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace straylight::intent {

// ─── Minimal JSON helpers (no external deps) ────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";

    if (json[pos] == '"') {
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                switch (json[pos]) {
                    case 'n':  result += '\n'; break;
                    case 't':  result += '\t'; break;
                    case '"':  result += '"';  break;
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
    return "";
}

static double extract_json_number(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0.0;

    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return 0.0;

    try {
        return std::stod(json.substr(pos));
    } catch (...) {
        return 0.0;
    }
}

static bool extract_json_bool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return false;

    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return false;

    return json.substr(pos, 4) == "true";
}

static std::vector<std::string> extract_json_string_array(const std::string& json,
                                                          const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;

    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;

    auto end = json.find(']', pos);
    if (end == std::string::npos) return result;

    std::string arr = json.substr(pos + 1, end - pos - 1);
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

// ─── IntentEngine ───────────────────────────────────────────────────────────

IntentEngine::IntentEngine() = default;
IntentEngine::~IntentEngine() = default;

bool IntentEngine::alice_available() const {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ALICE_SOCKET, sizeof(addr.sun_path) - 1);

    bool ok = (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(addr)) == 0);
    close(fd);
    return ok;
}

Result<IntentResult, std::string> IntentEngine::resolve(const IntentRequest& request) {
    if (request.natural_text.empty()) {
        return Result<IntentResult, std::string>::error("empty intent text");
    }

    // Try Alice first
    if (alice_available()) {
        auto result = resolve_via_alice(request);
        if (result) {
            return result;
        }
        // Alice failed, fall through to pattern matcher
    }

    return resolve_via_patterns(request);
}

Result<IntentResult, std::string> IntentEngine::resolve_via_alice(
    const IntentRequest& request) {

    // Build JSON-RPC params
    std::ostringstream params;
    params << "{";
    params << "\"text\": \"" << json_escape(request.natural_text) << "\"";
    if (!request.context.current_app.empty()) {
        params << ", \"current_app\": \"" << json_escape(request.context.current_app) << "\"";
    }
    if (!request.context.workspace.empty()) {
        params << ", \"workspace\": \"" << json_escape(request.context.workspace) << "\"";
    }
    if (!request.context.focused_window.empty()) {
        params << ", \"focused_window\": \"" << json_escape(request.context.focused_window) << "\"";
    }
    if (!request.context.selected_file.empty()) {
        params << ", \"selected_file\": \"" << json_escape(request.context.selected_file) << "\"";
    }
    params << "}";

    auto rpc_result = alice_rpc("intent.resolve", params.str());
    if (!rpc_result) {
        return Result<IntentResult, std::string>::error(rpc_result.error());
    }

    const auto& response = rpc_result.value();

    // Parse Alice response
    IntentResult ir;
    ir.action_name  = extract_json_string(response, "action_name");
    ir.action_type  = action_type_from_str(extract_json_string(response, "action_type"));
    ir.description  = extract_json_string(response, "description");
    ir.confidence   = extract_json_number(response, "confidence");
    ir.destructive  = extract_json_bool(response, "destructive");
    ir.commands     = extract_json_string_array(response, "commands");

    // If Alice gave us an action_name, resolve it through the ActionRegistry
    if (!ir.action_name.empty() && ir.commands.empty()) {
        auto& registry = ActionRegistry::instance();
        auto action = registry.lookup(ir.action_name);
        if (action) {
            ir.commands = action.value().commands;
            ir.action_type = action.value().type;
            ir.description = action.value().description;
            ir.destructive = action.value().destructive;
        }
    }

    if (ir.commands.empty()) {
        return Result<IntentResult, std::string>::error(
            "Alice returned no actionable commands");
    }

    return Result<IntentResult, std::string>::ok(std::move(ir));
}

Result<IntentResult, std::string> IntentEngine::resolve_via_patterns(
    const IntentRequest& request) {

    PatternMatcher matcher;
    auto match = matcher.match(request.natural_text);
    if (!match) {
        return Result<IntentResult, std::string>::error(
            "no matching intent pattern for: " + request.natural_text);
    }

    const auto& pm = match.value();

    // Resolve the matched action name through the ActionRegistry
    auto& registry = ActionRegistry::instance();
    auto action = registry.lookup(pm.action_name);

    IntentResult ir;
    ir.action_name = pm.action_name;
    ir.confidence  = pm.confidence;

    if (action) {
        ir.commands     = action.value().commands;
        ir.action_type  = action.value().type;
        ir.description  = action.value().description;
        ir.destructive  = action.value().destructive;
    } else {
        // Action not in registry — use the raw action name as a command
        ir.commands    = {pm.action_name};
        ir.action_type = ActionType::system_command;
        ir.description = "Execute: " + pm.action_name;
    }

    // Substitute captured arguments into commands
    if (!pm.captured_args.empty()) {
        for (auto& cmd : ir.commands) {
            for (size_t i = 0; i < pm.captured_args.size(); ++i) {
                std::string placeholder = "{" + std::to_string(i) + "}";
                size_t pos;
                while ((pos = cmd.find(placeholder)) != std::string::npos) {
                    cmd.replace(pos, placeholder.size(), pm.captured_args[i]);
                }
            }
            // Also replace {file} with the context file if present
            if (!request.context.selected_file.empty()) {
                size_t pos;
                while ((pos = cmd.find("{file}")) != std::string::npos) {
                    cmd.replace(pos, 6, request.context.selected_file);
                }
            }
        }
    }

    return Result<IntentResult, std::string>::ok(std::move(ir));
}

Result<std::string, std::string> IntentEngine::alice_rpc(
    const std::string& method, const std::string& params_json) {

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<std::string, std::string>::error("socket() failed");
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ALICE_SOCKET, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return Result<std::string, std::string>::error(
            "cannot connect to Alice at " + std::string(ALICE_SOCKET));
    }

    // Build JSON-RPC 2.0 request
    int id = ++rpc_id_;
    std::ostringstream rpc;
    rpc << "{\"jsonrpc\": \"2.0\", \"method\": \""
        << json_escape(method)
        << "\", \"params\": " << params_json
        << ", \"id\": " << id << "}\n";

    std::string msg = rpc.str();
    ssize_t sent = write(fd, msg.c_str(), msg.size());
    if (sent < 0 || static_cast<size_t>(sent) != msg.size()) {
        close(fd);
        return Result<std::string, std::string>::error("write to Alice failed");
    }

    // Wait for response with timeout (5 seconds)
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 5000);
    if (pr <= 0) {
        close(fd);
        return Result<std::string, std::string>::error("Alice response timeout");
    }

    // Read response
    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        response += buf;
        if (response.find('\n') != std::string::npos) break;
    }
    close(fd);

    if (response.empty()) {
        return Result<std::string, std::string>::error("empty response from Alice");
    }

    // Check for JSON-RPC error
    if (response.find("\"error\"") != std::string::npos &&
        response.find("\"result\"") == std::string::npos) {
        std::string err_msg = extract_json_string(response, "message");
        if (err_msg.empty()) err_msg = "unknown Alice RPC error";
        return Result<std::string, std::string>::error(err_msg);
    }

    // Extract result object
    auto result_pos = response.find("\"result\"");
    if (result_pos == std::string::npos) {
        return Result<std::string, std::string>::error("no result in Alice response");
    }

    // Find the opening brace of the result value
    auto brace_start = response.find('{', result_pos + 8);
    if (brace_start == std::string::npos) {
        return Result<std::string, std::string>::error("malformed Alice response");
    }

    // Find matching closing brace
    int depth = 0;
    size_t brace_end = brace_start;
    for (size_t i = brace_start; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        if (response[i] == '}') {
            --depth;
            if (depth == 0) {
                brace_end = i;
                break;
            }
        }
    }

    return Result<std::string, std::string>::ok(
        response.substr(brace_start, brace_end - brace_start + 1));
}

} // namespace straylight::intent
