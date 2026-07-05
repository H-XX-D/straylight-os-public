// services/voice/tool_executor.h
// Execute StrayLight system tools from LLM decisions.
#pragma once

#include "voice_config.h"
#include "straylight/result.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace straylight::voice {

/// Parameter definition for a tool.
struct ToolParam {
    std::string name;
    std::string type;         // "string", "int", "float", "bool"
    std::string description;
    bool required = true;
};

/// Definition of a callable tool.
struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolParam> parameters;
    bool dangerous = false;   // requires confirmation
};

/// Result of executing a tool.
struct ToolResult {
    bool        success = false;
    int         exit_code = 0;
    std::string output;
    std::string error;
    double      duration_s = 0.0;
};

/// Tool executor: registry + execution engine for all StrayLight tools.
class ToolExecutor {
public:
    ToolExecutor() = default;

    /// Initialize the tool registry with the voice config (for safety rules).
    Result<void, std::string> init(const VoiceConfig& cfg);

    /// Execute a tool by name with JSON arguments.
    Result<ToolResult, std::string> execute(
        const std::string& tool_name,
        const std::string& args_json);

    /// Get all registered tool definitions.
    const std::vector<ToolDefinition>& tools() const { return tools_; }

    /// Look up a tool by name.
    const ToolDefinition* find_tool(const std::string& name) const;

    /// Check if a tool call requires confirmation.
    bool requires_confirmation(const std::string& tool_name,
                               const std::string& args_json) const;

    /// Format the tool registry as a prompt section for the LLM.
    std::string tools_prompt() const;

    /// Set a confirmation callback (returns true = user confirmed).
    using ConfirmCallback = std::function<bool(const std::string& description)>;
    void set_confirm_callback(ConfirmCallback cb) { confirm_cb_ = std::move(cb); }

    /// Whether to auto-confirm (skip user confirmation for testing).
    void set_auto_confirm(bool v) { auto_confirm_ = v; }

private:
    std::vector<ToolDefinition> tools_;
    VoiceConfig config_;
    ConfirmCallback confirm_cb_;
    bool auto_confirm_ = false;

    /// Register all built-in tools.
    void register_tools();

    /// Execute a shell command and capture output.
    ToolResult run_command(const std::string& cmd);

    /// IPC call to a StrayLight daemon socket.
    ToolResult ipc_call(const std::string& socket_path,
                        const std::string& method,
                        const std::string& params_json);

    /// Extract a string value from a JSON object.
    static std::string json_str(const std::string& json, const std::string& key);

    /// Extract an int value from a JSON object.
    static int json_int(const std::string& json, const std::string& key);

    /// Extract a float value from a JSON object.
    static float json_float(const std::string& json, const std::string& key);
};

} // namespace straylight::voice
