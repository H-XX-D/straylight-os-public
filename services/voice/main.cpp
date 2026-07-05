/**
 * StrayLight Voice Daemon — Full voice-interactive AI assistant.
 *
 * Manages the complete voice pipeline:
 *   Audio Capture -> STT -> LLM -> Tool Execution -> TTS -> Audio Playback
 *
 * Operates in two modes:
 *   - Wake word ("hey straylight"): always listening, activates on trigger
 *   - Push-to-talk: activated via IPC command or hotkey
 *
 * Listens on /run/straylight/voice.sock for JSON-RPC control:
 *   voice.status       — Get daemon status (loaded models, active state)
 *   voice.ask          — Text query (no mic): LLM + tools, return text
 *   voice.say          — Text-to-speech: synthesize and play text
 *   voice.talk         — Start interactive voice mode
 *   voice.stop         — Stop current interaction
 *   voice.listen       — STT-only mode: mic -> text
 *   voice.transcribe   — Transcribe raw PCM audio data
 *   voice.ptt_start    — Begin push-to-talk recording
 *   voice.ptt_stop     — End push-to-talk, process utterance
 *   voice.models       — List available/installed models
 *   voice.config       — Get current configuration
 *   voice.history      — Get conversation history
 *   voice.clear        — Clear conversation history
 */

#include "audio_capture.h"
#include "conversation.h"
#include "llm_engine.h"
#include "stt_engine.h"
#include "tool_executor.h"
#include "tts_engine.h"
#include "voice_config.h"
#include "straylight/daemon_base.h"
#include "straylight/result.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace straylight;
using namespace straylight::voice;

static constexpr const char* SOCKET_PATH   = "/run/straylight/voice.sock";
static constexpr const char* CONFIG_PATH   = "/etc/straylight/voice.conf";
static constexpr int MAX_CLIENTS           = 16;
static constexpr int READ_BUF_SIZE         = 16384;

// ─── JSON helpers ───────────────────────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
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
                case '"':  result += '"'; break;
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

static std::string extract_params_object(const std::string& raw) {
    auto params_pos = raw.find("\"params\"");
    if (params_pos == std::string::npos) return "{}";
    auto brace = raw.find('{', params_pos + 8);
    if (brace == std::string::npos) return "{}";
    int depth = 0;
    size_t end = brace;
    for (size_t i = brace; i < raw.size(); ++i) {
        if (raw[i] == '{') ++depth;
        if (raw[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
    }
    return raw.substr(brace, end - brace + 1);
}

static std::string rpc_error(int id, int code, const std::string& msg) {
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":" << code
        << ",\"message\":\"" << json_escape(msg) << "\"},\"id\":" << id << "}\n";
    return out.str();
}

static std::string rpc_result(int id, const std::string& result_json) {
    std::ostringstream out;
    out << "{\"jsonrpc\":\"2.0\",\"result\":" << result_json
        << ",\"id\":" << id << "}\n";
    return out.str();
}

// ─── Voice pipeline states ──────────────────────────────────────────────────

enum class PipelineState {
    Idle,            // Waiting for wake word or PTT
    WakeListening,   // Always-on, listening for wake word
    Recording,       // Actively recording speech (after wake or PTT)
    Transcribing,    // Running STT
    Thinking,        // Running LLM
    Speaking,        // Playing TTS output
};

static const char* state_str(PipelineState s) {
    switch (s) {
        case PipelineState::Idle:          return "idle";
        case PipelineState::WakeListening: return "wake_listening";
        case PipelineState::Recording:     return "recording";
        case PipelineState::Transcribing:  return "transcribing";
        case PipelineState::Thinking:      return "thinking";
        case PipelineState::Speaking:      return "speaking";
    }
    return "unknown";
}

// ─── Voice Daemon ───────────────────────────────────────────────────────────

class VoiceDaemon : public DaemonBase {
public:
    VoiceDaemon() : DaemonBase("straylight-voice") {
        set_tick_interval_ms(50); // fast ticks for responsive IPC
    }

protected:
    VoidResult<> init() override {
        // Load configuration.
        auto cfg_result = VoiceConfig::load(CONFIG_PATH);
        if (cfg_result.has_value()) {
            config_ = cfg_result.value();
            fprintf(stdout, "[straylight-voice] loaded config from %s\n", CONFIG_PATH);
        } else {
            fprintf(stderr, "[straylight-voice] config load: %s (using defaults)\n",
                    cfg_result.error().c_str());
        }

        // Initialize subsystems.
        fprintf(stdout, "[straylight-voice] initializing audio capture...\n");
        auto audio_res = audio_.init(config_);
        if (!audio_res.has_value()) {
            fprintf(stderr, "[straylight-voice] audio init warning: %s\n",
                    audio_res.error().c_str());
        }

        fprintf(stdout, "[straylight-voice] initializing STT engine...\n");
        auto stt_res = stt_.init(config_);
        if (!stt_res.has_value()) {
            fprintf(stderr, "[straylight-voice] STT init warning: %s\n",
                    stt_res.error().c_str());
        }

        fprintf(stdout, "[straylight-voice] initializing TTS engine...\n");
        auto tts_res = tts_.init(config_);
        if (!tts_res.has_value()) {
            fprintf(stderr, "[straylight-voice] TTS init warning: %s\n",
                    tts_res.error().c_str());
        }

        fprintf(stdout, "[straylight-voice] initializing LLM engine...\n");
        auto llm_res = llm_.init(config_);
        if (!llm_res.has_value()) {
            fprintf(stderr, "[straylight-voice] LLM init warning: %s\n",
                    llm_res.error().c_str());
        }

        fprintf(stdout, "[straylight-voice] initializing tool executor...\n");
        auto tool_res = tools_.init(config_);
        if (!tool_res.has_value()) {
            fprintf(stderr, "[straylight-voice] tools init warning: %s\n",
                    tool_res.error().c_str());
        }

        // Set up conversation.
        conversation_.set_max_turns(config_.context_window);

        // Set up tool confirmation callback (TTS-based).
        tools_.set_confirm_callback([this](const std::string& desc) -> bool {
            // Speak the confirmation request.
            tts_.speak("This action requires confirmation: " + desc +
                       ". Please say yes or no.");

            // Record user response.
            auto ptt_res = audio_.start_push_to_talk();
            if (!ptt_res.has_value()) return false;

            // Wait for speech (up to 10 seconds).
            std::this_thread::sleep_for(std::chrono::seconds(5));

            auto buf_res = audio_.stop_push_to_talk();
            if (!buf_res.has_value()) return false;

            // Transcribe.
            auto trans_res = stt_.transcribe(buf_res.value());
            if (!trans_res.has_value()) return false;

            std::string answer = trans_res.value().text;
            // Lowercase.
            std::transform(answer.begin(), answer.end(), answer.begin(), ::tolower);

            return (answer.find("yes") != std::string::npos ||
                    answer.find("confirm") != std::string::npos ||
                    answer.find("go ahead") != std::string::npos ||
                    answer.find("do it") != std::string::npos);
        });

        // Create Unix socket.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(SOCKET_PATH).parent_path(), ec);
        ::unlink(SOCKET_PATH);

        listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            return VoidResult<>::error("socket() failed: " +
                                       std::string(strerror(errno)));
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(listen_fd_);
            return VoidResult<>::error("bind() failed: " +
                                       std::string(strerror(errno)));
        }

        chmod(SOCKET_PATH, 0666);

        if (listen(listen_fd_, 4) < 0) {
            close(listen_fd_);
            return VoidResult<>::error("listen() failed: " +
                                       std::string(strerror(errno)));
        }

        // Start wake-word listening if not push-to-talk mode.
        if (!config_.push_to_talk) {
            state_ = PipelineState::WakeListening;
            auto start_res = audio_.start_capture(
                [this](AudioBuffer buf) { on_speech_captured(std::move(buf)); });
            if (!start_res.has_value()) {
                fprintf(stderr, "[straylight-voice] wake capture failed: %s\n",
                        start_res.error().c_str());
                state_ = PipelineState::Idle;
            }
        }

        fprintf(stdout, "[straylight-voice] listening on %s\n", SOCKET_PATH);
        fprintf(stdout, "[straylight-voice] mode=%s wake_word=\"%s\"\n",
                config_.push_to_talk ? "push-to-talk" : "wake-word",
                config_.wake_word.c_str());
        fprintf(stdout, "[straylight-voice] STT=%s TTS=%s LLM=%s tools=%zu\n",
                stt_.is_loaded() ? "native" : "fallback",
                tts_.is_loaded() ? "piper" : "espeak",
                llm_.is_loaded() ? "native" : "fallback",
                tools_.tools().size());
        fprintf(stdout, "[straylight-voice] ready\n");

        return VoidResult<>::ok();
    }

    void tick() override {
        handle_clients();

        // Process any pending speech in the pipeline.
        process_pipeline();
    }

    void shutdown() override {
        audio_.stop_capture();
        tts_.stop_playback();

        for (int fd : client_fds_) close(fd);
        client_fds_.clear();

        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        ::unlink(SOCKET_PATH);

        // Save conversation history.
        conversation_.save("/var/lib/straylight/voice-history.json");

        fprintf(stdout, "[straylight-voice] shutdown complete\n");
    }

    void on_reload() override {
        auto cfg_result = VoiceConfig::load(CONFIG_PATH);
        if (cfg_result.has_value()) {
            config_ = cfg_result.value();
            fprintf(stdout, "[straylight-voice] config reloaded\n");
        }
    }

private:
    VoiceConfig   config_;
    AudioCapture  audio_;
    SttEngine     stt_;
    TtsEngine     tts_;
    LlmEngine     llm_;
    ToolExecutor  tools_;
    Conversation  conversation_{10};

    PipelineState state_ = PipelineState::Idle;
    int listen_fd_ = -1;
    std::vector<int> client_fds_;

    // Pending speech buffer from wake-word capture.
    std::mutex speech_mutex_;
    std::vector<AudioBuffer> pending_speech_;

    // Stats.
    int total_queries_    = 0;
    int total_tool_calls_ = 0;
    double total_audio_s_ = 0.0;

    // ─── Speech capture callback ────────────────────────────────────────

    void on_speech_captured(AudioBuffer buf) {
        std::lock_guard<std::mutex> lock(speech_mutex_);
        pending_speech_.push_back(std::move(buf));
    }

    // ─── Pipeline processing ────────────────────────────────────────────

    void process_pipeline() {
        // Check for pending speech buffers.
        std::vector<AudioBuffer> to_process;
        {
            std::lock_guard<std::mutex> lock(speech_mutex_);
            to_process.swap(pending_speech_);
        }

        for (auto& buf : to_process) {
            process_utterance(std::move(buf));
        }
    }

    void process_utterance(AudioBuffer audio) {
        total_audio_s_ += audio.duration_seconds();

        // 1. Transcribe.
        state_ = PipelineState::Transcribing;
        auto trans_result = stt_.transcribe(audio);
        if (!trans_result.has_value()) {
            fprintf(stderr, "[straylight-voice] STT failed: %s\n",
                    trans_result.error().c_str());
            state_ = config_.push_to_talk ? PipelineState::Idle
                                          : PipelineState::WakeListening;
            return;
        }

        std::string text = trans_result.value().text;
        if (text.empty()) {
            state_ = config_.push_to_talk ? PipelineState::Idle
                                          : PipelineState::WakeListening;
            return;
        }

        fprintf(stdout, "[straylight-voice] heard: \"%s\" (%.1fs, conf=%.2f)\n",
                text.c_str(), trans_result.value().audio_duration_s,
                trans_result.value().confidence);

        // 2. Check for wake word (if in wake-listening mode).
        if (state_ == PipelineState::WakeListening ||
            state_ == PipelineState::Transcribing) {

            std::string lower_text = text;
            std::transform(lower_text.begin(), lower_text.end(),
                           lower_text.begin(), ::tolower);

            // Check if this is a wake word trigger.
            std::string lower_wake = config_.wake_word;
            std::transform(lower_wake.begin(), lower_wake.end(),
                           lower_wake.begin(), ::tolower);

            auto wake_pos = lower_text.find(lower_wake);
            if (wake_pos != std::string::npos && !config_.push_to_talk) {
                // Extract the command after the wake word.
                text = text.substr(wake_pos + config_.wake_word.size());
                // Trim.
                auto start = text.find_first_not_of(" \t,.");
                if (start != std::string::npos) {
                    text = text.substr(start);
                } else {
                    // Wake word only, no command. Play acknowledgment.
                    tts_.speak("Yes?");
                    state_ = PipelineState::WakeListening;
                    return;
                }
            } else if (!config_.push_to_talk) {
                // No wake word detected, ignore.
                state_ = PipelineState::WakeListening;
                return;
            }
        }

        // 3. Process with LLM.
        process_text_query(text, audio.duration_seconds());
    }

    void process_text_query(const std::string& text, double audio_dur = 0.0) {
        ++total_queries_;
        state_ = PipelineState::Thinking;

        conversation_.add_user(text, audio_dur);

        auto llm_result = llm_.generate(text, conversation_, tools_);
        if (!llm_result.has_value()) {
            fprintf(stderr, "[straylight-voice] LLM failed: %s\n",
                    llm_result.error().c_str());
            tts_.speak("I'm sorry, I had trouble processing that request.");
            state_ = config_.push_to_talk ? PipelineState::Idle
                                          : PipelineState::WakeListening;
            return;
        }

        const auto& response = llm_result.value();
        total_tool_calls_ += static_cast<int>(response.tool_calls.size());

        conversation_.add_assistant(response.text);

        fprintf(stdout, "[straylight-voice] response: \"%s\" (%.1fs, %zu tools)\n",
                response.text.substr(0, 100).c_str(),
                response.inference_time_s,
                response.tool_calls.size());

        // 4. Speak the response.
        state_ = PipelineState::Speaking;
        auto speak_result = tts_.speak(response.text);
        if (!speak_result.has_value()) {
            fprintf(stderr, "[straylight-voice] TTS failed: %s\n",
                    speak_result.error().c_str());
        }

        state_ = config_.push_to_talk ? PipelineState::Idle
                                      : PipelineState::WakeListening;
    }

    // ─── Client handling ────────────────────────────────────────────────

    void handle_clients() {
        // Accept new connections.
        while (true) {
            int client_fd = accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) break;
            if (client_fds_.size() >= MAX_CLIENTS) {
                close(client_fd);
                continue;
            }
            client_fds_.push_back(client_fd);
        }

        if (client_fds_.empty()) return;

        std::vector<struct pollfd> pfds;
        for (int fd : client_fds_) {
            pfds.push_back({fd, POLLIN, 0});
        }

        int ready = poll(pfds.data(), pfds.size(), 5);
        if (ready <= 0) return;

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
            write(pfds[i].fd, response.c_str(), response.size());
        }

        for (int fd : to_remove) {
            close(fd);
            client_fds_.erase(
                std::remove(client_fds_.begin(), client_fds_.end(), fd),
                client_fds_.end());
        }
    }

    std::string handle_request(const std::string& raw) {
        std::string method = extract_json_string(raw, "method");
        int id = extract_json_int(raw, "id");

        if (method == "voice.status")       return handle_status(id);
        if (method == "voice.ask")          return handle_ask(raw, id);
        if (method == "voice.say")          return handle_say(raw, id);
        if (method == "voice.talk")         return handle_talk(id);
        if (method == "voice.stop")         return handle_stop(id);
        if (method == "voice.listen")       return handle_listen(id);
        if (method == "voice.transcribe")   return handle_transcribe(raw, id);
        if (method == "voice.ptt_start")    return handle_ptt_start(id);
        if (method == "voice.ptt_stop")     return handle_ptt_stop(id);
        if (method == "voice.models")       return handle_models(id);
        if (method == "voice.config")       return handle_config(id);
        if (method == "voice.history")      return handle_history(id);
        if (method == "voice.clear")        return handle_clear(id);

        return rpc_error(id, -32601, "method not found: " + method);
    }

    // ─── RPC handlers ───────────────────────────────────────────────────

    std::string handle_status(int id) {
        std::ostringstream out;
        out << "{";
        out << "\"state\": \"" << state_str(state_) << "\", ";
        out << "\"stt_loaded\": " << (stt_.is_loaded() ? "true" : "false") << ", ";
        out << "\"stt_model\": \"" << json_escape(stt_.model_path()) << "\", ";
        out << "\"tts_loaded\": " << (tts_.is_loaded() ? "true" : "false") << ", ";
        out << "\"tts_voice\": \"" << json_escape(tts_.current_voice()) << "\", ";
        out << "\"llm_loaded\": " << (llm_.is_loaded() ? "true" : "false") << ", ";
        out << "\"llm_model\": \"" << json_escape(llm_.model_path()) << "\", ";
        out << "\"tools_count\": " << tools_.tools().size() << ", ";
        out << "\"mode\": \"" << (config_.push_to_talk ? "push_to_talk" : "wake_word") << "\", ";
        out << "\"wake_word\": \"" << json_escape(config_.wake_word) << "\", ";
        out << "\"conversation_turns\": " << conversation_.size() << ", ";
        out << "\"total_queries\": " << total_queries_ << ", ";
        out << "\"total_tool_calls\": " << total_tool_calls_ << ", ";
        out << "\"total_audio_seconds\": " << total_audio_s_ << ", ";
        out << "\"capturing\": " << (audio_.is_capturing() ? "true" : "false");
        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_ask(const std::string& raw, int id) {
        std::string params = extract_params_object(raw);
        std::string text = extract_json_string(params, "text");

        if (text.empty()) {
            return rpc_error(id, -32602, "missing 'text' in params");
        }

        // Process text query synchronously (no mic, no TTS).
        ++total_queries_;
        conversation_.add_user(text);

        auto llm_result = llm_.generate(text, conversation_, tools_);
        if (!llm_result.has_value()) {
            return rpc_error(id, -32000, "LLM failed: " + llm_result.error());
        }

        const auto& response = llm_result.value();
        total_tool_calls_ += static_cast<int>(response.tool_calls.size());
        conversation_.add_assistant(response.text);

        std::ostringstream out;
        out << "{";
        out << "\"text\": \"" << json_escape(response.text) << "\", ";
        out << "\"inference_time_s\": " << response.inference_time_s << ", ";
        out << "\"tools_used\": [";
        for (size_t i = 0; i < response.tool_calls.size(); ++i) {
            if (i > 0) out << ", ";
            out << "{\"tool\": \"" << json_escape(response.tool_calls[i].tool_name)
                << "\", \"result\": \"";
            if (i < response.tool_results.size()) {
                std::string r = response.tool_results[i];
                if (r.size() > 500) r = r.substr(0, 500) + "...";
                out << json_escape(r);
            }
            out << "\"}";
        }
        out << "], ";
        out << "\"tokens_generated\": " << response.tokens_generated;
        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_say(const std::string& raw, int id) {
        std::string params = extract_params_object(raw);
        std::string text = extract_json_string(params, "text");

        if (text.empty()) {
            return rpc_error(id, -32602, "missing 'text' in params");
        }

        auto result = tts_.speak(text);
        if (!result.has_value()) {
            return rpc_error(id, -32000, "TTS failed: " + result.error());
        }

        return rpc_result(id, "{\"spoken\": true, \"text\": \"" +
                          json_escape(text) + "\"}");
    }

    std::string handle_talk(int id) {
        if (state_ != PipelineState::Idle &&
            state_ != PipelineState::WakeListening) {
            return rpc_error(id, -32000, "pipeline busy: " +
                             std::string(state_str(state_)));
        }

        // Start continuous capture with wake-word or direct processing.
        state_ = PipelineState::WakeListening;
        if (!audio_.is_capturing()) {
            auto res = audio_.start_capture(
                [this](AudioBuffer buf) { on_speech_captured(std::move(buf)); });
            if (!res.has_value()) {
                return rpc_error(id, -32000, "capture start failed: " + res.error());
            }
        }

        return rpc_result(id, "{\"mode\": \"talk\", \"state\": \"wake_listening\"}");
    }

    std::string handle_stop(int id) {
        audio_.stop_capture();
        tts_.stop_playback();
        state_ = PipelineState::Idle;

        return rpc_result(id, "{\"stopped\": true}");
    }

    std::string handle_listen(int id) {
        // STT-only mode: start capture, transcribe, return text.
        if (!audio_.is_capturing()) {
            auto res = audio_.start_capture(
                [this](AudioBuffer buf) { on_speech_captured(std::move(buf)); });
            if (!res.has_value()) {
                return rpc_error(id, -32000, "capture failed: " + res.error());
            }
        }

        // Wait for a speech segment (blocking, up to max_recording_s).
        auto start = std::chrono::steady_clock::now();
        int max_wait_ms = config_.max_recording_s * 1000;

        while (true) {
            {
                std::lock_guard<std::mutex> lock(speech_mutex_);
                if (!pending_speech_.empty()) {
                    auto buf = std::move(pending_speech_.front());
                    pending_speech_.erase(pending_speech_.begin());

                    auto trans = stt_.transcribe(buf);
                    if (trans.has_value()) {
                        std::ostringstream out;
                        out << "{\"text\": \"" << json_escape(trans.value().text) << "\", ";
                        out << "\"language\": \"" << json_escape(trans.value().language) << "\", ";
                        out << "\"confidence\": " << trans.value().confidence << ", ";
                        out << "\"audio_duration_s\": " << trans.value().audio_duration_s << ", ";
                        out << "\"process_time_s\": " << trans.value().process_time_s;
                        out << "}";
                        return rpc_result(id, out.str());
                    } else {
                        return rpc_error(id, -32000, "STT failed: " + trans.error());
                    }
                }
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= max_wait_ms) {
                return rpc_error(id, -32000, "listen timeout: no speech detected");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::string handle_transcribe(const std::string& raw, int id) {
        std::string params = extract_params_object(raw);
        std::string audio_file = extract_json_string(params, "file");

        if (audio_file.empty()) {
            return rpc_error(id, -32602, "missing 'file' in params");
        }

        // Read the audio file (assume WAV for now).
        std::ifstream in(audio_file, std::ios::binary);
        if (!in.is_open()) {
            return rpc_error(id, -32602, "cannot open file: " + audio_file);
        }

        // Skip WAV header (44 bytes).
        char header[44];
        in.read(header, 44);
        int sr = *reinterpret_cast<int*>(header + 24);

        // Read samples.
        std::vector<float> samples;
        int16_t sample;
        while (in.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
            samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
        in.close();

        auto result = stt_.transcribe(samples, sr);
        if (!result.has_value()) {
            return rpc_error(id, -32000, "transcribe failed: " + result.error());
        }

        const auto& tr = result.value();
        std::ostringstream out;
        out << "{\"text\": \"" << json_escape(tr.text) << "\", ";
        out << "\"language\": \"" << json_escape(tr.language) << "\", ";
        out << "\"confidence\": " << tr.confidence << ", ";
        out << "\"audio_duration_s\": " << tr.audio_duration_s << ", ";
        out << "\"process_time_s\": " << tr.process_time_s << ", ";
        out << "\"timestamps\": [";
        for (size_t i = 0; i < tr.timestamps.size(); ++i) {
            if (i > 0) out << ", ";
            out << "{\"word\": \"" << json_escape(tr.timestamps[i].word) << "\", ";
            out << "\"start\": " << tr.timestamps[i].start_s << ", ";
            out << "\"end\": " << tr.timestamps[i].end_s << "}";
        }
        out << "]";
        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_ptt_start(int id) {
        auto res = audio_.start_push_to_talk();
        if (!res.has_value()) {
            return rpc_error(id, -32000, "PTT start failed: " + res.error());
        }
        state_ = PipelineState::Recording;

        // Make sure capture is running.
        if (!audio_.is_capturing()) {
            audio_.start_capture(
                [this](AudioBuffer buf) { on_speech_captured(std::move(buf)); });
        }

        return rpc_result(id, "{\"recording\": true}");
    }

    std::string handle_ptt_stop(int id) {
        auto buf_result = audio_.stop_push_to_talk();
        if (!buf_result.has_value()) {
            return rpc_error(id, -32000, "PTT stop failed: " + buf_result.error());
        }

        AudioBuffer buf = std::move(buf_result.value());
        if (buf.empty()) {
            state_ = config_.push_to_talk ? PipelineState::Idle
                                          : PipelineState::WakeListening;
            return rpc_error(id, -32000, "no audio recorded");
        }

        // Process the PTT buffer through the full pipeline.
        total_audio_s_ += buf.duration_seconds();

        state_ = PipelineState::Transcribing;
        auto trans = stt_.transcribe(buf);
        if (!trans.has_value()) {
            state_ = PipelineState::Idle;
            return rpc_error(id, -32000, "STT failed: " + trans.error());
        }

        std::string text = trans.value().text;
        if (text.empty()) {
            state_ = PipelineState::Idle;
            return rpc_error(id, -32000, "no speech detected");
        }

        // Run through LLM.
        ++total_queries_;
        state_ = PipelineState::Thinking;
        conversation_.add_user(text, buf.duration_seconds());

        auto llm_result = llm_.generate(text, conversation_, tools_);
        if (!llm_result.has_value()) {
            state_ = PipelineState::Idle;
            return rpc_error(id, -32000, "LLM failed: " + llm_result.error());
        }

        const auto& response = llm_result.value();
        total_tool_calls_ += static_cast<int>(response.tool_calls.size());
        conversation_.add_assistant(response.text);

        // Speak the response.
        state_ = PipelineState::Speaking;
        tts_.speak(response.text);

        state_ = config_.push_to_talk ? PipelineState::Idle
                                      : PipelineState::WakeListening;

        std::ostringstream out;
        out << "{";
        out << "\"user_text\": \"" << json_escape(text) << "\", ";
        out << "\"response_text\": \"" << json_escape(response.text) << "\", ";
        out << "\"audio_duration_s\": " << trans.value().audio_duration_s << ", ";
        out << "\"inference_time_s\": " << response.inference_time_s << ", ";
        out << "\"tools_used\": " << response.tool_calls.size();
        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_models(int id) {
        std::ostringstream out;
        out << "{";

        // STT models.
        out << "\"stt\": {\"current\": \"" << json_escape(stt_.model_path())
            << "\", \"loaded\": " << (stt_.is_loaded() ? "true" : "false")
            << ", \"size\": \"";
        switch (stt_.model_size()) {
            case WhisperModelSize::Tiny:   out << "tiny"; break;
            case WhisperModelSize::Base:   out << "base"; break;
            case WhisperModelSize::Small:  out << "small"; break;
            case WhisperModelSize::Medium: out << "medium"; break;
        }
        out << "\"}, ";

        // TTS voices.
        auto voices = tts_.list_voices();
        out << "\"tts\": {\"current\": \"" << json_escape(tts_.current_voice())
            << "\", \"loaded\": " << (tts_.is_loaded() ? "true" : "false")
            << ", \"voices\": [";
        for (size_t i = 0; i < voices.size(); ++i) {
            if (i > 0) out << ", ";
            out << "{\"name\": \"" << json_escape(voices[i].name)
                << "\", \"language\": \"" << json_escape(voices[i].language)
                << "\", \"loaded\": " << (voices[i].loaded ? "true" : "false") << "}";
        }
        out << "]}, ";

        // LLM.
        out << "\"llm\": {\"current\": \"" << json_escape(llm_.model_path())
            << "\", \"loaded\": " << (llm_.is_loaded() ? "true" : "false") << "}";

        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_config(int id) {
        std::ostringstream out;
        out << "{";
        out << "\"wake_word\": \"" << json_escape(config_.wake_word) << "\", ";
        out << "\"language\": \"" << json_escape(config_.language) << "\", ";
        out << "\"push_to_talk\": " << (config_.push_to_talk ? "true" : "false") << ", ";
        out << "\"stt_model\": \"" << json_escape(config_.stt_model_path) << "\", ";
        out << "\"tts_model\": \"" << json_escape(config_.tts_model_path) << "\", ";
        out << "\"tts_voice\": \"" << json_escape(config_.tts_voice) << "\", ";
        out << "\"tts_speed\": " << config_.tts_speed << ", ";
        out << "\"tts_pitch\": " << config_.tts_pitch << ", ";
        out << "\"llm_model\": \"" << json_escape(config_.llm_model_path) << "\", ";
        out << "\"llm_temperature\": " << config_.llm_temperature << ", ";
        out << "\"llm_max_tokens\": " << config_.llm_max_tokens << ", ";
        out << "\"context_window\": " << config_.context_window << ", ";
        out << "\"audio_device\": \"" << json_escape(config_.audio_device) << "\", ";
        out << "\"sample_rate\": " << config_.sample_rate << ", ";
        out << "\"vad_threshold\": " << config_.vad_threshold << ", ";
        out << "\"silence_timeout_ms\": " << config_.silence_timeout_ms << ", ";
        out << "\"max_recording_s\": " << config_.max_recording_s;
        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_history(int id) {
        std::ostringstream out;
        out << "{\"turns\": [";
        const auto& turns = conversation_.turns();
        for (size_t i = 0; i < turns.size(); ++i) {
            if (i > 0) out << ", ";
            out << turns[i].to_json();
        }
        out << "], \"total\": " << conversation_.total_turns()
            << ", \"window\": " << conversation_.size() << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_clear(int id) {
        conversation_.clear();
        return rpc_result(id, "{\"cleared\": true}");
    }
};

} // anonymous namespace

int main() {
    VoiceDaemon daemon;
    return daemon.run();
}
