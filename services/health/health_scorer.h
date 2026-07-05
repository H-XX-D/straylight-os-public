// services/health/health_scorer.h
// Weighted health scoring and history tracking.
#pragma once

#include "checks.h"

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// A timestamped health snapshot.
struct HealthSnapshot {
    std::string timestamp;  // ISO 8601
    int overall_score = 0;
    HealthStatus overall_status = HealthStatus::Ok;
    std::vector<CheckResult> checks;
};

/// Alert threshold configuration for a check.
struct AlertThreshold {
    std::string check_name;
    int warn_below = 70;
    int critical_below = 30;
};

/// Computes weighted overall health score and tracks history.
class HealthScorer {
public:
    /// Maximum history snapshots to retain.
    static constexpr size_t kMaxHistory = 1440;  // 24h at 1-minute intervals

    /// Configure alert thresholds.
    void set_threshold(const std::string& check_name, int warn_below, int critical_below);

    /// Compute overall health score from check results.
    HealthSnapshot score(const std::vector<CheckResult>& checks);

    /// Get the latest snapshot.
    HealthSnapshot latest() const;

    /// Get history for trend analysis.
    std::vector<HealthSnapshot> history(int limit = 60) const;

    /// Generate a detailed HTML report.
    std::string generate_html_report() const;

    /// Generate a plain-text status report.
    std::string format_status(const HealthSnapshot& snap) const;

    /// Format a single check result.
    static std::string format_check(const CheckResult& cr);

    /// Get the status string for a HealthStatus enum.
    static std::string status_string(HealthStatus status);

private:
    /// ISO 8601 timestamp.
    static std::string now_iso8601();

    mutable std::mutex mu_;
    std::deque<HealthSnapshot> history_;
    std::map<std::string, AlertThreshold> thresholds_;
};

} // namespace straylight
