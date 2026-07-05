/**
 * StrayLight Predict Daemon — Predictive resource preloading.
 *
 * Learns app usage patterns from timeline data, predicts next apps,
 * and pre-warms system resources (libraries, VPU slabs, files, mesh).
 *
 * Runs a prediction cycle every 30 seconds and retrains the model hourly.
 *
 * Listens on /run/straylight/predict.sock for JSON-RPC control:
 *   predict.status      — Get daemon status and statistics
 *   predict.predictions — Get current predictions
 *   predict.history     — Get usage patterns
 *   predict.train       — Force model retrain
 *   predict.preloads    — List active preloads
 *   predict.enable      — Enable predictive preloading
 *   predict.disable     — Disable predictive preloading
 */

#include "prediction_engine.h"
#include "preloader.h"
#include "usage_collector.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

namespace {

using namespace straylight;
using namespace straylight::predict;

static constexpr const char* SOCKET_PATH = "/run/straylight/predict.sock";
static constexpr int MAX_CLIENTS = 16;
static constexpr int PREDICT_INTERVAL_SEC = 30;
static constexpr int RETRAIN_INTERVAL_SEC = 3600;

// ─── JSON helpers ───────────────────────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
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
        }
        result += json[pos++];
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

// ─── Predict Daemon ─────────────────────────────────────────────────────────

class PredictDaemon : public DaemonBase {
public:
    PredictDaemon() = default;

protected:
    Result<void, SLError> init(const Config& /*cfg*/) override {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(SOCKET_PATH).parent_path(), ec);

        ::unlink(SOCKET_PATH);

        listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            return fail("socket() failed: " + std::string(strerror(errno)));
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(listen_fd_);
            return fail("bind() failed: " + std::string(strerror(errno)));
        }

        chmod(SOCKET_PATH, 0666);

        if (listen(listen_fd_, 4) < 0) {
            close(listen_fd_);
            return fail("listen() failed: " + std::string(strerror(errno)));
        }

        // Initial model training
        fprintf(stdout, "[straylight-predict] training prediction model...\n");
        auto train_result = engine_.train();
        if (train_result) {
            fprintf(stdout, "[straylight-predict] model trained on %d events, %d apps\n",
                    train_result.value(), engine_.app_count());
        } else {
            fprintf(stdout, "[straylight-predict] initial training: %s (will retry)\n",
                    train_result.error().c_str());
        }

        fprintf(stdout, "[straylight-predict] listening on %s\n", SOCKET_PATH);
        fprintf(stdout, "[straylight-predict] prediction cycle: every %ds\n",
                PREDICT_INTERVAL_SEC);

        last_predict_time_ = std::chrono::steady_clock::now();
        last_retrain_time_ = std::chrono::steady_clock::now();

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto now = std::chrono::steady_clock::now();

        // Handle client connections
        handle_clients();

        // Prediction cycle (every 30 seconds)
        auto since_predict = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_predict_time_).count();
        if (since_predict >= PREDICT_INTERVAL_SEC && enabled_) {
            run_prediction_cycle();
            last_predict_time_ = now;
        }

        // Retrain cycle (every hour)
        auto since_retrain = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_retrain_time_).count();
        if (since_retrain >= RETRAIN_INTERVAL_SEC) {
            fprintf(stdout, "[straylight-predict] retraining model...\n");
            auto result = engine_.train();
            if (result) {
                fprintf(stdout, "[straylight-predict] retrained on %d events\n",
                        result.value());
            }
            last_retrain_time_ = now;
        }

        return finish_tick();
    }

    void shutdown() override {
        // Evict all preloads
        preloader_.evict_all();

        for (int fd : client_fds_) close(fd);
        client_fds_.clear();

        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        ::unlink(SOCKET_PATH);
        fprintf(stdout, "[straylight-predict] all preloads evicted, socket removed\n");
    }

private:
    int listen_fd_ = -1;
    std::vector<int> client_fds_;

    PredictionEngine engine_;
    Preloader preloader_;
    UsageCollector collector_;

    bool enabled_ = true;
    int total_prediction_cycles_ = 0;
    int total_preloads_ = 0;
    int total_evictions_ = 0;

    std::chrono::steady_clock::time_point last_predict_time_;
    std::chrono::steady_clock::time_point last_retrain_time_;

    // Currently predicted and preloaded app set
    std::set<std::string> predicted_apps_;

    static Result<void, SLError> fail(const std::string& message) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, message});
    }

    static Result<void, SLError> finish_tick() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return Result<void, SLError>::ok();
    }

    // ─── Prediction cycle ───────────────────────────────────────────────

    void run_prediction_cycle() {
        ++total_prediction_cycles_;

        // Collect current state
        auto running = collector_.running_apps();
        std::string focused = collector_.focused_app();

        // Get predictions
        auto predictions = engine_.predict_next_apps(5, focused, running);

        // Build new predicted set
        std::set<std::string> new_predicted;
        for (const auto& pred : predictions) {
            if (pred.probability > 0.1) { // Only preload if >10% likely
                new_predicted.insert(pred.app_name);
            }
        }

        // Evict apps that are no longer predicted
        for (const auto& app : predicted_apps_) {
            if (new_predicted.find(app) == new_predicted.end()) {
                preloader_.evict_preloads(app);
                ++total_evictions_;
            }
        }

        // Preload newly predicted apps
        for (const auto& app : new_predicted) {
            if (predicted_apps_.find(app) == predicted_apps_.end()) {
                auto result = preloader_.preload_app(app);
                if (result) {
                    ++total_preloads_;
                }
            }
        }

        predicted_apps_ = new_predicted;
    }

    // ─── Client handling ────────────────────────────────────────────────

    void handle_clients() {
        // Accept new connections
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

        int ready = poll(pfds.data(), pfds.size(), 10);
        if (ready <= 0) return;

        std::vector<int> to_remove;

        for (size_t i = 0; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            char buf[4096];
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

        if (method == "predict.status")       return handle_status(id);
        if (method == "predict.predictions")  return handle_predictions(raw, id);
        if (method == "predict.history")      return handle_history(id);
        if (method == "predict.train")        return handle_train(id);
        if (method == "predict.preloads")     return handle_preloads(id);
        if (method == "predict.enable")       return handle_enable(id);
        if (method == "predict.disable")      return handle_disable(id);

        return rpc_error(id, -32601, "method not found: " + method);
    }

    std::string handle_status(int id) {
        auto budget = preloader_.get_budget();
        auto preloads = preloader_.list_preloads();

        std::ostringstream out;
        out << "{";
        out << "\"enabled\": " << (enabled_ ? "true" : "false") << ", ";
        out << "\"model_events\": " << engine_.total_events() << ", ";
        out << "\"known_apps\": " << engine_.app_count() << ", ";
        out << "\"prediction_cycles\": " << total_prediction_cycles_ << ", ";
        out << "\"total_preloads\": " << total_preloads_ << ", ";
        out << "\"total_evictions\": " << total_evictions_ << ", ";
        out << "\"active_preloads\": " << preloads.size() << ", ";
        out << "\"predicted_apps\": " << predicted_apps_.size() << ", ";
        out << "\"ram_budget_mb\": " << (budget.max_ram_bytes / (1024 * 1024)) << ", ";
        out << "\"ram_used_mb\": " << (budget.used_ram_bytes / (1024 * 1024)) << ", ";
        out << "\"vram_budget_mb\": " << (budget.max_vram_bytes / (1024 * 1024)) << ", ";
        out << "\"vram_used_mb\": " << (budget.used_vram_bytes / (1024 * 1024));
        out << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_predictions(const std::string& raw, int id) {
        int top_n = 5;

        // Extract params if present
        auto params_pos = raw.find("\"params\"");
        if (params_pos != std::string::npos) {
            auto brace = raw.find('{', params_pos + 8);
            if (brace != std::string::npos) {
                int depth = 0;
                size_t end = brace;
                for (size_t i = brace; i < raw.size(); ++i) {
                    if (raw[i] == '{') ++depth;
                    if (raw[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
                }
                std::string params = raw.substr(brace, end - brace + 1);
                int n = extract_json_int(params, "top_n");
                if (n > 0) top_n = n;
            }
        }

        auto running = collector_.running_apps();
        std::string focused = collector_.focused_app();
        auto predictions = engine_.predict_next_apps(top_n, focused, running);

        std::ostringstream out;
        out << "{\"focused_app\": \"" << json_escape(focused) << "\", ";
        out << "\"predictions\": [";
        for (size_t i = 0; i < predictions.size(); ++i) {
            if (i > 0) out << ", ";
            out << "{\"app\": \"" << json_escape(predictions[i].app_name) << "\", "
                << "\"probability\": " << std::fixed << std::setprecision(3)
                << predictions[i].probability << ", "
                << "\"reason\": \"" << json_escape(predictions[i].reason) << "\", "
                << "\"preloaded\": "
                << (preloader_.is_preloaded(predictions[i].app_name) ? "true" : "false")
                << "}";
        }
        out << "]}";
        return rpc_result(id, out.str());
    }

    std::string handle_history(int id) {
        auto patterns = engine_.get_patterns();

        std::ostringstream out;
        out << "{\"patterns\": [";
        for (size_t i = 0; i < patterns.size() && i < 30; ++i) {
            if (i > 0) out << ", ";
            out << "{\"app\": \"" << json_escape(patterns[i].app_name) << "\", "
                << "\"total_launches\": " << patterns[i].total_launches << ", "
                << "\"avg_session_seconds\": " << std::fixed << std::setprecision(1)
                << patterns[i].avg_session_seconds << ", "
                << "\"preceded_by\": \"" << json_escape(patterns[i].preceded_by) << "\", "
                << "\"typical_hours\": [";
            for (size_t h = 0; h < patterns[i].typical_launch_hours.size(); ++h) {
                if (h > 0) out << ", ";
                out << patterns[i].typical_launch_hours[h];
            }
            out << "], \"typical_days\": [";
            for (size_t d = 0; d < patterns[i].typical_launch_days.size(); ++d) {
                if (d > 0) out << ", ";
                out << patterns[i].typical_launch_days[d];
            }
            out << "]}";
        }
        out << "], \"total_apps\": " << patterns.size() << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_train(int id) {
        auto result = engine_.train();
        if (!result) {
            return rpc_error(id, -32000, result.error());
        }

        std::ostringstream out;
        out << "{\"events\": " << result.value()
            << ", \"apps\": " << engine_.app_count() << "}";
        return rpc_result(id, out.str());
    }

    std::string handle_preloads(int id) {
        auto preloads = preloader_.list_preloads();
        auto budget = preloader_.get_budget();

        std::ostringstream out;
        out << "{\"preloads\": [";
        for (size_t i = 0; i < preloads.size(); ++i) {
            if (i > 0) out << ", ";
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - preloads[i].loaded_at).count();

            out << "{\"app\": \"" << json_escape(preloads[i].app_name) << "\", "
                << "\"resource\": \"" << json_escape(preloads[i].resource_path) << "\", "
                << "\"type\": \"" << json_escape(preloads[i].resource_type) << "\", "
                << "\"size_kb\": " << (preloads[i].size_bytes / 1024) << ", "
                << "\"loaded_seconds_ago\": " << elapsed << ", "
                << "\"active\": " << (preloads[i].active ? "true" : "false")
                << "}";
        }
        out << "], \"budget\": {"
            << "\"max_ram_mb\": " << (budget.max_ram_bytes / (1024 * 1024)) << ", "
            << "\"used_ram_mb\": " << (budget.used_ram_bytes / (1024 * 1024)) << ", "
            << "\"max_vram_mb\": " << (budget.max_vram_bytes / (1024 * 1024)) << ", "
            << "\"used_vram_mb\": " << (budget.used_vram_bytes / (1024 * 1024))
            << "}}";
        return rpc_result(id, out.str());
    }

    std::string handle_enable(int id) {
        enabled_ = true;
        return rpc_result(id, "{\"enabled\": true}");
    }

    std::string handle_disable(int id) {
        enabled_ = false;
        preloader_.evict_all();
        predicted_apps_.clear();
        return rpc_result(id, "{\"enabled\": false, \"preloads_evicted\": true}");
    }
};

} // anonymous namespace

int main() {
    Log::init("straylight-predict");
    PredictDaemon daemon;
    return daemon.run(Config::make_empty());
}
