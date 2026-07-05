// services/alice/ipc_server.cpp
#include "ipc_server.h"
#include "alice_engine.h"
#include "alert_manager.h"
#include "log_analyzer.h"
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

namespace straylight {

AliceIpcServer::AliceIpcServer() = default;

AliceIpcServer::~AliceIpcServer() {
    stop();
}

void AliceIpcServer::set_engine(AliceEngine* engine) { engine_ = engine; }
void AliceIpcServer::set_analyzer(LogAnalyzer* analyzer) { analyzer_ = analyzer; }
void AliceIpcServer::set_alerts(AlertManager* alerts) { alerts_ = alerts; }

Result<void, std::string> AliceIpcServer::start(const std::string& socket_path,
                                                  int max_clients) {
    socket_path_ = socket_path;
    max_clients_ = max_clients;

    // Remove stale socket file
    std::error_code ec;
    std::filesystem::remove(socket_path_, ec);

    // Ensure parent directory exists
    auto parent = std::filesystem::path(socket_path_).parent_path();
    std::filesystem::create_directories(parent, ec);

    // Create socket
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return Result<void, std::string>::error(
            std::string("socket() failed: ") + ::strerror(errno));
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, std::string>::error(
            std::string("bind() failed on ") + socket_path_ + ": " + ::strerror(e));
    }

    if (::listen(server_fd_, max_clients_) < 0) {
        int e = errno;
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return Result<void, std::string>::error(
            std::string("listen() failed: ") + ::strerror(e));
    }

    // Set socket permissions (owner + group read/write)
    ::chmod(socket_path_.c_str(), 0660);

    running_.store(true);
    accept_thread_ = std::thread(&AliceIpcServer::accept_loop, this);

    SL_INFO("alice: IPC server listening on {}", socket_path_);
    return Result<void, std::string>::ok();
}

void AliceIpcServer::stop() {
    running_.store(false);

    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    {
        std::lock_guard lock(threads_mutex_);
        for (auto& t : client_threads_) {
            if (t.thread.joinable()) {
                t.thread.join();
            }
        }
        client_threads_.clear();
    }

    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }

    SL_INFO("alice: IPC server stopped");
}

void AliceIpcServer::accept_loop() {
    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int pr = ::poll(&pfd, 1, 1000);  // 1s timeout for shutdown checks
        if (pr <= 0) continue;

        int client_fd = ::accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (running_.load()) {
                SL_WARN("alice: accept() failed: {}", ::strerror(errno));
            }
            continue;
        }

        // Clean up finished threads
        {
            std::lock_guard lock(threads_mutex_);
            for (auto it = client_threads_.begin(); it != client_threads_.end();) {
                if (it->done && it->done->load()) {
                    if (it->thread.joinable()) {
                        it->thread.join();
                    }
                    it = client_threads_.erase(it);
                } else {
                    ++it;
                }
            }

            if (static_cast<int>(client_threads_.size()) >= max_clients_) {
                SL_WARN("alice: max clients ({}) reached, rejecting connection", max_clients_);
                std::string err = json_error("null", -32000, "Server busy");
                err += "\n";
                ::write(client_fd, err.data(), err.size());
                ::close(client_fd);
                continue;
            }

            auto done = std::make_shared<std::atomic<bool>>(false);
            client_threads_.push_back(ClientThread{
                std::thread([this, client_fd, done]() {
                    handle_client(client_fd);
                    done->store(true);
                }),
                done
            });
        }
    }
}

void AliceIpcServer::handle_client(int client_fd) {
    SL_DEBUG("alice: client connected (fd={})", client_fd);

    while (running_.load()) {
        // Poll for incoming data with timeout
        struct pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        int pr = ::poll(&pfd, 1, 5000);
        if (pr < 0) break;
        if (pr == 0) continue;  // Timeout, check running flag

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        // Read newline-delimited JSON
        std::string request;
        char buf[4096];

        while (true) {
            ssize_t n = ::read(client_fd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                goto client_done;
            }
            request.append(buf, static_cast<size_t>(n));
            if (request.find('\n') != std::string::npos) break;
        }

        // Trim trailing newline
        while (!request.empty() && (request.back() == '\n' || request.back() == '\r')) {
            request.pop_back();
        }

        if (request.empty()) continue;

        {
            std::string response = dispatch(request);
            response += "\n";
            size_t total = 0;
            while (total < response.size()) {
                ssize_t n = ::write(client_fd, response.data() + total,
                                    response.size() - total);
                if (n <= 0) {
                    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                    goto client_done;
                }
                total += static_cast<size_t>(n);
            }
        }
    }

client_done:
    ::close(client_fd);
    SL_DEBUG("alice: client disconnected (fd={})", client_fd);
}

std::string AliceIpcServer::dispatch(const std::string& request_json) {
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(request_json);
    } catch (const nlohmann::json::parse_error& e) {
        return json_error("null", -32700, std::string("Parse error: ") + e.what());
    }

    // Extract JSON-RPC fields
    std::string id = "null";
    if (req.contains("id")) {
        if (req["id"].is_string()) {
            id = "\"" + req["id"].get<std::string>() + "\"";
        } else {
            id = req["id"].dump();
        }
    }

    if (!req.contains("method") || !req["method"].is_string()) {
        return json_error(id, -32600, "Invalid request: missing 'method'");
    }

    std::string method = req["method"].get<std::string>();

    // Also support the simpler {"cmd": "..."} format used by IpcJsonClient
    if (method.empty() && req.contains("cmd") && req["cmd"].is_string()) {
        method = req["cmd"].get<std::string>();
    }

    if (method == "status") {
        return json_result(id, handle_status());
    } else if (method == "ask") {
        std::string query;
        if (req.contains("params") && req["params"].is_object() &&
            req["params"].contains("query")) {
            query = req["params"]["query"].get<std::string>();
        } else if (req.contains("params") && req["params"].is_string()) {
            query = req["params"].get<std::string>();
        }
        if (query.empty()) {
            return json_error(id, -32602, "Missing 'query' parameter");
        }
        return json_result(id, handle_ask(query));
    } else if (method == "analyze") {
        return json_result(id, handle_analyze());
    } else if (method == "alerts") {
        int count = 50;
        if (req.contains("params") && req["params"].is_object() &&
            req["params"].contains("count")) {
            count = req["params"]["count"].get<int>();
        }
        return json_result(id, handle_alerts(count));
    } else if (method == "logs") {
        int limit = 50;
        if (req.contains("params") && req["params"].is_object() &&
            req["params"].contains("limit")) {
            limit = req["params"]["limit"].get<int>();
        }
        return json_result(id, handle_logs(limit));
    }

    return json_error(id, -32601, "Method not found: " + method);
}

// ---------------------------------------------------------------------------
// Method handlers
// ---------------------------------------------------------------------------

std::string AliceIpcServer::handle_status() {
    nlohmann::json result;
    result["daemon"] = "alice";
    result["status"] = "running";
    result["model_loaded"] = engine_ ? engine_->is_loaded() : false;

    // Quick health summary
    if (analyzer_) {
        auto thermal = analyzer_->thermal_status();
        result["thermal"] = thermal.has_value() ? "ok" : "unavailable";

        auto memory = analyzer_->memory_pressure();
        result["memory"] = memory.has_value() ? "ok" : "unavailable";
    }

    if (alerts_) {
        auto recent_alerts = alerts_->recent(5);
        result["recent_alert_count"] = static_cast<int>(recent_alerts.size());
        if (!recent_alerts.empty()) {
            result["latest_alert"] = recent_alerts.front().title;
            result["latest_severity"] = AlertManager::severity_to_string(
                recent_alerts.front().severity);
        }
    }

    return result.dump();
}

std::string AliceIpcServer::handle_ask(const std::string& query) {
    nlohmann::json result;

    if (!engine_) {
        result["error"] = "Engine not available";
        return result.dump();
    }

    auto response = engine_->analyze(query);
    if (response.has_value()) {
        result["response"] = response.value();
    } else {
        result["error"] = response.error();
    }

    return result.dump();
}

std::string AliceIpcServer::handle_analyze() {
    nlohmann::json result;

    if (!analyzer_) {
        result["error"] = "Analyzer not available";
        return result.dump();
    }

    // Collect system data
    std::string system_report;

    auto thermal = analyzer_->thermal_status();
    if (thermal.has_value()) system_report += thermal.value() + "\n";

    auto memory = analyzer_->memory_pressure();
    if (memory.has_value()) system_report += memory.value() + "\n";

    auto gpu = analyzer_->gpu_health();
    if (gpu.has_value()) system_report += gpu.value() + "\n";

    auto disk = analyzer_->disk_health();
    if (disk.has_value()) system_report += disk.value() + "\n";

    auto net = analyzer_->network_status();
    if (net.has_value()) system_report += net.value() + "\n";

    // Collect recent logs
    auto logs = analyzer_->collect_recent(100);
    if (logs.has_value()) {
        auto summary = analyzer_->summarize_for_ai(logs.value());
        if (summary.has_value()) {
            system_report += summary.value() + "\n";
        }
    }

    result["system_report"] = system_report;

    // Run AI analysis if engine available
    if (engine_) {
        auto ai = engine_->analyze(system_report);
        if (ai.has_value()) {
            result["ai_analysis"] = ai.value();
        } else {
            result["ai_error"] = ai.error();
        }
    }

    return result.dump();
}

std::string AliceIpcServer::handle_alerts(int count) {
    nlohmann::json result;

    if (!alerts_) {
        result["error"] = "Alert manager not available";
        return result.dump();
    }

    auto recent_alerts = alerts_->recent(count);
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& a : recent_alerts) {
        nlohmann::json obj;
        obj["severity"] = AlertManager::severity_to_string(a.severity);
        obj["category"] = a.category;
        obj["title"] = a.title;
        obj["detail"] = a.detail;
        if (!a.ai_analysis.empty()) {
            obj["ai_analysis"] = a.ai_analysis;
        }
        auto time_t_val = std::chrono::system_clock::to_time_t(a.timestamp);
        std::tm tm_val{};
        ::localtime_r(&time_t_val, &tm_val);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_val);
        obj["timestamp"] = time_buf;
        arr.push_back(std::move(obj));
    }

    result["alerts"] = std::move(arr);
    result["count"] = static_cast<int>(recent_alerts.size());
    return result.dump();
}

std::string AliceIpcServer::handle_logs(int limit) {
    nlohmann::json result;

    if (!analyzer_) {
        result["error"] = "Analyzer not available";
        return result.dump();
    }

    auto logs = analyzer_->collect_recent(limit);
    if (!logs.has_value()) {
        result["error"] = logs.error();
        return result.dump();
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& entry : logs.value()) {
        nlohmann::json obj;
        obj["source"] = entry.source;
        obj["level"] = entry.level;
        obj["message"] = entry.message;
        arr.push_back(std::move(obj));
    }

    result["logs"] = std::move(arr);
    result["count"] = static_cast<int>(logs.value().size());
    return result.dump();
}

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

std::string AliceIpcServer::json_result(const std::string& id,
                                         const std::string& result_json) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result_json + "}";
}

std::string AliceIpcServer::json_error(const std::string& id, int code,
                                        const std::string& message) {
    nlohmann::json err;
    err["code"] = code;
    err["message"] = message;
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":" + err.dump() + "}";
}

} // namespace straylight
