// services/alice/alice_engine.cpp
#include "alice_engine.h"
#include <straylight/log.h>

#ifdef HAVE_LLAMA_CPP
#include <llama.h>
#endif

#include <algorithm>
#include <sstream>

namespace straylight {

AliceEngine::AliceEngine() = default;

AliceEngine::~AliceEngine() {
    stop_idle_timer();
    unload();
}

void AliceEngine::configure(const ModelConfig& config) {
    std::lock_guard lock(mutex_);
    config_ = config;
}

Result<void, std::string> AliceEngine::load_model(const std::string& model_path) {
    std::lock_guard lock(mutex_);

    if (loaded_.load()) {
        return Result<void, std::string>::ok();
    }

#ifdef HAVE_LLAMA_CPP
    SL_INFO("alice: loading model from {}", model_path);

    // Initialize llama backend
    llama_backend_init();

    // Model parameters
    auto model_params = llama_model_default_params();
    if (config_.gpu_offload) {
        model_params.n_gpu_layers = (config_.gpu_layers < 0) ? 99 : config_.gpu_layers;
    } else {
        model_params.n_gpu_layers = 0;
    }

    model_ = llama_model_load_from_file(model_path.c_str(), model_params);
    if (!model_) {
        llama_backend_free();
        return Result<void, std::string>::error("Failed to load model: " + model_path);
    }

    // Context parameters
    auto ctx_params = llama_context_default_params();
    ctx_params.n_ctx = static_cast<uint32_t>(config_.context_size);
    ctx_params.n_threads = static_cast<uint32_t>(config_.threads);
    ctx_params.n_threads_batch = static_cast<uint32_t>(config_.threads);

    ctx_ = llama_context_init_from_model(model_, ctx_params);
    if (!ctx_) {
        llama_model_free(model_);
        model_ = nullptr;
        llama_backend_free();
        return Result<void, std::string>::error("Failed to create context for model");
    }

    // Sampler chain
    auto sparams = llama_sampler_chain_default_params();
    sampler_ = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler_, llama_sampler_init_temp(config_.temperature));
    llama_sampler_chain_add(sampler_, llama_sampler_init_dist(42));

    loaded_.store(true);
    last_use_ = std::chrono::steady_clock::now();
    SL_INFO("alice: model loaded successfully ({} ctx, {} threads)",
            config_.context_size, config_.threads);

    return Result<void, std::string>::ok();
#else
    (void)model_path;
    SL_INFO("alice: HAVE_LLAMA_CPP not defined, using rule-based analysis");
    loaded_.store(true);
    last_use_ = std::chrono::steady_clock::now();
    return Result<void, std::string>::ok();
#endif
}

Result<std::string, std::string> AliceEngine::analyze(const std::string& prompt) {
    // Load model if not already loaded
    if (!loaded_.load()) {
        auto load_result = load_model(config_.model_path);
        if (!load_result.has_value()) {
            return Result<std::string, std::string>::error(load_result.error());
        }
    }

    std::lock_guard lock(mutex_);
    last_use_ = std::chrono::steady_clock::now();

#ifdef HAVE_LLAMA_CPP
    // Build the full prompt with system instruction
    std::string full_prompt = std::string("<|im_start|>system\n") + SYSTEM_PROMPT +
                              "<|im_end|>\n<|im_start|>user\n" + prompt +
                              "<|im_end|>\n<|im_start|>assistant\n";

    // Tokenize
    const int max_tokens = config_.context_size;
    std::vector<llama_token> tokens(max_tokens);
    int n_tokens = llama_tokenize(
        llama_context_get_model(ctx_),
        full_prompt.c_str(),
        static_cast<int32_t>(full_prompt.size()),
        tokens.data(),
        max_tokens,
        true,  // add_bos
        false  // special
    );

    if (n_tokens < 0) {
        return Result<std::string, std::string>::error("Tokenization failed");
    }
    tokens.resize(static_cast<size_t>(n_tokens));

    // Clear KV cache
    llama_kv_cache_clear(ctx_);

    // Decode prompt tokens
    llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
    if (llama_decode(ctx_, batch) != 0) {
        return Result<std::string, std::string>::error("Prompt decode failed");
    }

    // Generate response tokens
    std::string response;
    const int max_gen = std::min(512, max_tokens - n_tokens);
    const llama_model* model = llama_context_get_model(ctx_);
    const llama_token eos = llama_token_eos(model);

    for (int i = 0; i < max_gen; ++i) {
        llama_token id = llama_sampler_sample(sampler_, ctx_, -1);

        if (id == eos) break;

        // Convert token to text
        char buf[256];
        int len = llama_token_to_piece(model, id, buf, sizeof(buf), 0, false);
        if (len > 0) {
            response.append(buf, static_cast<size_t>(len));
        }

        // Decode the new token
        llama_batch next = llama_batch_get_one(&id, 1);
        if (llama_decode(ctx_, next) != 0) {
            break;
        }
    }

    llama_sampler_reset(sampler_);

    return Result<std::string, std::string>::ok(std::move(response));
#else
    return Result<std::string, std::string>::ok(rule_based_analysis(prompt));
#endif
}

void AliceEngine::unload() {
    std::lock_guard lock(mutex_);

    if (!loaded_.load()) return;

#ifdef HAVE_LLAMA_CPP
    SL_INFO("alice: unloading model from VRAM");

    if (sampler_) {
        llama_sampler_free(sampler_);
        sampler_ = nullptr;
    }
    if (ctx_) {
        llama_context_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    llama_backend_free();
#endif

    loaded_.store(false);
    SL_INFO("alice: model unloaded");
}

bool AliceEngine::is_loaded() const {
    return loaded_.load();
}

void AliceEngine::start_idle_timer() {
    if (idle_running_.load()) return;

    idle_running_.store(true);
    idle_thread_ = std::thread(&AliceEngine::idle_eviction_loop, this);
}

void AliceEngine::stop_idle_timer() {
    idle_running_.store(false);
    if (idle_thread_.joinable()) {
        idle_thread_.join();
    }
}

void AliceEngine::idle_eviction_loop() {
    while (idle_running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (!loaded_.load()) continue;

        bool should_evict = false;
        {
            std::lock_guard lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_use_).count();
            if (elapsed >= config_.idle_unload_seconds) {
                SL_INFO("alice: idle for {}s, evicting model from VRAM", elapsed);
                should_evict = true;
            }
        }

        if (should_evict) {
            unload();
        }
    }
}

std::string AliceEngine::rule_based_analysis(const std::string& prompt) const {
    std::ostringstream out;
    out << "[Alice — Rule-Based Analysis]\n\n";

    // Parse the prompt for known patterns and generate structured analysis
    bool found_issues = false;

    // Check for error-level log entries
    if (prompt.find("[err]") != std::string::npos ||
        prompt.find("[crit]") != std::string::npos ||
        prompt.find("ERROR") != std::string::npos ||
        prompt.find("CRITICAL") != std::string::npos) {
        out << "WARNING: Error-level messages detected in system logs.\n";
        found_issues = true;
    }

    // Check for thermal keywords
    if (prompt.find("temperature") != std::string::npos ||
        prompt.find("thermal") != std::string::npos ||
        prompt.find("overheating") != std::string::npos) {
        out << "THERMAL: Temperature data present in input. ";

        // Try to find temperature values (simple numeric scan after "temp" or "C")
        auto pos = prompt.find("temp");
        if (pos != std::string::npos) {
            // Look for a number nearby
            auto num_start = prompt.find_first_of("0123456789", pos);
            if (num_start != std::string::npos) {
                auto num_end = prompt.find_first_not_of("0123456789.", num_start);
                std::string val = prompt.substr(num_start, num_end - num_start);
                try {
                    float temp = std::stof(val);
                    if (temp > 90.0f) {
                        out << "CRITICAL: Temperature " << val << " exceeds 90C threshold.\n";
                    } else if (temp > 75.0f) {
                        out << "WARNING: Temperature " << val << " is elevated (>75C).\n";
                    } else {
                        out << "Temperature " << val << " is within normal range.\n";
                    }
                } catch (...) {
                    out << "Could not parse temperature value.\n";
                }
            }
        }
        found_issues = true;
    }

    // Check for memory keywords
    if (prompt.find("MemAvailable") != std::string::npos ||
        prompt.find("memory") != std::string::npos ||
        prompt.find("OOM") != std::string::npos ||
        prompt.find("oom") != std::string::npos) {
        out << "MEMORY: Memory pressure data present.\n";
        if (prompt.find("OOM") != std::string::npos || prompt.find("oom") != std::string::npos) {
            out << "CRITICAL: OOM (out-of-memory) condition detected.\n";
        }
        found_issues = true;
    }

    // Check for disk keywords
    if (prompt.find("disk") != std::string::npos ||
        prompt.find("SMART") != std::string::npos ||
        prompt.find("smart") != std::string::npos ||
        prompt.find("nvme") != std::string::npos) {
        out << "DISK: Storage health data present.\n";
        if (prompt.find("FAILING") != std::string::npos ||
            prompt.find("failed") != std::string::npos) {
            out << "CRITICAL: Disk health failure indicator detected.\n";
        }
        found_issues = true;
    }

    // Check for GPU keywords
    if (prompt.find("gpu") != std::string::npos ||
        prompt.find("GPU") != std::string::npos ||
        prompt.find("drm") != std::string::npos ||
        prompt.find("nvidia") != std::string::npos) {
        out << "GPU: Graphics subsystem data present.\n";
        if (prompt.find("Xid") != std::string::npos ||
            prompt.find("fell off") != std::string::npos) {
            out << "CRITICAL: GPU error (Xid or bus fault) detected.\n";
        }
        found_issues = true;
    }

    // Check for network keywords
    if (prompt.find("network") != std::string::npos ||
        prompt.find("eth") != std::string::npos ||
        prompt.find("wlan") != std::string::npos ||
        prompt.find("link") != std::string::npos) {
        out << "NETWORK: Network status data present.\n";
        if (prompt.find("down") != std::string::npos ||
            prompt.find("carrier lost") != std::string::npos) {
            out << "WARNING: Network link down or carrier lost.\n";
        }
        found_issues = true;
    }

    if (!found_issues) {
        out << "System appears healthy. No anomalies detected in the provided data.\n";
    }

    out << "\nNote: This is a rule-based analysis. For deeper insights, "
           "install a GGUF model and rebuild with HAVE_LLAMA_CPP.\n";

    return out.str();
}

} // namespace straylight
