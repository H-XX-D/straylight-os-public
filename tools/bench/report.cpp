// tools/bench/report.cpp
// HTML report generator implementation.
#include "report.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static std::string escape_html(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default:  out += c; break;
        }
    }
    return out;
}

static std::string bar_color(const std::string& category) {
    if (category == "cpu") return "#4a90d9";
    if (category == "memory") return "#50c878";
    if (category == "storage") return "#f5a623";
    if (category == "gpu") return "#d0021b";
    if (category == "network") return "#9013fe";
    if (category == "ml") return "#ff6b6b";
    return "#999999";
}

static std::string format_bytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f TB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024 * 1024));
        return buf;
    }
    if (bytes >= 1024ULL * 1024 * 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024));
    return buf;
}

// --------------------------------------------------------------------------
// HTML generation
// --------------------------------------------------------------------------

Result<std::string, std::string> ReportGenerator::generate_html(
    const BenchReport& report) {

    if (report.results.empty()) {
        return Result<std::string, std::string>::error("No benchmark results to report");
    }

    std::ostringstream html;

    html << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>StrayLight Benchmark Report</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    background: #0a0a0f; color: #e0e0e0;
    max-width: 960px; margin: 0 auto; padding: 24px;
  }
  h1 { color: #00d4ff; margin-bottom: 8px; font-size: 28px; }
  h2 { color: #88ccff; margin: 32px 0 16px; font-size: 20px;
       border-bottom: 1px solid #333; padding-bottom: 8px; }
  .sysinfo { background: #161620; border-radius: 8px; padding: 16px; margin: 16px 0;
             display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  .sysinfo span { color: #888; }
  .sysinfo strong { color: #ccc; }
  .bench-item {
    background: #161620; border-radius: 6px; padding: 12px 16px; margin: 8px 0;
  }
  .bench-header { display: flex; justify-content: space-between; margin-bottom: 6px; }
  .bench-name { font-weight: 600; color: #ddd; }
  .bench-value { font-weight: 700; font-size: 18px; }
  .bench-desc { color: #888; font-size: 13px; margin-bottom: 8px; }
  .bar-container {
    background: #222; border-radius: 4px; height: 8px; overflow: hidden;
  }
  .bar-fill { height: 100%; border-radius: 4px; transition: width 0.3s; }
  .timestamp { color: #666; font-size: 12px; margin-top: 32px; text-align: center; }
  .category-tag {
    display: inline-block; padding: 2px 8px; border-radius: 3px;
    font-size: 11px; text-transform: uppercase; color: #fff; margin-right: 8px;
  }
</style>
</head>
<body>
<h1>StrayLight Benchmark Report</h1>
)";

    // System info header
    html << "<div class=\"sysinfo\">\n";
    html << "  <div><span>Hostname:</span> <strong>"
         << escape_html(report.hostname) << "</strong></div>\n";
    html << "  <div><span>OS:</span> <strong>"
         << escape_html(report.os_version) << "</strong></div>\n";
    html << "  <div><span>Kernel:</span> <strong>"
         << escape_html(report.kernel_version) << "</strong></div>\n";
    html << "  <div><span>CPU:</span> <strong>"
         << escape_html(report.cpu_model) << "</strong></div>\n";
    html << "  <div><span>Cores:</span> <strong>"
         << report.cpu_cores << "</strong></div>\n";
    html << "  <div><span>Memory:</span> <strong>"
         << format_bytes(report.memory_bytes) << "</strong></div>\n";
    html << "  <div><span>GPU:</span> <strong>"
         << escape_html(report.gpu_model) << "</strong></div>\n";
    html << "  <div><span>Date:</span> <strong>"
         << escape_html(report.timestamp) << "</strong></div>\n";
    html << "</div>\n";

    // Group results by category
    std::vector<std::string> categories = {
        "cpu", "memory", "storage", "gpu", "network", "ml"
    };
    std::vector<std::string> category_names = {
        "CPU", "Memory", "Storage", "GPU", "Network", "Machine Learning"
    };

    for (size_t ci = 0; ci < categories.size(); ++ci) {
        std::vector<const BenchResult*> cat_results;
        for (const auto& r : report.results) {
            if (r.category == categories[ci]) {
                cat_results.push_back(&r);
            }
        }

        if (cat_results.empty()) continue;

        html << "<h2>" << category_names[ci] << "</h2>\n";

        // Find max value in category for bar scaling
        double max_val = 0;
        for (const auto* br : cat_results) {
            if (br->value > max_val) max_val = br->value;
        }
        if (max_val <= 0) max_val = 1;

        for (const auto* br : cat_results) {
            double bar_pct = (br->value > 0) ? (br->value / max_val * 100.0) : 0;
            if (bar_pct > 100) bar_pct = 100;
            std::string color = bar_color(categories[ci]);

            char val_buf[32];
            std::snprintf(val_buf, sizeof(val_buf), "%.2f", br->value);

            html << "<div class=\"bench-item\">\n";
            html << "  <div class=\"bench-header\">\n";
            html << "    <span class=\"bench-name\">"
                 << "<span class=\"category-tag\" style=\"background:" << color << "\">"
                 << categories[ci] << "</span>"
                 << escape_html(br->name) << "</span>\n";
            html << "    <span class=\"bench-value\" style=\"color:" << color << "\">"
                 << val_buf << " " << escape_html(br->unit) << "</span>\n";
            html << "  </div>\n";
            html << "  <div class=\"bench-desc\">"
                 << escape_html(br->description) << "</div>\n";
            html << "  <div class=\"bar-container\">\n";
            html << "    <div class=\"bar-fill\" style=\"width:"
                 << static_cast<int>(bar_pct) << "%;background:" << color
                 << "\"></div>\n";
            html << "  </div>\n";
            html << "</div>\n";
        }
    }

    html << "<div class=\"timestamp\">Generated by straylight-bench | "
         << escape_html(report.timestamp) << "</div>\n";
    html << "</body>\n</html>\n";

    return Result<std::string, std::string>::ok(html.str());
}

Result<std::string, std::string> ReportGenerator::generate_comparison_html(
    const BenchReport& baseline, const BenchReport& current) {

    std::ostringstream html;

    html << R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>StrayLight Benchmark Comparison</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    background: #0a0a0f; color: #e0e0e0;
    max-width: 960px; margin: 0 auto; padding: 24px;
  }
  h1 { color: #00d4ff; margin-bottom: 16px; }
  table { width: 100%; border-collapse: collapse; margin: 16px 0; }
  th { text-align: left; padding: 10px 12px; background: #1a1a2e; color: #88ccff;
       border-bottom: 2px solid #333; }
  td { padding: 10px 12px; border-bottom: 1px solid #222; }
  tr:hover { background: #1a1a2e; }
  .better { color: #50c878; }
  .worse { color: #ff6b6b; }
  .same { color: #888; }
  .info { background: #161620; border-radius: 8px; padding: 12px 16px; margin: 8px 0; }
</style>
</head>
<body>
<h1>StrayLight Benchmark Comparison</h1>
)";

    html << "<div class=\"info\">\n";
    html << "  <strong>Baseline:</strong> " << escape_html(baseline.hostname)
         << " @ " << escape_html(baseline.timestamp) << "<br>\n";
    html << "  <strong>Current:</strong> " << escape_html(current.hostname)
         << " @ " << escape_html(current.timestamp) << "\n";
    html << "</div>\n";

    html << "<table>\n";
    html << "<tr><th>Benchmark</th><th>Baseline</th><th>Current</th>"
         << "<th>Change</th><th>Unit</th></tr>\n";

    for (const auto& cur : current.results) {
        const BenchResult* base = nullptr;
        for (const auto& b : baseline.results) {
            if (b.name == cur.name) { base = &b; break; }
        }

        html << "<tr>\n";
        html << "  <td>" << escape_html(cur.name) << "</td>\n";

        if (base) {
            char bb[32], cb[32];
            std::snprintf(bb, sizeof(bb), "%.2f", base->value);
            std::snprintf(cb, sizeof(cb), "%.2f", cur.value);

            double pct = 0;
            if (base->value != 0) {
                pct = ((cur.value - base->value) / base->value) * 100.0;
            }

            bool lower_is_better = (cur.unit == "us" || cur.unit == "ms" ||
                                    cur.unit == "ns");
            std::string cls;
            if (lower_is_better) {
                cls = (pct < -2) ? "better" : (pct > 2) ? "worse" : "same";
            } else {
                cls = (pct > 2) ? "better" : (pct < -2) ? "worse" : "same";
            }

            char pct_buf[16];
            std::snprintf(pct_buf, sizeof(pct_buf), "%+.1f%%", pct);

            html << "  <td>" << bb << "</td>\n";
            html << "  <td>" << cb << "</td>\n";
            html << "  <td class=\"" << cls << "\">" << pct_buf << "</td>\n";
        } else {
            char cb[32];
            std::snprintf(cb, sizeof(cb), "%.2f", cur.value);
            html << "  <td>n/a</td>\n";
            html << "  <td>" << cb << "</td>\n";
            html << "  <td class=\"same\">new</td>\n";
        }

        html << "  <td>" << escape_html(cur.unit) << "</td>\n";
        html << "</tr>\n";
    }

    html << "</table>\n</body>\n</html>\n";

    return Result<std::string, std::string>::ok(html.str());
}

// --------------------------------------------------------------------------
// JSON serialization
// --------------------------------------------------------------------------

Result<void, std::string> ReportGenerator::save_json(const BenchReport& report,
                                                      const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) {
        return Result<void, std::string>::error("Cannot open " + path + " for writing");
    }

    f << "{\n";
    f << "  \"hostname\": \"" << report.hostname << "\",\n";
    f << "  \"os_version\": \"" << report.os_version << "\",\n";
    f << "  \"kernel_version\": \"" << report.kernel_version << "\",\n";
    f << "  \"cpu_model\": \"" << report.cpu_model << "\",\n";
    f << "  \"cpu_cores\": " << report.cpu_cores << ",\n";
    f << "  \"memory_bytes\": " << report.memory_bytes << ",\n";
    f << "  \"gpu_model\": \"" << report.gpu_model << "\",\n";
    f << "  \"timestamp\": \"" << report.timestamp << "\",\n";
    f << "  \"results\": [\n";

    for (size_t i = 0; i < report.results.size(); ++i) {
        const auto& r = report.results[i];
        f << "    {\n";
        f << "      \"name\": \"" << r.name << "\",\n";
        f << "      \"category\": \"" << r.category << "\",\n";
        f << "      \"value\": " << r.value << ",\n";
        f << "      \"unit\": \"" << r.unit << "\",\n";
        f << "      \"duration_seconds\": " << r.duration_seconds << ",\n";
        f << "      \"description\": \"" << r.description << "\"\n";
        f << "    }";
        if (i + 1 < report.results.size()) f << ",";
        f << "\n";
    }

    f << "  ]\n";
    f << "}\n";

    f.close();
    return Result<void, std::string>::ok();
}

Result<BenchReport, std::string> ReportGenerator::load_json(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return Result<BenchReport, std::string>::error("Cannot open " + path);
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();

    // Simple JSON parsing — we use a manual approach since nlohmann/json
    // may not be linked in the CLI tool. But since we link straylight-common
    // which includes nlohmann/json, we can use it.
    try {
        // Parse with nlohmann (available via straylight-common)
        // We include the header indirectly through the common library
        // For robustness, do a basic manual parse

        BenchReport report;

        auto extract_string = [&](const std::string& key) -> std::string {
            std::string search = "\"" + key + "\": \"";
            auto pos = content.find(search);
            if (pos == std::string::npos) return "";
            pos += search.size();
            auto end = content.find('"', pos);
            if (end == std::string::npos) return "";
            return content.substr(pos, end - pos);
        };

        auto extract_number = [&](const std::string& key) -> double {
            std::string search = "\"" + key + "\": ";
            auto pos = content.find(search);
            if (pos == std::string::npos) return 0;
            pos += search.size();
            return std::atof(content.c_str() + pos);
        };

        report.hostname = extract_string("hostname");
        report.os_version = extract_string("os_version");
        report.kernel_version = extract_string("kernel_version");
        report.cpu_model = extract_string("cpu_model");
        report.cpu_cores = static_cast<uint32_t>(extract_number("cpu_cores"));
        report.memory_bytes = static_cast<uint64_t>(extract_number("memory_bytes"));
        report.gpu_model = extract_string("gpu_model");
        report.timestamp = extract_string("timestamp");

        // Parse results array
        std::string results_key = "\"results\": [";
        auto results_pos = content.find(results_key);
        if (results_pos != std::string::npos) {
            size_t pos = results_pos + results_key.size();
            while (pos < content.size()) {
                auto obj_start = content.find('{', pos);
                if (obj_start == std::string::npos) break;
                auto obj_end = content.find('}', obj_start);
                if (obj_end == std::string::npos) break;

                std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

                BenchResult br;
                auto obj_str = [&](const std::string& key) -> std::string {
                    std::string s = "\"" + key + "\": \"";
                    auto p = obj.find(s);
                    if (p == std::string::npos) return "";
                    p += s.size();
                    auto e = obj.find('"', p);
                    return (e != std::string::npos) ? obj.substr(p, e - p) : "";
                };

                auto obj_num = [&](const std::string& key) -> double {
                    std::string s = "\"" + key + "\": ";
                    auto p = obj.find(s);
                    if (p == std::string::npos) return 0;
                    p += s.size();
                    return std::atof(obj.c_str() + p);
                };

                br.name = obj_str("name");
                br.category = obj_str("category");
                br.value = obj_num("value");
                br.unit = obj_str("unit");
                br.duration_seconds = obj_num("duration_seconds");
                br.description = obj_str("description");

                if (!br.name.empty()) {
                    report.results.push_back(std::move(br));
                }

                pos = obj_end + 1;
            }
        }

        return Result<BenchReport, std::string>::ok(std::move(report));

    } catch (const std::exception& e) {
        return Result<BenchReport, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }
}

} // namespace straylight
