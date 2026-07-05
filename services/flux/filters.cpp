// services/flux/filters.cpp
#include "filters.h"
#include <straylight/log.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace straylight {

// ---------------------------------------------------------------------------
// FilterEngine
// ---------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

static bool try_parse_double(const std::string& s, double& out) {
    try {
        size_t pos;
        out = std::stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

Result<std::vector<FilterCondition>, SLError> FilterEngine::parse(const std::string& expr) {
    std::vector<FilterCondition> conditions;

    // Split by " and " or " && "
    std::string remaining = expr;
    while (!remaining.empty()) {
        std::string part;
        auto and_pos = remaining.find(" and ");
        auto amp_pos = remaining.find(" && ");

        size_t split_pos = std::string::npos;
        size_t split_len = 0;

        if (and_pos != std::string::npos && (amp_pos == std::string::npos || and_pos < amp_pos)) {
            split_pos = and_pos;
            split_len = 5; // " and "
        } else if (amp_pos != std::string::npos) {
            split_pos = amp_pos;
            split_len = 4; // " && "
        }

        if (split_pos != std::string::npos) {
            part = trim(remaining.substr(0, split_pos));
            remaining = trim(remaining.substr(split_pos + split_len));
        } else {
            part = trim(remaining);
            remaining.clear();
        }

        if (part.empty()) continue;

        // Parse a single condition: field op value
        // Supported operators: ==, !=, >, >=, <, <=, contains, regex
        FilterCondition cond;

        // Try each operator (longest first to avoid partial matches)
        struct OpPattern {
            const char* text;
            FilterOp op;
        };
        static const OpPattern patterns[] = {
            {">=", FilterOp::GreaterEqual},
            {"<=", FilterOp::LessEqual},
            {"!=", FilterOp::NotEqual},
            {"==", FilterOp::Equal},
            {">",  FilterOp::GreaterThan},
            {"<",  FilterOp::LessThan},
            {" contains ", FilterOp::Contains},
            {" regex ", FilterOp::Regex},
        };

        bool found = false;
        for (const auto& pat : patterns) {
            std::string op_str(pat.text);
            auto pos = part.find(op_str);
            if (pos != std::string::npos) {
                cond.field = trim(part.substr(0, pos));
                cond.op = pat.op;
                cond.value = trim(part.substr(pos + op_str.size()));

                // Strip quotes from value if present
                if (cond.value.size() >= 2 &&
                    ((cond.value.front() == '"' && cond.value.back() == '"') ||
                     (cond.value.front() == '\'' && cond.value.back() == '\''))) {
                    cond.value = cond.value.substr(1, cond.value.size() - 2);
                }

                found = true;
                break;
            }
        }

        if (!found) {
            return Result<std::vector<FilterCondition>, SLError>::error(
                SLError{SLErrorCode::ParseError,
                        "Cannot parse filter condition: '" + part + "'"});
        }

        if (cond.field.empty()) {
            return Result<std::vector<FilterCondition>, SLError>::error(
                SLError{SLErrorCode::ParseError,
                        "Empty field name in filter: '" + part + "'"});
        }

        conditions.push_back(std::move(cond));
    }

    if (conditions.empty()) {
        return Result<std::vector<FilterCondition>, SLError>::error(
            SLError{SLErrorCode::ParseError, "Empty filter expression"});
    }

    return Result<std::vector<FilterCondition>, SLError>::ok(std::move(conditions));
}

const nlohmann::json* FilterEngine::resolve_path(const nlohmann::json& root,
                                                   const std::string& path) {
    const nlohmann::json* current = &root;

    std::istringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '.')) {
        if (segment.empty()) continue;

        if (current->is_object() && current->contains(segment)) {
            current = &(*current)[segment];
        } else {
            return nullptr;
        }
    }

    return current;
}

bool FilterEngine::evaluate_one(const FilterCondition& cond, const nlohmann::json& payload) {
    const nlohmann::json* val = resolve_path(payload, cond.field);
    if (!val) return false;

    switch (cond.op) {
    case FilterOp::Equal: {
        if (val->is_number()) {
            double d;
            if (try_parse_double(cond.value, d)) {
                return val->get<double>() == d;
            }
        }
        if (val->is_string()) {
            return val->get<std::string>() == cond.value;
        }
        if (val->is_boolean()) {
            bool b = (cond.value == "true" || cond.value == "1");
            return val->get<bool>() == b;
        }
        return val->dump() == cond.value;
    }

    case FilterOp::NotEqual: {
        if (val->is_number()) {
            double d;
            if (try_parse_double(cond.value, d)) {
                return val->get<double>() != d;
            }
        }
        if (val->is_string()) {
            return val->get<std::string>() != cond.value;
        }
        return val->dump() != cond.value;
    }

    case FilterOp::GreaterThan: {
        if (val->is_number()) {
            double d;
            if (try_parse_double(cond.value, d)) {
                return val->get<double>() > d;
            }
        }
        if (val->is_string()) {
            return val->get<std::string>() > cond.value;
        }
        return false;
    }

    case FilterOp::GreaterEqual: {
        if (val->is_number()) {
            double d;
            if (try_parse_double(cond.value, d)) {
                return val->get<double>() >= d;
            }
        }
        if (val->is_string()) {
            return val->get<std::string>() >= cond.value;
        }
        return false;
    }

    case FilterOp::LessThan: {
        if (val->is_number()) {
            double d;
            if (try_parse_double(cond.value, d)) {
                return val->get<double>() < d;
            }
        }
        if (val->is_string()) {
            return val->get<std::string>() < cond.value;
        }
        return false;
    }

    case FilterOp::LessEqual: {
        if (val->is_number()) {
            double d;
            if (try_parse_double(cond.value, d)) {
                return val->get<double>() <= d;
            }
        }
        if (val->is_string()) {
            return val->get<std::string>() <= cond.value;
        }
        return false;
    }

    case FilterOp::Contains: {
        if (val->is_string()) {
            return val->get<std::string>().find(cond.value) != std::string::npos;
        }
        if (val->is_array()) {
            for (const auto& elem : *val) {
                if (elem.is_string() && elem.get<std::string>() == cond.value) return true;
                if (elem.dump() == cond.value) return true;
            }
        }
        return false;
    }

    case FilterOp::Regex: {
        if (!val->is_string()) return false;
        try {
            std::regex re(cond.value, std::regex::ECMAScript);
            return std::regex_search(val->get<std::string>(), re);
        } catch (const std::regex_error&) {
            return false;
        }
    }
    }

    return false;
}

bool FilterEngine::evaluate(const std::vector<FilterCondition>& conditions,
                             const nlohmann::json& payload) {
    for (const auto& cond : conditions) {
        if (!evaluate_one(cond, payload)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// TransformEngine
// ---------------------------------------------------------------------------

std::string TransformEngine::normalize_path(const std::string& path) {
    if (!path.empty() && path[0] == '.') {
        return path.substr(1);
    }
    return path;
}

Result<nlohmann::json, SLError> TransformEngine::extract(const nlohmann::json& payload,
                                                           const std::string& path) {
    std::string normalized = normalize_path(path);
    if (normalized.empty()) {
        return Result<nlohmann::json, SLError>::ok(payload);
    }

    const nlohmann::json* val = FilterEngine::resolve_path(payload, normalized);
    if (!val) {
        return Result<nlohmann::json, SLError>::error(
            SLError{SLErrorCode::NotFound, "Path '" + path + "' not found in payload"});
    }

    return Result<nlohmann::json, SLError>::ok(*val);
}

Result<AggregationType, SLError> TransformEngine::parse_aggregation_type(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "min") return Result<AggregationType, SLError>::ok(AggregationType::Min);
    if (lower == "max") return Result<AggregationType, SLError>::ok(AggregationType::Max);
    if (lower == "avg" || lower == "average") return Result<AggregationType, SLError>::ok(AggregationType::Avg);
    if (lower == "sum") return Result<AggregationType, SLError>::ok(AggregationType::Sum);
    if (lower == "count") return Result<AggregationType, SLError>::ok(AggregationType::Count);

    return Result<AggregationType, SLError>::error(
        SLError{SLErrorCode::ParseError, "Unknown aggregation type: '" + s + "'"});
}

Result<double, SLError> TransformEngine::aggregate(const std::vector<nlohmann::json>& window,
                                                     const AggregationSpec& spec) {
    std::string field_path = normalize_path(spec.field);

    // Collect numeric values from the window
    std::vector<double> values;
    values.reserve(window.size());

    for (const auto& payload : window) {
        const nlohmann::json* val = FilterEngine::resolve_path(payload, field_path);
        if (val && val->is_number()) {
            values.push_back(val->get<double>());
        }
    }

    if (spec.type == AggregationType::Count) {
        return Result<double, SLError>::ok(static_cast<double>(values.size()));
    }

    if (values.empty()) {
        return Result<double, SLError>::error(
            SLError{SLErrorCode::NotFound, "No numeric values found for field '" + spec.field + "'"});
    }

    switch (spec.type) {
    case AggregationType::Min: {
        double result = values[0];
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] < result) result = values[i];
        }
        return Result<double, SLError>::ok(result);
    }

    case AggregationType::Max: {
        double result = values[0];
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] > result) result = values[i];
        }
        return Result<double, SLError>::ok(result);
    }

    case AggregationType::Avg: {
        double sum = 0.0;
        for (double v : values) sum += v;
        return Result<double, SLError>::ok(sum / static_cast<double>(values.size()));
    }

    case AggregationType::Sum: {
        double sum = 0.0;
        for (double v : values) sum += v;
        return Result<double, SLError>::ok(sum);
    }

    case AggregationType::Count:
        return Result<double, SLError>::ok(static_cast<double>(values.size()));
    }

    return Result<double, SLError>::error(
        SLError{SLErrorCode::Internal, "Unreachable aggregation branch"});
}

} // namespace straylight
