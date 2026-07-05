// services/flux/filters.h
// Filter expressions and JSON transforms for stream data.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <nlohmann/json.hpp>

#include <regex>
#include <string>
#include <vector>

namespace straylight {

/// Supported comparison operators in filter expressions.
enum class FilterOp {
    Equal,
    NotEqual,
    GreaterThan,
    GreaterEqual,
    LessThan,
    LessEqual,
    Contains,
    Regex,
};

/// A parsed filter condition: field op value.
struct FilterCondition {
    std::string field;     // JSON path (dot-separated)
    FilterOp op;
    std::string value;     // String representation of comparison value
};

/// Evaluate filter expressions against JSON payloads.
class FilterEngine {
public:
    /// Parse a filter expression string like "temp > 40" or "name contains foo".
    /// Supports: ==, !=, >, >=, <, <=, contains, regex
    static Result<std::vector<FilterCondition>, SLError> parse(const std::string& expr);

    /// Evaluate a set of filter conditions against a JSON payload.
    /// Returns true if all conditions match (AND logic).
    static bool evaluate(const std::vector<FilterCondition>& conditions,
                         const nlohmann::json& payload);

    /// Evaluate a single condition against a JSON payload.
    static bool evaluate_one(const FilterCondition& cond, const nlohmann::json& payload);

    /// Resolve a dotted JSON path to a value.
    static const nlohmann::json* resolve_path(const nlohmann::json& root,
                                               const std::string& path);
};

/// Aggregation window types for transforms.
enum class AggregationType {
    Min,
    Max,
    Avg,
    Sum,
    Count,
};

/// A single aggregation spec: which field, which function, over how many events.
struct AggregationSpec {
    std::string field;
    AggregationType type;
    size_t window_size = 10;
};

/// Transform engine for jq-like JSON path extraction and aggregation.
class TransformEngine {
public:
    /// Extract a field from JSON using dot-notation path (jq-like ".field.sub").
    static Result<nlohmann::json, SLError> extract(const nlohmann::json& payload,
                                                     const std::string& path);

    /// Apply aggregation over a window of events.
    static Result<double, SLError> aggregate(const std::vector<nlohmann::json>& window,
                                              const AggregationSpec& spec);

    /// Parse a jq-like path expression. Strips leading dot if present.
    static std::string normalize_path(const std::string& path);

    /// Parse aggregation type from string.
    static Result<AggregationType, SLError> parse_aggregation_type(const std::string& s);
};

} // namespace straylight
