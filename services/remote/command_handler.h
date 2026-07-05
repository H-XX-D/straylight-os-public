// services/remote/command_handler.h
#pragma once

#include <straylight/result.h>
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <unordered_map>

namespace straylight {

class FileTransfer;
class SystemInfo;

/// Dispatches JSON-RPC 2.0 requests to the appropriate handler.
/// Each RPC method maps to a system capability (exec, upload, sysinfo, etc.).
class CommandHandler {
public:
    CommandHandler();
    ~CommandHandler();

    /// Process a raw JSON-RPC request and return a JSON-RPC response.
    std::string handle(const std::string& request);

    // Feature flags
    void set_feature_exec(bool enabled) { feat_exec_ = enabled; }
    void set_feature_file_transfer(bool enabled) { feat_file_transfer_ = enabled; }
    void set_feature_service_management(bool enabled) { feat_service_mgmt_ = enabled; }
    void set_feature_package_management(bool enabled) { feat_package_mgmt_ = enabled; }
    void set_feature_alice(bool enabled) { feat_alice_ = enabled; }

    // Limits
    void set_max_upload_bytes(size_t bytes) { max_upload_bytes_ = bytes; }
    void set_max_exec_timeout(int seconds) { max_exec_timeout_s_ = seconds; }

private:
    // Feature flags
    bool feat_exec_ = true;
    bool feat_file_transfer_ = true;
    bool feat_service_mgmt_ = true;
    bool feat_package_mgmt_ = false;
    bool feat_alice_ = true;

    // Limits
    size_t max_upload_bytes_ = 1024ULL * 1024 * 1024;  // 1GB default
    int max_exec_timeout_s_ = 3600;

    // Sub-modules
    std::unique_ptr<FileTransfer> file_transfer_;
    std::unique_ptr<SystemInfo> system_info_;

    // RPC method handlers
    using MethodHandler = std::function<nlohmann::json(const nlohmann::json&)>;
    std::unordered_map<std::string, MethodHandler> methods_;

    void register_methods();

    // Individual method implementations
    nlohmann::json handle_exec(const nlohmann::json& params);
    nlohmann::json handle_exec_stream(const nlohmann::json& params);
    nlohmann::json handle_upload(const nlohmann::json& params);
    nlohmann::json handle_download(const nlohmann::json& params);
    nlohmann::json handle_sysinfo(const nlohmann::json& params);
    nlohmann::json handle_services(const nlohmann::json& params);
    nlohmann::json handle_logs(const nlohmann::json& params);
    nlohmann::json handle_processes(const nlohmann::json& params);
    nlohmann::json handle_alice(const nlohmann::json& params);
    nlohmann::json handle_packages(const nlohmann::json& params);
    nlohmann::json handle_fs(const nlohmann::json& params);

    // JSON-RPC helpers
    static std::string make_response(const nlohmann::json& result, const nlohmann::json& id);
    static std::string make_error(int code, const std::string& message, const nlohmann::json& id);

    // Path validation to prevent directory traversal
    static bool is_safe_path(const std::string& path);
};

} // namespace straylight
