// services/lens/main.cpp
// straylight-lens daemon — Full-stack request tracing across compositor, IPC, VPU, GPU.

#include "trace_collector.h"
#include "correlator.h"
#include "trace_store.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include <sys/socket.h>
#include <sys/time.h>

namespace straylight {

namespace {

void set_receive_timeout(IpcConnection& conn) {
    timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(conn.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void send_response(IpcConnection& conn, const nlohmann::json& response) {
    auto send_result = conn.send(response.dump());
    if (!send_result.has_value()) {
        SL_WARN("lens: failed to send response: {}", send_result.error());
    }
}

} // namespace

class LensDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("lens: initializing daemon");

        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/lens.sock");
        store_dir_ = cfg.get<std::string>(
            "store_dir", "/var/lib/straylight/lens");
        max_traces_ = cfg.get<int>("retention.max_traces", 100);
        max_age_hours_ = cfg.get<int>("retention.max_age_hours", 168);
        tick_interval_ms_ = cfg.get<int>("tick_interval_ms", 200);

        collector_ = std::make_unique<TraceCollector>();
        correlator_ = std::make_unique<Correlator>();
        store_ = std::make_unique<TraceStore>(store_dir_);

        if (cfg.has("max_gap_ms")) {
            uint64_t gap_ns = cfg.get<int>("max_gap_ms", 5) * 1000000ULL;
            correlator_->set_max_gap_ns(gap_ns);
        }

        auto bind_result = server_.bind(socket_path_);
        if (!bind_result.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IpcFailed,
                        "Failed to bind socket: " + bind_result.error()});
        }

        // Apply retention on startup
        store_->apply_retention(static_cast<size_t>(max_traces_),
                                static_cast<uint64_t>(max_age_hours_));

        SL_INFO("lens: listening on {}", socket_path_);
        SL_INFO("lens: daemon initialized (store={})", store_dir_);

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto conn_result = server_.accept(tick_interval_ms_);
        if (conn_result.has_value()) {
            handle_connection(std::move(conn_result).value());
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("lens: shutting down");
        if (collector_->is_collecting()) {
            auto result = collector_->stop();
            if (result.has_value()) {
                store_->store(result.value());
            }
        }
        SL_INFO("lens: shutdown complete");
    }

private:
    void handle_connection(std::unique_ptr<IpcConnection> conn) {
        set_receive_timeout(*conn);
        auto msg_result = conn->receive();
        if (!msg_result.has_value()) {
            if (msg_result.error() == "Failed to receive message length") {
                SL_DEBUG("lens: client closed before sending a request");
            } else {
                SL_WARN("lens: failed to receive: {}", msg_result.error());
            }
            return;
        }

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(msg_result.value());
        } catch (const nlohmann::json::parse_error& e) {
            nlohmann::json err;
            err["error"] = std::string("JSON parse error: ") + e.what();
            send_response(*conn, err);
            return;
        }

        nlohmann::json response = dispatch(request);
        send_response(*conn, response);
    }

    nlohmann::json dispatch(const nlohmann::json& request) {
        std::string method = request.value("method", "");
        nlohmann::json params = request.value("params", nlohmann::json::object());
        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", 0);

        if (method == "start") {
            return handle_start(params, response);
        } else if (method == "stop") {
            return handle_stop(response);
        } else if (method == "show") {
            return handle_show(params, response);
        } else if (method == "list") {
            return handle_list(response);
        } else if (method == "export") {
            return handle_export(params, response);
        } else if (method == "bottleneck") {
            return handle_bottleneck(params, response);
        } else if (method == "live") {
            return handle_live(response);
        } else {
            response["error"] = "Unknown method: " + method;
        }
        return response;
    }

    nlohmann::json handle_start(const nlohmann::json& params, nlohmann::json response) {
        std::string corr_id = params.value("correlation_id", "");

        auto result = collector_->start(corr_id);
        if (result.has_value()) {
            auto snapshot = collector_->current_snapshot();
            response["result"]["trace_id"] = snapshot.trace_id;
            response["result"]["correlation_id"] = snapshot.correlation_id;
            response["result"]["status"] = "collecting";
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_stop(nlohmann::json response) {
        auto result = collector_->stop();
        if (result.has_value()) {
            auto& trace = result.value();

            // Correlate and store
            auto graph = correlator_->correlate(trace);
            auto cp = correlator_->get_critical_path(graph);
            store_->store(trace);

            response["result"]["trace_id"] = trace.trace_id;
            response["result"]["event_count"] = trace.events.size();
            response["result"]["duration_ns"] = trace.end_ns - trace.start_ns;
            response["result"]["critical_path_events"] = cp.event_indices.size();
            response["result"]["critical_path_duration_ns"] = cp.total_duration_ns;
            response["result"]["status"] = "complete";
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_show(const nlohmann::json& params, nlohmann::json response) {
        std::string trace_id = params.value("trace_id", "");
        auto load_result = store_->load(trace_id);
        if (!load_result.has_value()) {
            response["error"]["code"] = static_cast<int>(load_result.error().code());
            response["error"]["message"] = load_result.error().message();
            return response;
        }

        const auto& trace = load_result.value();
        auto graph = correlator_->correlate(trace);
        auto cp = correlator_->get_critical_path(graph);

        nlohmann::json result;
        result["trace_id"] = trace.trace_id;
        result["correlation_id"] = trace.correlation_id;
        result["event_count"] = trace.events.size();
        result["duration_ns"] = trace.end_ns - trace.start_ns;

        // Build timeline by layer
        nlohmann::json timeline;
        std::map<TraceLayer, nlohmann::json> layer_events;
        for (const auto& ev : trace.events) {
            nlohmann::json ej;
            ej["timestamp_ns"] = ev.timestamp_ns;
            ej["event_type"] = ev.event_type;
            ej["pid"] = ev.pid;
            ej["duration_ns"] = ev.duration_ns;
            ej["data"] = ev.data;
            layer_events[ev.layer].push_back(ej);
        }

        for (const auto& [layer, events] : layer_events) {
            timeline[trace_layer_name(layer)] = events;
        }
        result["timeline"] = timeline;

        // Critical path
        nlohmann::json cp_json;
        cp_json["total_duration_ns"] = cp.total_duration_ns;
        cp_json["total_work_ns"] = cp.total_work_ns;
        cp_json["total_gap_ns"] = cp.total_gap_ns;
        nlohmann::json cp_events = nlohmann::json::array();
        for (size_t idx : cp.event_indices) {
            if (idx < graph.events.size()) {
                nlohmann::json ce;
                ce["layer"] = trace_layer_name(graph.events[idx].layer);
                ce["event_type"] = graph.events[idx].event_type;
                ce["duration_ns"] = graph.events[idx].duration_ns;
                cp_events.push_back(ce);
            }
        }
        cp_json["events"] = cp_events;
        result["critical_path"] = cp_json;

        response["result"] = result;
        return response;
    }

    nlohmann::json handle_list(nlohmann::json response) {
        auto summaries = store_->list();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : summaries) {
            nlohmann::json entry;
            entry["trace_id"] = s.trace_id;
            entry["correlation_id"] = s.correlation_id;
            entry["event_count"] = s.event_count;
            entry["duration_ns"] = s.end_ns - s.start_ns;
            entry["file_size"] = s.file_size;
            arr.push_back(entry);
        }
        response["result"] = arr;
        return response;
    }

    nlohmann::json handle_export(const nlohmann::json& params, nlohmann::json response) {
        std::string trace_id = params.value("trace_id", "");
        std::string format_str = params.value("format", "chrome");

        TraceExportFormat format = TraceExportFormat::ChromeTrace;
        if (format_str == "json") format = TraceExportFormat::Json;
        else if (format_str == "binary") format = TraceExportFormat::Binary;

        auto result = store_->export_trace(trace_id, format);
        if (result.has_value()) {
            response["result"]["data"] = result.value();
            response["result"]["format"] = format_str;
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_bottleneck(const nlohmann::json& params, nlohmann::json response) {
        std::string trace_id = params.value("trace_id", "");

        auto load_result = store_->load(trace_id);
        if (!load_result.has_value()) {
            response["error"]["code"] = static_cast<int>(load_result.error().code());
            response["error"]["message"] = load_result.error().message();
            return response;
        }

        const auto& trace = load_result.value();
        auto graph = correlator_->correlate(trace);
        auto bn = correlator_->get_bottleneck(graph);

        nlohmann::json result;
        result["layer"] = trace_layer_name(bn.event.layer);
        result["event_type"] = bn.event.event_type;
        result["duration_ns"] = bn.duration_ns;
        result["fraction"] = bn.fraction;
        result["suggestion"] = bn.suggestion;
        result["pid"] = bn.event.pid;

        response["result"] = result;
        return response;
    }

    nlohmann::json handle_live(nlohmann::json response) {
        if (!collector_->is_collecting()) {
            response["result"]["status"] = "idle";
            response["result"]["event_count"] = 0;
            return response;
        }

        auto snapshot = collector_->current_snapshot();
        response["result"]["status"] = "collecting";
        response["result"]["trace_id"] = snapshot.trace_id;
        response["result"]["event_count"] = snapshot.events.size();
        response["result"]["duration_ns"] = TraceCollector::now_ns() - snapshot.start_ns;

        // Layer breakdown
        std::map<TraceLayer, size_t> counts;
        for (const auto& ev : snapshot.events) {
            counts[ev.layer]++;
        }
        nlohmann::json layers;
        for (const auto& [layer, count] : counts) {
            layers[trace_layer_name(layer)] = count;
        }
        response["result"]["layers"] = layers;

        return response;
    }

    IpcServer server_;
    std::unique_ptr<TraceCollector> collector_;
    std::unique_ptr<Correlator> correlator_;
    std::unique_ptr<TraceStore> store_;

    std::string socket_path_;
    std::string store_dir_;
    int max_traces_{100};
    int max_age_hours_{168};
    int tick_interval_ms_{200};
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-lens");

    auto cfg_result = straylight::Config::load("/etc/straylight/lens.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("lens: no config file, using defaults");
        auto default_cfg = straylight::Config::load("/dev/null");
        if (!default_cfg.has_value()) {
            SL_ERROR("lens: cannot initialize");
            return 1;
        }
        straylight::LensDaemon daemon;
        return daemon.run(default_cfg.value());
    }

    straylight::LensDaemon daemon;
    return daemon.run(cfg_result.value());
}
