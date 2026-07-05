// services/voice/llm_engine.h
// Local LLM engine backed by llama.cpp for reasoning and tool use.
#pragma once

#include "conversation.h"
#include "tool_executor.h"
#include "voice_config.h"
#include "straylight/result.h"

#include <string>
#include <vector>

namespace straylight::voice {

/// LLM generation parameters.
struct LlmParams {
    float temperature   = 0.7f;
    float top_p         = 0.9f;
    int   max_tokens    = 512;
    float repeat_penalty = 1.1f;
};

/// A single tool call parsed from LLM output.
struct LlmToolCall {
    std::string tool_name;
    std::string arguments_json;  // raw JSON string of arguments
};

/// Full LLM response.
struct LlmResponse {
    std::string text;                      // final text output
    std::vector<LlmToolCall> tool_calls;   // any tool calls made
    std::vector<std::string> tool_results; // results from executed tools
    int   tokens_generated = 0;
    double inference_time_s = 0.0;
};

/// Local LLM engine for voice assistant reasoning.
class LlmEngine {
public:
    LlmEngine() = default;
    ~LlmEngine();

    // Non-copyable.
    LlmEngine(const LlmEngine&) = delete;
    LlmEngine& operator=(const LlmEngine&) = delete;

    /// Initialize with config, load model.
    Result<void, std::string> init(const VoiceConfig& cfg);

    /// Generate a response for a user message, with conversation context.
    /// This handles the full loop: prompt -> generate -> parse tool calls ->
    /// execute tools -> re-prompt with results -> final response.
    Result<LlmResponse, std::string> generate(
        const std::string& user_message,
        Conversation& conversation,
        ToolExecutor& tools);

    /// Simple generation without tool use (for testing / one-shot).
    Result<std::string, std::string> complete(const std::string& prompt);

    /// Check if model is loaded.
    bool is_loaded() const { return model_loaded_; }

    /// Get model path.
    const std::string& model_path() const { return model_path_; }

    /// Set generation parameters at runtime.
    void set_params(const LlmParams& params) { params_ = params; }

    /// Get current system context (CPU, memory, etc.) for injection.
    std::string get_system_context() const;

private:
    std::string model_path_;
    std::string system_prompt_;
    LlmParams params_;
    bool model_loaded_ = false;

    // Opaque handle to llama_context / llama_model.
    void* llama_model_ = nullptr;
    void* llama_ctx_   = nullptr;

    /// Build the full prompt from system prompt + context + conversation history.
    std::string build_prompt(
        const std::string& user_message,
        const Conversation& conversation,
        const ToolExecutor& tools) const;

    /// Build a follow-up prompt after tool execution.
    std::string build_tool_result_prompt(
        const std::string& original_message,
        const Conversation& conversation,
        const std::vector<LlmToolCall>& calls,
        const std::vector<std::string>& results) const;

    /// Parse tool calls from LLM output.
    std::vector<LlmToolCall> parse_tool_calls(const std::string& output) const;

    /// Run inference via llama.cpp (or CLI fallback).
    Result<std::string, std::string> run_inference(const std::string& prompt);

    /// Fallback: use a CLI tool (llama-cli, ollama, etc.).
    Result<std::string, std::string> fallback_inference(const std::string& prompt);

    /// Read /proc/stat, /proc/meminfo, etc. for system context.
    std::string read_system_stats() const;
};

} // namespace straylight::voice
