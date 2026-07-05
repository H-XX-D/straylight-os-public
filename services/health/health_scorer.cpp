// services/health/health_scorer.cpp
// Health scoring and reporting implementation.
#include "health_scorer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>

namespace straylight {

std::string HealthScorer::now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%FT%TZ");
    return oss.str();
}

std::string HealthScorer::status_string(HealthStatus status) {
    switch (status) {
        case HealthStatus::Ok: return "ok";
        case HealthStatus::Warn: return "warn";
        case HealthStatus::Critical: return "critical";
    }
    return "unknown";
}

void HealthScorer::set_threshold(const std::string& check_name,
                                  int warn_below, int critical_below) {
    std::lock_guard<std::mutex> lk(mu_);
    AlertThreshold at;
    at.check_name = check_name;
    at.warn_below = warn_below;
    at.critical_below = critical_below;
    thresholds_[check_name] = at;
}

HealthSnapshot HealthScorer::score(const std::vector<CheckResult>& checks) {
    HealthSnapshot snap;
    snap.timestamp = now_iso8601();
    snap.checks = checks;

    // Compute weighted average
    double total_weight = 0.0;
    double weighted_sum = 0.0;
    HealthStatus worst = HealthStatus::Ok;

    for (const auto& cr : checks) {
        weighted_sum += cr.score * cr.weight;
        total_weight += cr.weight;

        if (cr.status == HealthStatus::Critical) {
            worst = HealthStatus::Critical;
        } else if (cr.status == HealthStatus::Warn && worst != HealthStatus::Critical) {
            worst = HealthStatus::Warn;
        }

        // Apply custom thresholds
        auto it = thresholds_.find(cr.name);
        if (it != thresholds_.end()) {
            if (cr.score < it->second.critical_below && worst != HealthStatus::Critical) {
                worst = HealthStatus::Critical;
            } else if (cr.score < it->second.warn_below && worst == HealthStatus::Ok) {
                worst = HealthStatus::Warn;
            }
        }
    }

    snap.overall_score = (total_weight > 0.0)
        ? static_cast<int>(std::round(weighted_sum / total_weight))
        : 0;
    snap.overall_status = worst;

    // Clamp to 0-100
    snap.overall_score = std::clamp(snap.overall_score, 0, 100);

    // Store in history
    {
        std::lock_guard<std::mutex> lk(mu_);
        history_.push_back(snap);
        while (history_.size() > kMaxHistory) {
            history_.pop_front();
        }
    }

    return snap;
}

HealthSnapshot HealthScorer::latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (history_.empty()) {
        HealthSnapshot empty;
        empty.overall_score = -1;
        return empty;
    }
    return history_.back();
}

std::vector<HealthSnapshot> HealthScorer::history(int limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    int count = std::min(limit, static_cast<int>(history_.size()));
    return std::vector<HealthSnapshot>(history_.end() - count, history_.end());
}

std::string HealthScorer::format_check(const CheckResult& cr) {
    std::ostringstream oss;
    std::string status_tag;
    if (cr.status == HealthStatus::Ok) status_tag = "  [OK]  ";
    else if (cr.status == HealthStatus::Warn) status_tag = " [WARN] ";
    else status_tag = " [CRIT] ";

    // Pad name to 20 chars
    std::string name = cr.name;
    if (name.size() < 20) name += std::string(20 - name.size(), ' ');

    // Score bar
    std::string bar;
    int filled = cr.score / 5; // 20 chars wide
    bar += "[";
    for (int i = 0; i < 20; ++i) {
        bar += (i < filled) ? "#" : " ";
    }
    bar += "]";

    std::string score_str = std::to_string(cr.score);
    if (score_str.size() < 3) score_str = std::string(3 - score_str.size(), ' ') + score_str;

    oss << status_tag << name << " " << score_str << " " << bar << "  " << cr.detail;
    return oss.str();
}

std::string HealthScorer::format_status(const HealthSnapshot& snap) const {
    std::ostringstream oss;

    oss << "\n  StrayLight Health Dashboard\n";
    oss << "  " << snap.timestamp << "\n\n";

    // Overall score with bar
    std::string overall_bar;
    overall_bar += "[";
    int filled = snap.overall_score / 2; // 50 chars wide
    for (int i = 0; i < 50; ++i) {
        overall_bar += (i < filled) ? "=" : " ";
    }
    overall_bar += "]";

    oss << "  Overall: " << snap.overall_score << "/100 "
        << overall_bar << " " << status_string(snap.overall_status) << "\n\n";

    // Individual checks
    for (const auto& cr : snap.checks) {
        oss << "  " << format_check(cr) << "\n";
    }

    oss << "\n";
    return oss.str();
}

// ---------------------------------------------------------------------------
// HTML Report
// ---------------------------------------------------------------------------

std::string HealthScorer::generate_html_report() const {
    auto snap = latest();
    auto hist = history(60);

    std::ostringstream html;
    html << "<!DOCTYPE html>\n<html><head>\n"
         << "<meta charset='utf-8'>\n"
         << "<title>StrayLight Health Report</title>\n"
         << "<style>\n"
         << "  body { font-family: 'SF Mono', 'Fira Code', monospace; "
         << "    background: #0a0a0a; color: #e0e0e0; padding: 40px; }\n"
         << "  h1 { color: #00ff88; border-bottom: 1px solid #333; padding-bottom: 10px; }\n"
         << "  h2 { color: #00aaff; margin-top: 30px; }\n"
         << "  .score-big { font-size: 72px; font-weight: bold; margin: 20px 0; }\n"
         << "  .ok { color: #00ff88; }\n"
         << "  .warn { color: #ffaa00; }\n"
         << "  .critical { color: #ff3333; }\n"
         << "  table { border-collapse: collapse; width: 100%; margin: 20px 0; }\n"
         << "  th, td { padding: 10px 15px; text-align: left; border-bottom: 1px solid #222; }\n"
         << "  th { color: #888; font-weight: normal; text-transform: uppercase; font-size: 12px; }\n"
         << "  .bar { display: inline-block; height: 16px; border-radius: 3px; }\n"
         << "  .bar-bg { background: #222; width: 200px; display: inline-block; "
         << "    height: 16px; border-radius: 3px; }\n"
         << "  .trend { margin: 30px 0; }\n"
         << "  .trend-point { display: inline-block; width: 8px; margin: 0 1px; "
         << "    border-radius: 2px; vertical-align: bottom; }\n"
         << "  .meta { color: #666; font-size: 12px; margin-top: 40px; }\n"
         << "</style></head><body>\n";

    html << "<h1>StrayLight Health Report</h1>\n";
    html << "<p>Generated: " << snap.timestamp << "</p>\n";

    // Overall score
    std::string score_class;
    if (snap.overall_status == HealthStatus::Ok) score_class = "ok";
    else if (snap.overall_status == HealthStatus::Warn) score_class = "warn";
    else score_class = "critical";

    html << "<div class='score-big " << score_class << "'>"
         << snap.overall_score << "/100</div>\n";
    html << "<p>Status: <span class='" << score_class << "'>"
         << status_string(snap.overall_status) << "</span></p>\n";

    // Check details table
    html << "<h2>Subsystem Checks</h2>\n";
    html << "<table><tr><th>Status</th><th>Check</th><th>Score</th>"
         << "<th>Bar</th><th>Detail</th></tr>\n";

    for (const auto& cr : snap.checks) {
        std::string cls;
        if (cr.status == HealthStatus::Ok) cls = "ok";
        else if (cr.status == HealthStatus::Warn) cls = "warn";
        else cls = "critical";

        int bar_width = cr.score * 2; // max 200px

        html << "<tr><td class='" << cls << "'>" << status_string(cr.status) << "</td>"
             << "<td>" << cr.name << "</td>"
             << "<td>" << cr.score << "</td>"
             << "<td><div class='bar-bg'>"
             << "<div class='bar " << cls << "' style='width:" << bar_width << "px'></div>"
             << "</div></td>"
             << "<td>" << cr.detail << "</td></tr>\n";
    }
    html << "</table>\n";

    // Trend chart (ASCII-art style with colored divs)
    if (hist.size() > 1) {
        html << "<h2>Score Trend (last " << hist.size() << " readings)</h2>\n";
        html << "<div class='trend' style='height: 100px; position: relative;'>\n";

        for (const auto& h : hist) {
            int height = h.overall_score;
            std::string cls2;
            if (h.overall_status == HealthStatus::Ok) cls2 = "ok";
            else if (h.overall_status == HealthStatus::Warn) cls2 = "warn";
            else cls2 = "critical";

            html << "<div class='trend-point " << cls2
                 << "' style='height:" << height << "px; background:currentColor;' "
                 << "title='" << h.timestamp << ": " << h.overall_score << "'></div>";
        }
        html << "\n</div>\n";
    }

    html << "<p class='meta'>StrayLight OS Health Daemon v1.0</p>\n";
    html << "</body></html>\n";

    return html.str();
}

} // namespace straylight
