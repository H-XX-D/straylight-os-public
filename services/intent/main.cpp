/**
 * StrayLight Intent Daemon — Natural-language system command resolution.
 *
 * Listens on /run/straylight/intent.sock for JSON-RPC requests.
 * Clients send natural language text and receive structured action plans.
 *
 * JSON-RPC methods:
 *   intent.resolve   — Resolve natural text into system commands
 *   intent.execute   — Resolve and execute (with optional confirmation)
 *   intent.actions    — List all registered actions
 *   intent.reload     — Reload action registry
 */

#include "intent_engine.h"
#include "action_registry.h"
#include "executor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

namespace {

using namespace straylight;
using namespace straylight::intent;

static constexpr const char* SOCKET_PATH = "/run/straylight/intent.sock";
static constexpr int MAX_CLIENTS = 32;
static constexpr int READ_BUF_SIZE = 8192;

// ─── JSON helpers ───────────────────────────────────────────────────────────

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

static int extract_json_int(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return 0;
    try { return std::stoi(json.substr(pos)); } catch (...) { return 0; }
}

// ─── Response builders ──────────────────────────────────────────────────────

static std::string rpc_error(int id, int code, const std::string& message) {
    std::ostringstream out;
    out << "{\"jsonrpc\": \"2.0\", \"error\": {\"code\": " << code
        << ", \"message\": \"" << json_escape(message)
        << "\"}, \"id\": " << id << "}\n";
    return out.str();
}

static std::string rpc_result(int id, const std::string& result_json) {
    std::ostringstream out;
    out << "{\"jsonrpc\": \"2.0\", \"result\": " << result_json
        << ", \"id\": " << id << "}\n";
    return out.str();
}

static std::string intent_result_to_json(const IntentResult& ir) {
    std::ostringstream out;
    out << "{";
    out << "\"action_type\": \"" << action_type_str(ir.action_type) << "\", ";
    out << "\"action_name\": \"" << json_escape(ir.action_name) << "\", ";
    out << "\"description\": \"" << json_escape(ir.description) << "\", ";
    out << "\"confidence\": " << ir.confidence << ", ";
    out << "\"destructive\": " << (ir.destructive ? "true" : "false") << ", ";
    out << "\"commands\": [";
    for (size_t i = 0; i < ir.commands.size(); ++i) {
        if (i > 0) out << ", ";
        out << "\"" << json_escape(ir.commands[i]) << "\"";
    }
    out << "]}";
    return out.str();
}

static std::string exec_results_to_json(const std::vector<ExecutionResult>& results) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) out << ", ";
        out << "{\"success\": " << (results[i].success ? "true" : "false")
            << ", \"exit_code\": " << results[i].exit_code
            << ", \"duration_ms\": " << results[i].duration.count()
            << ", \"output\": \"" << json_escape(results[i].output.substr(0, 2000)) << "\""
            << ", \"error\": \"" << json_escape(results[i].error_output.substr(0, 1000)) << "\""
            << "}";
    }
    out << "]";
    return out.str();
}

// ─── Intent Daemon ──────────────────────────────────────────────────────────

class IntentDaemon : public DaemonBase {
public:
    IntentDaemon() = default;

protected:
    Result<void, SLError> init(const Config& /*cfg*/) override {
        // Create socket directory
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(SOCKET_PATH).parent_path(), ec);

        // Remove stale socket
        ::unlink(SOCKET_PATH);

        // Create Unix socket
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            return fail("socket() failed: " + std::string(strerror(errno)));
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(listen_fd_);
            return fail("bind() failed: " + std::string(strerror(errno)));
        }

        // Allow all users to connect
        chmod(SOCKET_PATH, 0666);

        if (listen(listen_fd_, 8) < 0) {
            close(listen_fd_);
            return fail("listen() failed: " + std::string(strerror(errno)));
        }

        fprintf(stdout, "[straylight-intent] listening on %s\n", SOCKET_PATH);
        fprintf(stdout, "[straylight-intent] Alice AI at %s: %s\n",
                "/run/straylight/alice.sock",
                engine_.alice_available() ? "AVAILABLE" : "offline (pattern fallback)");
        fprintf(stdout, "[straylight-intent] %zu actions registered\n",
                ActionRegistry::instance().list_actions().size());

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        // Accept new connections
        while (true) {
            int client_fd = accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) break;

            if (client_fds_.size() >= MAX_CLIENTS) {
                const char* msg = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,"
                                  "\"message\":\"too many clients\"},\"id\":0}\n";
                write(client_fd, msg, strlen(msg));
                close(client_fd);
                continue;
            }

            client_fds_.push_back(client_fd);
        }

        if (client_fds_.empty()) return finish_tick();

        // Poll all clients
        std::vector<struct pollfd> pfds;
        pfds.reserve(client_fds_.size());
        for (int fd : client_fds_) {
            pfds.push_back({fd, POLLIN, 0});
        }

        int ready = poll(pfds.data(), pfds.size(), 10);
        if (ready <= 0) return finish_tick();

        std::vector<int> to_remove;

        for (size_t i = 0; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            char buf[READ_BUF_SIZE];
            ssize_t n = read(pfds[i].fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                to_remove.push_back(pfds[i].fd);
                continue;
            }
            buf[n] = '\0';

            std::string request(buf, n);
            std::string response = handle_request(request);

            // Write response
            ssize_t written = write(pfds[i].fd, response.c_str(), response.size());
            (void)written;
        }

        // Remove disconnected clients
        for (int fd : to_remove) {
            close(fd);
            client_fds_.erase(
                std::remove(client_fds_.begin(), client_fds_.end(), fd),
                client_fds_.end());
        }

        return finish_tick();
    }

    void shutdown() override {
        // Close all client connections
        for (int fd : client_fds_) {
            close(fd);
        }
        client_fds_.clear();

        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        ::unlink(SOCKET_PATH);
        fprintf(stdout, "[straylight-intent] socket removed\n");
    }

private:
    int listen_fd_ = -1;
    std::vector<int> client_fds_;
    IntentEngine engine_;
    Executor executor_;

    static Result<void, SLError> fail(const std::string& message) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, message});
    }

    static Result<void, SLError> finish_tick() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return Result<void, SLError>::ok();
    }

    std::string handle_request(const std::string& raw) {
        std::string method = extract_json_string(raw, "method");
        int id = extract_json_int(raw, "id");

        if (method == "intent.resolve") {
            return handle_resolve(raw, id);
        } else if (method == "intent.execute") {
            return handle_execute(raw, id);
        } else if (method == "intent.actions") {
            return handle_list_actions(id);
        } else if (method == "intent.reload") {
            return handle_reload(id);
        } else {
            return rpc_error(id, -32601, "method not found: " + method);
        }
    }

    std::string handle_resolve(const std::string& raw, int id) {
        // Extract params
        auto params_pos = raw.find("\"params\"");
        std::string text;
        IntentContext ctx;

        if (params_pos != std::string::npos) {
            // Find the params object
            auto brace = raw.find('{', params_pos + 8);
            if (brace != std::string::npos) {
                int depth = 0;
                size_t end = brace;
                for (size_t i = brace; i < raw.size(); ++i) {
                    if (raw[i] == '{') ++depth;
                    if (raw[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
                }
                std::string params = raw.substr(brace, end - brace + 1);
                text = extract_json_string(params, "text");
                ctx.current_app = extract_json_string(params, "current_app");
                ctx.workspace = extract_json_string(params, "workspace");
                ctx.focused_window = extract_json_string(params, "focused_window");
                ctx.selected_file = extract_json_string(params, "selected_file");
            }
        }

        if (text.empty()) {
            return rpc_error(id, -32602, "missing 'text' in params");
        }

        IntentRequest req{text, ctx};
        auto result = engine_.resolve(req);

        if (!result) {
            return rpc_error(id, -32000, result.error());
        }

        return rpc_result(id, intent_result_to_json(result.value()));
    }

    std::string handle_execute(const std::string& raw, int id) {
        // First resolve the intent
        auto params_pos = raw.find("\"params\"");
        std::string text;
        IntentContext ctx;
        bool force = false;

        if (params_pos != std::string::npos) {
            auto brace = raw.find('{', params_pos + 8);
            if (brace != std::string::npos) {
                int depth = 0;
                size_t end = brace;
                for (size_t i = brace; i < raw.size(); ++i) {
                    if (raw[i] == '{') ++depth;
                    if (raw[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
                }
                std::string params = raw.substr(brace, end - brace + 1);
                text = extract_json_string(params, "text");
                ctx.current_app = extract_json_string(params, "current_app");
                ctx.workspace = extract_json_string(params, "workspace");
                ctx.selected_file = extract_json_string(params, "selected_file");

                // Check for force flag
                if (params.find("\"force\"") != std::string::npos &&
                    params.find("true") != std::string::npos) {
                    force = true;
                }
            }
        }

        if (text.empty()) {
            return rpc_error(id, -32602, "missing 'text' in params");
        }

        IntentRequest req{text, ctx};
        auto resolve_result = engine_.resolve(req);

        if (!resolve_result) {
            return rpc_error(id, -32000, resolve_result.error());
        }

        const auto& intent = resolve_result.value();

        // If destructive and not forced, return the plan without executing
        if (intent.destructive && !force) {
            std::ostringstream out;
            out << "{\"needs_confirmation\": true, \"intent\": "
                << intent_result_to_json(intent) << "}";
            return rpc_result(id, out.str());
        }

        // Execute
        auto exec_results = executor_.execute(intent, text);

        std::ostringstream out;
        out << "{\"intent\": " << intent_result_to_json(intent)
            << ", \"execution\": " << exec_results_to_json(exec_results) << "}";

        return rpc_result(id, out.str());
    }

    std::string handle_list_actions(int id) {
        auto names = ActionRegistry::instance().list_actions();
        std::ostringstream out;
        out << "{\"actions\": [";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) out << ", ";
            auto action = ActionRegistry::instance().lookup(names[i]);
            if (action) {
                out << "{\"name\": \"" << json_escape(names[i]) << "\""
                    << ", \"description\": \"" << json_escape(action.value().description) << "\""
                    << ", \"type\": \"" << action_type_str(action.value().type) << "\""
                    << ", \"destructive\": " << (action.value().destructive ? "true" : "false")
                    << "}";
            }
        }
        out << "], \"count\": " << names.size() << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_reload(int id) {
        ActionRegistry::instance().reload();
        size_t count = ActionRegistry::instance().list_actions().size();
        std::ostringstream out;
        out << "{\"reloaded\": true, \"actions_count\": " << count << "}";
        return rpc_result(id, out.str());
    }
};

} // anonymous namespace

int main() {
    Log::init("straylight-intent");
    IntentDaemon daemon;
    return daemon.run(Config::make_empty());
}
