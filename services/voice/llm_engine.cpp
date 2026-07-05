// services/voice/llm_engine.cpp
// Local LLM engine backed by llama.cpp with CLI fallback.

#include "llm_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#ifdef HAVE_LLAMA_CPP
#include "llama.h"
#endif

namespace straylight::voice {

// ─── Lifecycle ──────────────────────────────────────────────────────────────

LlmEngine::~LlmEngine() {
#ifdef HAVE_LLAMA_CPP
    if (llama_ctx_) {
        llama_free(static_cast<llama_context*>(llama_ctx_));
        llama_ctx_ = nullptr;
    }
    if (llama_model_) {
        llama_model_free(static_cast<llama_model*>(llama_model_));
        llama_model_ = nullptr;
    }
#endif
}

Result<void, std::string> LlmEngine::init(const VoiceConfig& cfg) {
    model_path_    = cfg.llm_model_path;
    system_prompt_ = cfg.system_prompt;
    params_.temperature = cfg.llm_temperature;
    params_.max_tokens  = cfg.llm_max_tokens;

    namespace fs = std::filesystem;
    if (!fs::exists(model_path_)) {
        fprintf(stderr, "[voice:llm] model not found: %s\n", model_path_.c_str());
        fprintf(stderr, "[voice:llm] will use CLI fallback (llama-cli / ollama)\n");
        return Result<void, std::string>::ok();
    }

    auto file_size = fs::file_size(model_path_);
    fprintf(stdout, "[voice:llm] loading model %s (%.1f MB)...\n",
            model_path_.c_str(), static_cast<double>(file_size) / (1024.0 * 1024.0));

#ifdef HAVE_LLAMA_CPP
    llama_backend_init();

    // Load model.
    auto model_params = llama_model_default_params();
    model_params.n_gpu_layers = 99;  // offload all layers to GPU if available

    auto* model = llama_model_load_from_file(model_path_.c_str(), model_params);
    if (!model) {
        return Result<void, std::string>::error(
            "llama_model_load_from_file failed: " + model_path_);
    }
    llama_model_ = model;

    // Create context.
    auto ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 4096;
    ctx_params.n_batch = 512;
    ctx_params.n_threads = 4;

    auto* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        llama_model_free(model);
        llama_model_ = nullptr;
        return Result<void, std::string>::error("llama context creation failed");
    }
    llama_ctx_ = ctx;

    model_loaded_ = true;
    fprintf(stdout, "[voice:llm] model loaded successfully (ctx=4096)\n");
#else
    model_loaded_ = false;
    fprintf(stdout, "[voice:llm] native llama.cpp not linked, CLI fallback active\n");
#endif

    return Result<void, std::string>::ok();
}

// ─── System context ─────────────────────────────────────────────────────────

std::string LlmEngine::get_system_context() const {
    return read_system_stats();
}

std::string LlmEngine::read_system_stats() const {
    std::ostringstream ctx;
    ctx << "[System State]\n";

    // CPU usage from /proc/stat (Linux only).
    std::ifstream stat_file("/proc/stat");
    if (stat_file.is_open()) {
        std::string line;
        if (std::getline(stat_file, line)) {
            // Parse first "cpu" line: user nice system idle iowait irq softirq
            unsigned long long user, nice, system, idle, iowait, irq, softirq;
            if (sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &idle, &iowait, &irq, &softirq) == 7) {
                unsigned long long total = user + nice + system + idle + iowait + irq + softirq;
                unsigned long long active = total - idle - iowait;
                double cpu_pct = (total > 0) ? (100.0 * active / total) : 0.0;
                ctx << "CPU: " << static_cast<int>(cpu_pct) << "% used\n";
            }
        }
        stat_file.close();
    }

    // Memory from /proc/meminfo.
    std::ifstream mem_file("/proc/meminfo");
    if (mem_file.is_open()) {
        unsigned long total_kb = 0, avail_kb = 0;
        std::string line;
        while (std::getline(mem_file, line)) {
            if (line.find("MemTotal:") == 0) {
                sscanf(line.c_str(), "MemTotal: %lu kB", &total_kb);
            } else if (line.find("MemAvailable:") == 0) {
                sscanf(line.c_str(), "MemAvailable: %lu kB", &avail_kb);
            }
        }
        mem_file.close();
        if (total_kb > 0) {
            unsigned long used_mb = (total_kb - avail_kb) / 1024;
            unsigned long total_mb = total_kb / 1024;
            ctx << "Memory: " << used_mb << "/" << total_mb << " MB used\n";
        }
    }

    // Uptime.
    std::ifstream uptime_file("/proc/uptime");
    if (uptime_file.is_open()) {
        double uptime_sec = 0;
        uptime_file >> uptime_sec;
        uptime_file.close();
        int hours = static_cast<int>(uptime_sec / 3600);
        int mins = static_cast<int>((uptime_sec - hours * 3600) / 60);
        ctx << "Uptime: " << hours << "h " << mins << "m\n";
    }

    // Load average.
    std::ifstream loadavg_file("/proc/loadavg");
    if (loadavg_file.is_open()) {
        std::string load;
        std::getline(loadavg_file, load);
        loadavg_file.close();
        // Take first three fields.
        auto space3 = load.find(' ');
        if (space3 != std::string::npos) {
            space3 = load.find(' ', space3 + 1);
            if (space3 != std::string::npos) {
                space3 = load.find(' ', space3 + 1);
                if (space3 != std::string::npos) {
                    load = load.substr(0, space3);
                }
            }
        }
        ctx << "Load: " << load << "\n";
    }

    // Disk usage of root.
    FILE* df_pipe = popen("df -h / 2>/dev/null | tail -1", "r");
    if (df_pipe) {
        char buf[256];
        if (fgets(buf, sizeof(buf), df_pipe)) {
            // Parse: filesystem size used avail use% mount
            std::istringstream ss(buf);
            std::string fs, size, used, avail, pct;
            ss >> fs >> size >> used >> avail >> pct;
            ctx << "Disk /: " << used << " used / " << size << " total (" << pct << ")\n";
        }
        pclose(df_pipe);
    }

    return ctx.str();
}

// ─── Prompt building ────────────────────────────────────────────────────────

std::string LlmEngine::build_prompt(
    const std::string& user_message,
    const Conversation& conversation,
    const ToolExecutor& tools) const
{
    std::ostringstream prompt;

    // System prompt with tools and context.
    prompt << "<|system|>\n";
    prompt << system_prompt_ << "\n\n";
    prompt << get_system_context() << "\n";
    prompt << tools.tools_prompt() << "\n";

    // Conversation history.
    prompt << conversation.to_prompt_text();

    // Current user message.
    prompt << "<|user|>\n" << user_message << "\n";
    prompt << "<|assistant|>\n";

    return prompt.str();
}

std::string LlmEngine::build_tool_result_prompt(
    const std::string& original_message,
    const Conversation& conversation,
    const std::vector<LlmToolCall>& calls,
    const std::vector<std::string>& results) const
{
    std::ostringstream prompt;

    // System prompt.
    prompt << "<|system|>\n";
    prompt << system_prompt_ << "\n\n";
    prompt << get_system_context() << "\n";

    // Conversation history.
    prompt << conversation.to_prompt_text();

    // Original user message.
    prompt << "<|user|>\n" << original_message << "\n";

    // Tool calls and results.
    prompt << "<|assistant|>\n";
    for (size_t i = 0; i < calls.size(); ++i) {
        prompt << "[Called tool: " << calls[i].tool_name << "]\n";
        if (i < results.size()) {
            prompt << "<|tool|> [" << calls[i].tool_name << "]\n";
            // Truncate very long results.
            std::string r = results[i];
            if (r.size() > 2000) {
                r = r.substr(0, 2000) + "\n... (truncated)";
            }
            prompt << r << "\n";
        }
    }

    prompt << "<|assistant|>\n"
           << "Based on the tool results above, here is my response:\n";

    return prompt.str();
}

// ─── Tool call parsing ──────────────────────────────────────────────────────

std::vector<LlmToolCall> LlmEngine::parse_tool_calls(const std::string& output) const {
    std::vector<LlmToolCall> calls;

    // Look for JSON objects with "tool" key.
    // Pattern: {"tool": "name", "args": {...}}
    size_t pos = 0;
    while (pos < output.size()) {
        auto obj_start = output.find("{\"tool\"", pos);
        if (obj_start == std::string::npos) {
            // Also check for {"tool" with no prefix.
            obj_start = output.find("{ \"tool\"", pos);
            if (obj_start == std::string::npos) break;
        }

        // Find the matching closing brace.
        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < output.size(); ++i) {
            if (output[i] == '{') ++depth;
            if (output[i] == '}') {
                --depth;
                if (depth == 0) { obj_end = i; break; }
            }
        }

        if (depth != 0) break; // malformed

        std::string obj = output.substr(obj_start, obj_end - obj_start + 1);

        LlmToolCall call;

        // Extract tool name.
        auto tool_key = obj.find("\"tool\"");
        if (tool_key != std::string::npos) {
            auto colon = obj.find(':', tool_key);
            if (colon != std::string::npos) {
                auto quote1 = obj.find('"', colon + 1);
                if (quote1 != std::string::npos) {
                    auto quote2 = obj.find('"', quote1 + 1);
                    if (quote2 != std::string::npos) {
                        call.tool_name = obj.substr(quote1 + 1, quote2 - quote1 - 1);
                    }
                }
            }
        }

        // Extract args object.
        auto args_key = obj.find("\"args\"");
        if (args_key != std::string::npos) {
            auto colon = obj.find(':', args_key);
            if (colon != std::string::npos) {
                auto brace = obj.find('{', colon + 1);
                if (brace != std::string::npos) {
                    int d = 0;
                    size_t end = brace;
                    for (size_t i = brace; i < obj.size(); ++i) {
                        if (obj[i] == '{') ++d;
                        if (obj[i] == '}') {
                            --d;
                            if (d == 0) { end = i; break; }
                        }
                    }
                    call.arguments_json = obj.substr(brace, end - brace + 1);
                }
            }
        }

        if (!call.tool_name.empty()) {
            calls.push_back(std::move(call));
        }

        pos = obj_end + 1;
    }

    return calls;
}

// ─── Generation ─────────────────────────────────────────────────────────────

Result<LlmResponse, std::string> LlmEngine::generate(
    const std::string& user_message,
    Conversation& conversation,
    ToolExecutor& tools)
{
    auto t0 = std::chrono::steady_clock::now();

    // Build prompt.
    std::string prompt = build_prompt(user_message, conversation, tools);

    // Run inference.
    auto result = run_inference(prompt);
    if (!result.has_value()) {
        return Result<LlmResponse, std::string>::error(result.error());
    }

    std::string raw_output = result.value();
    LlmResponse response;

    // Check for tool calls in the output.
    auto tool_calls = parse_tool_calls(raw_output);

    if (!tool_calls.empty()) {
        response.tool_calls = tool_calls;

        // Execute each tool.
        for (const auto& call : tool_calls) {
            auto exec_result = tools.execute(call.tool_name, call.arguments_json);
            std::string tool_output;
            if (exec_result.has_value()) {
                const auto& tr = exec_result.value();
                if (tr.success) {
                    tool_output = tr.output;
                } else {
                    tool_output = "Error: " + tr.error;
                }
            } else {
                tool_output = "Error: " + exec_result.error();
            }
            response.tool_results.push_back(tool_output);

            // Add to conversation.
            conversation.add_tool_result(call.tool_name, tool_output);
        }

        // Re-prompt with tool results for a final natural language response.
        std::string followup_prompt = build_tool_result_prompt(
            user_message, conversation, tool_calls, response.tool_results);

        auto followup_result = run_inference(followup_prompt);
        if (followup_result.has_value()) {
            response.text = followup_result.value();
        } else {
            // If follow-up fails, summarize tool results directly.
            std::ostringstream summary;
            for (size_t i = 0; i < tool_calls.size(); ++i) {
                summary << tool_calls[i].tool_name << ": ";
                if (i < response.tool_results.size()) {
                    // Take first line of result.
                    std::string r = response.tool_results[i];
                    auto nl = r.find('\n');
                    if (nl != std::string::npos) r = r.substr(0, nl);
                    summary << r;
                }
                if (i + 1 < tool_calls.size()) summary << ". ";
            }
            response.text = summary.str();
        }
    } else {
        // No tool calls — raw_output is the response text.
        response.text = raw_output;
    }

    // Clean up response text.
    // Remove any trailing <|...> tokens.
    auto end_token = response.text.find("<|");
    if (end_token != std::string::npos) {
        response.text = response.text.substr(0, end_token);
    }

    // Trim whitespace.
    auto start = response.text.find_first_not_of(" \t\n\r");
    auto end   = response.text.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
        response.text = response.text.substr(start, end - start + 1);
    }

    auto t1 = std::chrono::steady_clock::now();
    response.inference_time_s = std::chrono::duration<double>(t1 - t0).count();

    return Result<LlmResponse, std::string>::ok(std::move(response));
}

Result<std::string, std::string> LlmEngine::complete(const std::string& prompt) {
    return run_inference(prompt);
}

// ─── Inference (native or fallback) ─────────────────────────────────────────

Result<std::string, std::string> LlmEngine::run_inference(const std::string& prompt) {
#ifdef HAVE_LLAMA_CPP
    if (model_loaded_ && llama_ctx_ && llama_model_) {
        auto* ctx = static_cast<llama_context*>(llama_ctx_);
        auto* model = static_cast<llama_model*>(llama_model_);

        // Tokenize prompt.
        int n_prompt_tokens = prompt.size() + 256;
        std::vector<llama_token> tokens(n_prompt_tokens);

        int n = llama_tokenize(model, prompt.c_str(), prompt.size(),
                               tokens.data(), tokens.size(), true, false);
        if (n < 0) {
            tokens.resize(-n);
            n = llama_tokenize(model, prompt.c_str(), prompt.size(),
                               tokens.data(), tokens.size(), true, false);
        }
        tokens.resize(n);

        // Clear KV cache.
        llama_kv_cache_clear(ctx);

        // Evaluate prompt tokens.
        llama_batch batch = llama_batch_init(512, 0, 1);
        for (int i = 0; i < n; ++i) {
            llama_batch_add(batch, tokens[i], i, {0}, false);
        }
        batch.logits[batch.n_tokens - 1] = true;

        if (llama_decode(ctx, batch) != 0) {
            llama_batch_free(batch);
            return fallback_inference(prompt);
        }

        // Sample tokens.
        std::string output;
        int n_generated = 0;
        llama_token eos = llama_token_eos(model);

        while (n_generated < params_.max_tokens) {
            auto* logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
            int n_vocab = llama_n_vocab(model);

            // Simple temperature sampling.
            std::vector<llama_token_data> candidates(n_vocab);
            for (int i = 0; i < n_vocab; ++i) {
                candidates[i] = {static_cast<llama_token>(i), logits[i], 0.0f};
            }

            llama_token_data_array candidates_arr = {
                candidates.data(),
                static_cast<size_t>(n_vocab),
                false
            };

            llama_sample_temp(ctx, &candidates_arr, params_.temperature);
            llama_sample_top_p(ctx, &candidates_arr, params_.top_p, 1);
            llama_token new_token = llama_sample_token(ctx, &candidates_arr);

            if (new_token == eos) break;

            // Convert token to text.
            char buf[256];
            int len = llama_token_to_piece(model, new_token, buf, sizeof(buf), 0, false);
            if (len > 0) {
                output.append(buf, len);
            }

            // Prepare next batch.
            llama_batch_clear(batch);
            llama_batch_add(batch, new_token, n + n_generated, {0}, true);

            if (llama_decode(ctx, batch) != 0) break;

            ++n_generated;
        }

        llama_batch_free(batch);

        return Result<std::string, std::string>::ok(std::move(output));
    }
#endif

    return fallback_inference(prompt);
}

Result<std::string, std::string> LlmEngine::fallback_inference(const std::string& prompt) {
    // Write prompt to temp file.
    std::string prompt_file = "/tmp/straylight-llm-prompt-" + std::to_string(getpid()) + ".txt";
    {
        std::ofstream out(prompt_file);
        if (!out.is_open()) {
            return Result<std::string, std::string>::error("cannot write prompt file");
        }
        out << prompt;
        out.close();
    }

    // Try different CLI backends in order of preference.
    std::string output;
    bool success = false;

    // 1. llama-cli (llama.cpp CLI)
    if (!success && std::system("which llama-cli >/dev/null 2>&1") == 0 &&
        std::filesystem::exists(model_path_)) {
        std::string cmd = "llama-cli -m " + model_path_ +
                          " -n " + std::to_string(params_.max_tokens) +
                          " --temp " + std::to_string(params_.temperature) +
                          " -f " + prompt_file +
                          " --no-display-prompt 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            std::array<char, 4096> buf{};
            while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
                output += buf.data();
            }
            int status = pclose(pipe);
            success = (status == 0 && !output.empty());
        }
    }

    // 2. ollama
    if (!success && std::system("which ollama >/dev/null 2>&1") == 0) {
        // Read the prompt back and pipe to ollama.
        std::string cmd = "ollama run qwen2:0.5b < " + prompt_file + " 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe) {
            std::array<char, 4096> buf{};
            while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
                output += buf.data();
            }
            int status = pclose(pipe);
            success = (status == 0 && !output.empty());
        }
    }

    // 3. straylight-alice (the built-in Alice LLM daemon)
    if (!success) {
        // Try IPC to Alice.
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, "/run/straylight/alice.sock",
                    sizeof(addr.sun_path) - 1);

            if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(addr)) == 0) {
                // Build JSON-RPC request.
                // Escape the prompt for JSON.
                std::string escaped;
                for (char c : prompt) {
                    switch (c) {
                        case '"':  escaped += "\\\""; break;
                        case '\\': escaped += "\\\\"; break;
                        case '\n': escaped += "\\n"; break;
                        case '\r': escaped += "\\r"; break;
                        case '\t': escaped += "\\t"; break;
                        default:   escaped += c;
                    }
                }

                std::string request = "{\"jsonrpc\":\"2.0\",\"method\":\"alice.generate\","
                                      "\"params\":{\"prompt\":\"" + escaped + "\","
                                      "\"max_tokens\":" + std::to_string(params_.max_tokens) +
                                      ",\"temperature\":" + std::to_string(params_.temperature) +
                                      "},\"id\":1}\n";

                write(fd, request.c_str(), request.size());

                // Set read timeout.
                struct timeval tv;
                tv.tv_sec = 30;
                tv.tv_usec = 0;
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                char buf[32768];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string response(buf, n);

                    // Extract the "text" field from the JSON-RPC result.
                    auto text_pos = response.find("\"text\"");
                    if (text_pos != std::string::npos) {
                        auto colon = response.find(':', text_pos);
                        if (colon != std::string::npos) {
                            auto q1 = response.find('"', colon + 1);
                            if (q1 != std::string::npos) {
                                ++q1;
                                std::string text;
                                while (q1 < response.size() && response[q1] != '"') {
                                    if (response[q1] == '\\' && q1 + 1 < response.size()) {
                                        ++q1;
                                        switch (response[q1]) {
                                            case 'n': text += '\n'; break;
                                            case 't': text += '\t'; break;
                                            case '"': text += '"'; break;
                                            case '\\': text += '\\'; break;
                                            default: text += response[q1]; break;
                                        }
                                    } else {
                                        text += response[q1];
                                    }
                                    ++q1;
                                }
                                output = text;
                                success = true;
                            }
                        }
                    }
                }
            }
            close(fd);
        }
    }

    // Clean up.
    std::remove(prompt_file.c_str());

    if (!success || output.empty()) {
        return Result<std::string, std::string>::error(
            "no LLM backend available: install llama.cpp, ollama, or ensure "
            "straylight-alice is running on /run/straylight/alice.sock");
    }

    // Trim.
    auto start = output.find_first_not_of(" \t\n\r");
    auto end   = output.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
        output = output.substr(start, end - start + 1);
    }

    return Result<std::string, std::string>::ok(std::move(output));
}

} // namespace straylight::voice
