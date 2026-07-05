// tools/trace/report.h
// Report generation from trace data — summary, flamegraph, chrome trace format.
#pragma once

#include "tracer.h"
#include <straylight/result.h>
#include <straylight/error.h>

#include <string>

namespace straylight {

/// Generates human-readable and machine-readable reports from trace data.
class TraceReport {
public:
    /// Generate a human-readable summary report to stdout.
    static void print_summary(const TraceData& data);

    /// Export trace data as Chrome Trace Format JSON (for chrome://tracing).
    static Result<void, SLError> export_chrome_trace(const TraceData& data,
                                                       const std::string& output_path);

    /// Export trace data as raw JSON for later analysis.
    static Result<void, SLError> export_json(const TraceData& data,
                                               const std::string& output_path);

    /// Import trace data from a previously exported JSON file.
    static Result<TraceData, SLError> import_json(const std::string& input_path);

    /// Generate flamegraph-compatible output (folded stacks format).
    static Result<void, SLError> export_flamegraph(const TraceData& data,
                                                     const std::string& output_path);

private:
    /// Format a nanosecond duration as human-readable string.
    static std::string format_duration(uint64_t ns);

    /// Format byte count as human-readable string.
    static std::string format_bytes(uint64_t bytes);
};

} // namespace straylight
