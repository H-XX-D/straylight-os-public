// tools/bench/report.h
// HTML report generator for StrayLight benchmark results.
#pragma once

#include "benchmarks.h"

#include <straylight/result.h>

#include <string>

namespace straylight {

/// Generates self-contained HTML benchmark reports with colored bars
/// and comparison tables.
class ReportGenerator {
public:
    /// Generate a full HTML report from benchmark results.
    static Result<std::string, std::string> generate_html(const BenchReport& report);

    /// Generate an HTML comparison report between two benchmark runs.
    static Result<std::string, std::string> generate_comparison_html(
        const BenchReport& baseline, const BenchReport& current);

    /// Save benchmark results as JSON.
    static Result<void, std::string> save_json(const BenchReport& report,
                                                const std::string& path);

    /// Load benchmark results from JSON.
    static Result<BenchReport, std::string> load_json(const std::string& path);
};

} // namespace straylight
