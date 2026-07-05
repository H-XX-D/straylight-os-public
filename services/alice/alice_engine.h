// services/alice/alice_engine.h
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

// Forward declarations for llama.cpp types (only used when HAVE_LLAMA_CPP is defined)
#ifdef HAVE_LLAMA_CPP
struct llama_model;
struct llama_context;
struct llama_sampler;
#endif

namespace straylight {

/// GGUF model inference engine for Alice.
/// Wraps llama.cpp for local LLM inference with automatic VRAM management.
/// Falls back to rule-based analysis when compiled without HAVE_LLAMA_CPP.
class AliceEngine {
public:
    struct ModelConfig {
        std::string model_path;
        int context_size = 2048;
        int threads = 4;
        float temperature = 0.3f;
        bool gpu_offload = true;
        int gpu_layers = -1;  // -1 = auto
        int idle_unload_seconds = 60;
    };

    AliceEngine();
    ~AliceEngine();

    AliceEngine(const AliceEngine&) = delete;
    AliceEngine& operator=(const AliceEngine&) = delete;

    /// Configure the engine. Must be called before analyze().
    void configure(const ModelConfig& config);

    /// Load the GGUF model into memory. Called automatically by analyze() if needed.
    Result<void, std::string> load_model(const std::string& model_path);

    /// Run inference on a prompt and return the model's response.
    /// Loads the model on first call, unloads after idle timeout.
    Result<std::string, std::string> analyze(const std::string& prompt);

    /// Immediately unload model from memory / VRAM.
    void unload();

    /// Check if the model is currently loaded.
    [[nodiscard]] bool is_loaded() const;

    /// Start the idle eviction timer thread.
    void start_idle_timer();

    /// Stop the idle eviction timer thread.
    void stop_idle_timer();

private:
    ModelConfig config_;
    mutable std::mutex mutex_;

#ifdef HAVE_LLAMA_CPP
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    llama_sampler* sampler_ = nullptr;
#endif

    std::atomic<bool> loaded_{false};
    std::chrono::steady_clock::time_point last_use_;

    // Idle eviction
    std::thread idle_thread_;
    std::atomic<bool> idle_running_{false};

    void idle_eviction_loop();

    // Rule-based fallback (used when HAVE_LLAMA_CPP is not defined)
    std::string rule_based_analysis(const std::string& prompt) const;

    static constexpr const char* SYSTEM_PROMPT =
        "You are Alice, the StrayLight OS system monitor. "
        "Analyze the following system data and provide a concise health assessment. "
        "Flag any warnings or anomalies.";
};

} // namespace straylight
