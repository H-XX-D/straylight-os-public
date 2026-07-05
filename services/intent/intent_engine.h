/**
 * StrayLight Intent Engine — Natural-language to system action resolution.
 *
 * Takes natural language input ("record my screen at 4k") and resolves it
 * into structured system actions via Alice AI or pattern-matching fallback.
 */
#pragma once

#include "straylight/result.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace straylight::intent {

// ─── Types ──────────────────────────────────────────────────────────────────

enum class ActionType {
    system_command,
    pipeline,
    compositor_action,
    vpu_action,
    service_control,
    file_operation,
    unknown
};

inline const char* action_type_str(ActionType t) {
    switch (t) {
        case ActionType::system_command:    return "system_command";
        case ActionType::pipeline:          return "pipeline";
        case ActionType::compositor_action: return "compositor_action";
        case ActionType::vpu_action:        return "vpu_action";
        case ActionType::service_control:   return "service_control";
        case ActionType::file_operation:    return "file_operation";
        case ActionType::unknown:           return "unknown";
    }
    return "unknown";
}

inline ActionType action_type_from_str(const std::string& s) {
    if (s == "system_command")    return ActionType::system_command;
    if (s == "pipeline")          return ActionType::pipeline;
    if (s == "compositor_action") return ActionType::compositor_action;
    if (s == "vpu_action")        return ActionType::vpu_action;
    if (s == "service_control")   return ActionType::service_control;
    if (s == "file_operation")    return ActionType::file_operation;
    return ActionType::unknown;
}

struct IntentContext {
    std::string current_app;
    std::string workspace;
    std::string focused_window;
    std::string selected_file;
    std::map<std::string, std::string> extra;
};

struct IntentRequest {
    std::string natural_text;
    IntentContext context;
};

struct IntentResult {
    ActionType action_type = ActionType::unknown;
    std::vector<std::string> commands;
    std::string description;
    double confidence = 0.0;
    std::string action_name;
    bool destructive = false;
};

// ─── Engine ─────────────────────────────────────────────────────────────────

class IntentEngine {
public:
    IntentEngine();
    ~IntentEngine();

    /** Resolve a natural language request into an IntentResult.
     *  Tries Alice first, falls back to pattern matching. */
    Result<IntentResult, std::string> resolve(const IntentRequest& request);

    /** Check if Alice AI is reachable. */
    bool alice_available() const;

private:
    /** Send request to Alice via Unix socket IPC. */
    Result<IntentResult, std::string> resolve_via_alice(const IntentRequest& request);

    /** Fallback pattern matching. */
    Result<IntentResult, std::string> resolve_via_patterns(const IntentRequest& request);

    /** Connect to Alice's Unix socket and send/receive JSON-RPC. */
    Result<std::string, std::string> alice_rpc(const std::string& method,
                                               const std::string& params_json);

    static constexpr const char* ALICE_SOCKET = "/run/straylight/alice.sock";
    int rpc_id_ = 0;
};

} // namespace straylight::intent
