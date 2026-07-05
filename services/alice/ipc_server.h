// services/alice/ipc_server.h
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight {

class AliceEngine;
class LogAnalyzer;
class AlertManager;

/// Unix domain socket IPC server for Alice.
/// Accepts JSON-RPC 2.0 requests and dispatches to engine/analyzer/alerts.
class AliceIpcServer {
public:
    AliceIpcServer();
    ~AliceIpcServer();

    AliceIpcServer(const AliceIpcServer&) = delete;
    AliceIpcServer& operator=(const AliceIpcServer&) = delete;

    /// Start listening on the given Unix socket path.
    Result<void, std::string> start(const std::string& socket_path, int max_clients = 4);

    /// Stop the server and close all connections.
    void stop();

    /// Set the components this server can dispatch to.
    void set_engine(AliceEngine* engine);
    void set_analyzer(LogAnalyzer* analyzer);
    void set_alerts(AlertManager* alerts);

private:
    int server_fd_ = -1;
    std::string socket_path_;
    std::atomic<bool> running_{false};
    int max_clients_ = 4;

    AliceEngine* engine_ = nullptr;
    LogAnalyzer* analyzer_ = nullptr;
    AlertManager* alerts_ = nullptr;

    std::thread accept_thread_;
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<ClientThread> client_threads_;
    std::mutex threads_mutex_;

    /// Main accept loop (runs in accept_thread_).
    void accept_loop();

    /// Handle a single client connection.
    void handle_client(int client_fd);

    /// Process a JSON-RPC request string and return a JSON response string.
    std::string dispatch(const std::string& request_json);

    // JSON-RPC method handlers
    std::string handle_status();
    std::string handle_ask(const std::string& query);
    std::string handle_analyze();
    std::string handle_alerts(int count);
    std::string handle_logs(int limit);

    /// Build a JSON-RPC success response.
    static std::string json_result(const std::string& id, const std::string& result_json);

    /// Build a JSON-RPC error response.
    static std::string json_error(const std::string& id, int code, const std::string& message);
};

} // namespace straylight
