// services/lens/trace_store.h
// Persistent trace storage with Chrome Trace Format export.
#pragma once

#include "trace_collector.h"
#include "correlator.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace straylight {

/// Summary of a stored trace.
struct TraceSummary {
    std::string trace_id;
    std::string correlation_id;
    uint64_t start_ns{0};
    uint64_t end_ns{0};
    size_t event_count{0};
    uint64_t file_size{0};
    std::string file_path;
};

/// Export format for traces.
enum class TraceExportFormat {
    ChromeTrace,   // Chrome Trace Event Format JSON (chrome://tracing)
    Json,          // StrayLight native JSON
    Binary,        // Compact binary format
};

/// Manages persistent storage of traces.
class TraceStore {
public:
    explicit TraceStore(const std::filesystem::path& store_dir =
                        "/var/lib/straylight/lens");

    /// Store a completed trace.
    Result<std::string, SLError> store(const Trace& trace);

    /// Load a trace by ID.
    Result<Trace, SLError> load(const std::string& trace_id) const;

    /// List all stored traces.
    std::vector<TraceSummary> list() const;

    /// Delete a trace by ID.
    Result<void, SLError> remove(const std::string& trace_id);

    /// Export a trace in the specified format.
    Result<std::string, SLError> export_trace(const std::string& trace_id,
                                               TraceExportFormat format) const;

    /// Export a Trace object directly (without loading from disk).
    static std::string export_to_chrome_format(const Trace& trace);
    static std::string export_to_json(const Trace& trace);

    /// Apply retention policy: keep at most max_traces, or traces newer than max_age_hours.
    void apply_retention(size_t max_traces = 100, uint64_t max_age_hours = 168);

private:
    /// Path to the trace file for a given ID.
    std::filesystem::path trace_path(const std::string& trace_id) const;

    /// Serialize a trace to JSON.
    static std::string serialize_json(const Trace& trace);

    /// Deserialize a trace from JSON.
    static Result<Trace, std::string> deserialize_json(const std::string& data);

    /// Serialize to compact binary format.
    static std::vector<uint8_t> serialize_binary(const Trace& trace);

    /// Deserialize from binary format.
    static Result<Trace, std::string> deserialize_binary(const std::vector<uint8_t>& data);

    std::filesystem::path store_dir_;
};

} // namespace straylight
