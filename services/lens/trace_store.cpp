// services/lens/trace_store.cpp
// Persistent trace storage with Chrome Trace Format and binary export.

#include "trace_store.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace straylight {

TraceStore::TraceStore(const std::filesystem::path& store_dir)
    : store_dir_(store_dir)
{
    std::error_code ec;
    std::filesystem::create_directories(store_dir_, ec);
    if (ec) {
        SL_WARN("lens: cannot create trace store directory {}: {}",
                store_dir_.string(), ec.message());
    }
}

std::filesystem::path TraceStore::trace_path(const std::string& trace_id) const {
    return store_dir_ / (trace_id + ".json");
}

std::string TraceStore::serialize_json(const Trace& trace) {
    nlohmann::json j;
    j["trace_id"] = trace.trace_id;
    j["correlation_id"] = trace.correlation_id;
    j["state"] = static_cast<int>(trace.state);
    j["start_ns"] = trace.start_ns;
    j["end_ns"] = trace.end_ns;

    nlohmann::json events = nlohmann::json::array();
    for (const auto& ev : trace.events) {
        nlohmann::json ej;
        ej["timestamp_ns"] = ev.timestamp_ns;
        ej["layer"] = static_cast<int>(ev.layer);
        ej["event_type"] = ev.event_type;
        ej["correlation_id"] = ev.correlation_id;
        ej["pid"] = ev.pid;
        ej["duration_ns"] = ev.duration_ns;
        ej["data"] = ev.data;
        events.push_back(ej);
    }
    j["events"] = events;

    return j.dump(2);
}

Result<Trace, std::string> TraceStore::deserialize_json(const std::string& data) {
    try {
        auto j = nlohmann::json::parse(data);

        Trace trace;
        trace.trace_id = j.value("trace_id", "");
        trace.correlation_id = j.value("correlation_id", "");
        trace.state = static_cast<TraceState>(j.value("state", 0));
        trace.start_ns = j.value("start_ns", uint64_t{0});
        trace.end_ns = j.value("end_ns", uint64_t{0});

        if (j.contains("events") && j["events"].is_array()) {
            for (const auto& ej : j["events"]) {
                TraceEvent ev;
                ev.timestamp_ns = ej.value("timestamp_ns", uint64_t{0});
                ev.layer = static_cast<TraceLayer>(ej.value("layer", 0));
                ev.event_type = ej.value("event_type", "");
                ev.correlation_id = ej.value("correlation_id", "");
                ev.pid = ej.value("pid", 0);
                ev.duration_ns = ej.value("duration_ns", uint64_t{0});

                if (ej.contains("data") && ej["data"].is_object()) {
                    for (auto& [k, v] : ej["data"].items()) {
                        ev.data[k] = v.is_string() ? v.get<std::string>() : v.dump();
                    }
                }
                trace.events.push_back(std::move(ev));
            }
        }

        return Result<Trace, std::string>::ok(std::move(trace));
    } catch (const nlohmann::json::exception& e) {
        return Result<Trace, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }
}

std::vector<uint8_t> TraceStore::serialize_binary(const Trace& trace) {
    // Binary format: header + event records
    // Header: magic(4) version(2) event_count(4) start_ns(8) end_ns(8)
    //         trace_id_len(2) trace_id correlation_id_len(2) correlation_id
    // Event:  timestamp_ns(8) layer(1) event_type_len(2) event_type
    //         pid(4) duration_ns(8) data_count(2) [key_len(2) key val_len(2) val]*

    std::vector<uint8_t> buf;
    buf.reserve(trace.events.size() * 128);

    auto write_u8 = [&](uint8_t v) { buf.push_back(v); };
    auto write_u16 = [&](uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto write_u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };
    auto write_u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };
    auto write_str = [&](const std::string& s) {
        write_u16(static_cast<uint16_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    };

    // Header
    buf.push_back('S'); buf.push_back('L'); buf.push_back('T'); buf.push_back('R');
    write_u16(1); // version
    write_u32(static_cast<uint32_t>(trace.events.size()));
    write_u64(trace.start_ns);
    write_u64(trace.end_ns);
    write_str(trace.trace_id);
    write_str(trace.correlation_id);

    // Events
    for (const auto& ev : trace.events) {
        write_u64(ev.timestamp_ns);
        write_u8(static_cast<uint8_t>(ev.layer));
        write_str(ev.event_type);
        write_u32(static_cast<uint32_t>(ev.pid));
        write_u64(ev.duration_ns);
        write_u16(static_cast<uint16_t>(ev.data.size()));
        for (const auto& [k, v] : ev.data) {
            write_str(k);
            write_str(v);
        }
    }

    return buf;
}

Result<Trace, std::string> TraceStore::deserialize_binary(const std::vector<uint8_t>& data) {
    if (data.size() < 4 || data[0] != 'S' || data[1] != 'L' ||
        data[2] != 'T' || data[3] != 'R') {
        return Result<Trace, std::string>::error("Invalid binary trace header");
    }

    size_t pos = 4;
    auto read_u8 = [&]() -> uint8_t {
        return (pos < data.size()) ? data[pos++] : 0;
    };
    auto read_u16 = [&]() -> uint16_t {
        uint16_t v = 0;
        for (int i = 0; i < 2 && pos < data.size(); ++i) v |= static_cast<uint16_t>(data[pos++]) << (i * 8);
        return v;
    };
    auto read_u32 = [&]() -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4 && pos < data.size(); ++i) v |= static_cast<uint32_t>(data[pos++]) << (i * 8);
        return v;
    };
    auto read_u64 = [&]() -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8 && pos < data.size(); ++i) v |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        return v;
    };
    auto read_str = [&]() -> std::string {
        uint16_t len = read_u16();
        if (pos + len > data.size()) return "";
        std::string s(data.begin() + static_cast<ptrdiff_t>(pos),
                      data.begin() + static_cast<ptrdiff_t>(pos + len));
        pos += len;
        return s;
    };

    Trace trace;
    /*uint16_t version =*/ read_u16();
    uint32_t event_count = read_u32();
    trace.start_ns = read_u64();
    trace.end_ns = read_u64();
    trace.trace_id = read_str();
    trace.correlation_id = read_str();
    trace.state = TraceState::Complete;

    trace.events.reserve(event_count);
    for (uint32_t i = 0; i < event_count && pos < data.size(); ++i) {
        TraceEvent ev;
        ev.timestamp_ns = read_u64();
        ev.layer = static_cast<TraceLayer>(read_u8());
        ev.event_type = read_str();
        ev.pid = static_cast<pid_t>(read_u32());
        ev.duration_ns = read_u64();
        ev.correlation_id = trace.correlation_id;

        uint16_t data_count = read_u16();
        for (uint16_t d = 0; d < data_count && pos < data.size(); ++d) {
            std::string k = read_str();
            std::string v = read_str();
            ev.data[k] = v;
        }
        trace.events.push_back(std::move(ev));
    }

    return Result<Trace, std::string>::ok(std::move(trace));
}

Result<std::string, SLError> TraceStore::store(const Trace& trace) {
    std::string path = trace_path(trace.trace_id).string();

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return Result<std::string, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot write to " + path});
    }

    out << serialize_json(trace);
    out.close();

    if (out.fail()) {
        return Result<std::string, SLError>::error(
            SLError{SLErrorCode::IOError, "Write failed to " + path});
    }

    SL_DEBUG("lens: stored trace {} ({} events) to {}",
             trace.trace_id, trace.events.size(), path);

    return Result<std::string, SLError>::ok(trace.trace_id);
}

Result<Trace, SLError> TraceStore::load(const std::string& trace_id) const {
    std::string path = trace_path(trace_id).string();

    std::ifstream in(path);
    if (!in.is_open()) {
        return Result<Trace, SLError>::error(
            SLError{SLErrorCode::NotFound, "Trace not found: " + trace_id});
    }

    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    auto result = deserialize_json(data);
    if (!result.has_value()) {
        return Result<Trace, SLError>::error(
            SLError{SLErrorCode::ParseError, result.error()});
    }

    return Result<Trace, SLError>::ok(std::move(result.value()));
}

std::vector<TraceSummary> TraceStore::list() const {
    std::vector<TraceSummary> summaries;

    std::error_code ec;
    if (!std::filesystem::exists(store_dir_, ec)) {
        return summaries;
    }

    for (const auto& entry : std::filesystem::directory_iterator(store_dir_, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::string trace_id = entry.path().stem().string();

        // Quick parse: just read the top-level fields
        std::ifstream in(entry.path());
        if (!in.is_open()) continue;

        try {
            auto j = nlohmann::json::parse(in);

            TraceSummary summary;
            summary.trace_id = j.value("trace_id", trace_id);
            summary.correlation_id = j.value("correlation_id", "");
            summary.start_ns = j.value("start_ns", uint64_t{0});
            summary.end_ns = j.value("end_ns", uint64_t{0});
            summary.event_count = j.contains("events") ? j["events"].size() : 0;
            summary.file_size = entry.file_size(ec);
            summary.file_path = entry.path().string();

            summaries.push_back(std::move(summary));
        } catch (...) {
            // Skip corrupt files
        }
    }

    // Sort by start time, newest first
    std::sort(summaries.begin(), summaries.end(),
              [](const TraceSummary& a, const TraceSummary& b) {
                  return a.start_ns > b.start_ns;
              });

    return summaries;
}

Result<void, SLError> TraceStore::remove(const std::string& trace_id) {
    auto path = trace_path(trace_id);
    std::error_code ec;
    if (!std::filesystem::remove(path, ec)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Trace not found: " + trace_id});
    }
    return Result<void, SLError>::ok();
}

std::string TraceStore::export_to_chrome_format(const Trace& trace) {
    // Chrome Trace Event Format: https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU
    nlohmann::json chrome;
    chrome["traceEvents"] = nlohmann::json::array();
    chrome["displayTimeUnit"] = "ns";

    nlohmann::json metadata;
    metadata["name"] = "process_name";
    metadata["ph"] = "M";
    metadata["pid"] = 0;
    metadata["args"]["name"] = "StrayLight Trace: " + trace.trace_id;
    chrome["traceEvents"].push_back(metadata);

    // Map layers to process IDs for visual separation
    std::map<TraceLayer, int> layer_pids = {
        {TraceLayer::Compositor, 1},
        {TraceLayer::Ipc,        2},
        {TraceLayer::Vpu,        3},
        {TraceLayer::Gpu,        4},
        {TraceLayer::App,        5},
        {TraceLayer::Kernel,     6},
        {TraceLayer::Network,    7},
    };

    // Add process name events for each layer
    for (const auto& [layer, pid] : layer_pids) {
        nlohmann::json pn;
        pn["name"] = "process_name";
        pn["ph"] = "M";
        pn["pid"] = pid;
        pn["args"]["name"] = trace_layer_name(layer);
        chrome["traceEvents"].push_back(pn);
    }

    // Convert trace events to Chrome format
    for (const auto& ev : trace.events) {
        nlohmann::json ce;
        ce["name"] = ev.event_type;
        ce["cat"] = trace_layer_name(ev.layer);
        ce["pid"] = layer_pids.count(ev.layer) ? layer_pids[ev.layer] : 0;
        ce["tid"] = ev.pid;

        // Timestamp in microseconds (Chrome uses us)
        ce["ts"] = ev.timestamp_ns / 1000;

        if (ev.duration_ns > 0) {
            // Duration event
            ce["ph"] = "X";
            ce["dur"] = ev.duration_ns / 1000;
        } else {
            // Instant event
            ce["ph"] = "i";
            ce["s"] = "t"; // thread scope
        }

        // Add data as args
        nlohmann::json args;
        for (const auto& [k, v] : ev.data) {
            args[k] = v;
        }
        args["correlation_id"] = ev.correlation_id;
        ce["args"] = args;

        chrome["traceEvents"].push_back(ce);
    }

    return chrome.dump(2);
}

std::string TraceStore::export_to_json(const Trace& trace) {
    return serialize_json(trace);
}

Result<std::string, SLError> TraceStore::export_trace(const std::string& trace_id,
                                                        TraceExportFormat format) const {
    auto load_result = load(trace_id);
    if (!load_result.has_value()) {
        return Result<std::string, SLError>::error(load_result.error());
    }

    const auto& trace = load_result.value();

    switch (format) {
        case TraceExportFormat::ChromeTrace:
            return Result<std::string, SLError>::ok(export_to_chrome_format(trace));
        case TraceExportFormat::Json:
            return Result<std::string, SLError>::ok(export_to_json(trace));
        case TraceExportFormat::Binary: {
            auto bin = serialize_binary(trace);
            return Result<std::string, SLError>::ok(
                std::string(bin.begin(), bin.end()));
        }
    }

    return Result<std::string, SLError>::error(
        SLError{SLErrorCode::InvalidArgument, "Unknown export format"});
}

void TraceStore::apply_retention(size_t max_traces, uint64_t max_age_hours) {
    auto summaries = list();

    // Remove traces exceeding count limit (oldest first)
    if (summaries.size() > max_traces) {
        for (size_t i = max_traces; i < summaries.size(); ++i) {
            remove(summaries[i].trace_id);
        }
    }

    // Remove traces exceeding age limit
    uint64_t now_ns = 0;
    {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                 static_cast<uint64_t>(ts.tv_nsec);
    }

    uint64_t max_age_ns = max_age_hours * 3600ULL * 1000000000ULL;
    for (const auto& summary : summaries) {
        if (now_ns > summary.end_ns && (now_ns - summary.end_ns) > max_age_ns) {
            remove(summary.trace_id);
        }
    }
}

} // namespace straylight
