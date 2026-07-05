// tools/log/log_engine.cpp
// Full implementation of unified log viewer for StrayLight OS.

#include "log_engine.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

LogEngine::LogEngine() {
    load_alerts();
}

LogEngine::~LogEngine() = default;

Result<std::string, std::string> LogEngine::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

LogEntry::Level LogEngine::parse_level(const std::string& level_str) {
    std::string lower = level_str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "debug" || lower == "7") return LogEntry::Level::Debug;
    if (lower == "info" || lower == "6" || lower == "notice" || lower == "5")
        return LogEntry::Level::Info;
    if (lower == "warning" || lower == "warn" || lower == "4")
        return LogEntry::Level::Warning;
    if (lower == "error" || lower == "err" || lower == "3")
        return LogEntry::Level::Error;
    if (lower == "critical" || lower == "crit" || lower == "2" ||
        lower == "alert" || lower == "1" || lower == "emerg" || lower == "0")
        return LogEntry::Level::Critical;

    return LogEntry::Level::Info;
}

std::string LogEngine::level_to_string(LogEntry::Level level) {
    switch (level) {
        case LogEntry::Level::Debug:    return "DEBUG";
        case LogEntry::Level::Info:     return "INFO";
        case LogEntry::Level::Warning:  return "WARN";
        case LogEntry::Level::Error:    return "ERROR";
        case LogEntry::Level::Critical: return "CRIT";
    }
    return "INFO";
}

// ---------------------------------------------------------------------------
// Journald parsing
// ---------------------------------------------------------------------------

LogEntry LogEngine::parse_journald_json(const std::string& json) const {
    LogEntry entry;
    entry.source = "journald";

    // Extract fields from JSON
    auto extract = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return "";
        auto colon = json.find(':', pos + search.size());
        if (colon == std::string::npos) return "";
        auto val_start = json.find_first_not_of(" \t", colon + 1);
        if (val_start == std::string::npos) return "";

        if (json[val_start] == '"') {
            auto val_end = json.find('"', val_start + 1);
            if (val_end == std::string::npos) return "";
            return json.substr(val_start + 1, val_end - val_start - 1);
        } else {
            auto val_end = json.find_first_of(",}", val_start);
            if (val_end == std::string::npos) return "";
            std::string val = json.substr(val_start, val_end - val_start);
            // Trim whitespace
            auto end = val.find_last_not_of(" \t\n\r");
            return (end != std::string::npos) ? val.substr(0, end + 1) : val;
        }
    };

    entry.message = extract("MESSAGE");
    entry.service = extract("SYSLOG_IDENTIFIER");
    if (entry.service.empty()) entry.service = extract("_COMM");
    entry.hostname = extract("_HOSTNAME");
    entry.unit = extract("_SYSTEMD_UNIT");

    std::string pid_str = extract("_PID");
    if (!pid_str.empty()) {
        try { entry.pid = std::stoi(pid_str); } catch (...) {}
    }

    std::string priority = extract("PRIORITY");
    if (!priority.empty()) entry.level = parse_level(priority);

    // Parse timestamp (microseconds since epoch)
    std::string ts_str = extract("__REALTIME_TIMESTAMP");
    if (!ts_str.empty()) {
        try {
            uint64_t usec = std::stoull(ts_str);
            auto dur = std::chrono::microseconds(usec);
            entry.timestamp = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(dur));
        } catch (...) {}
    }

    return entry;
}

Result<std::vector<LogEntry>, std::string> LogEngine::read_journald(
    const std::string& args) const {

    std::string cmd = "journalctl --output=json --no-pager " + args + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<std::vector<LogEntry>, std::string>::error(
            "journalctl failed: " + res.error());
    }

    std::vector<LogEntry> entries;
    std::istringstream stream(res.value());
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] != '{') continue;
        entries.push_back(parse_journald_json(line));
    }

    return Result<std::vector<LogEntry>, std::string>::ok(entries);
}

// ---------------------------------------------------------------------------
// Syslog parsing
// ---------------------------------------------------------------------------

LogEntry LogEngine::parse_syslog_line(const std::string& line) const {
    LogEntry entry;
    entry.source = "syslog";

    // Format: "Mar 15 10:30:45 hostname service[pid]: message"
    std::regex syslog_re(
        R"(^(\w+\s+\d+\s+\d+:\d+:\d+)\s+(\S+)\s+(\S+?)(?:\[(\d+)\])?\s*:\s*(.*)$)");
    std::smatch m;

    if (std::regex_match(line, m, syslog_re)) {
        // Parse timestamp
        std::string ts_str = m[1].str();
        std::tm tm{};
        std::istringstream ss(ts_str);
        ss >> std::get_time(&tm, "%b %d %H:%M:%S");
        // Set year to current year
        auto now = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm{};
        localtime_r(&now_t, &now_tm);
        tm.tm_year = now_tm.tm_year;
        entry.timestamp = std::chrono::system_clock::from_time_t(mktime(&tm));

        entry.hostname = m[2].str();
        entry.service = m[3].str();
        if (m[4].matched) {
            try { entry.pid = std::stoi(m[4].str()); } catch (...) {}
        }
        entry.message = m[5].str();

        // Infer level from message content
        std::string lower_msg = entry.message;
        std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(), ::tolower);
        if (lower_msg.find("crit") != std::string::npos ||
            lower_msg.find("fatal") != std::string::npos ||
            lower_msg.find("panic") != std::string::npos) {
            entry.level = LogEntry::Level::Critical;
        } else if (lower_msg.find("error") != std::string::npos ||
                   lower_msg.find("fail") != std::string::npos) {
            entry.level = LogEntry::Level::Error;
        } else if (lower_msg.find("warn") != std::string::npos) {
            entry.level = LogEntry::Level::Warning;
        } else if (lower_msg.find("debug") != std::string::npos) {
            entry.level = LogEntry::Level::Debug;
        }
    } else {
        entry.message = line;
    }

    return entry;
}

Result<std::vector<LogEntry>, std::string> LogEngine::read_syslog(
    const std::string& since, const std::string& until) const {

    std::vector<LogEntry> entries;
    std::string path = "/var/log/syslog";
    if (!fs::exists(path)) {
        return Result<std::vector<LogEntry>, std::string>::ok(entries);
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<std::vector<LogEntry>, std::string>::error(
            "cannot read " + path);
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        entries.push_back(parse_syslog_line(line));
    }

    return Result<std::vector<LogEntry>, std::string>::ok(entries);
}

// ---------------------------------------------------------------------------
// StrayLight log parsing
// ---------------------------------------------------------------------------

LogEntry LogEngine::parse_straylight_line(const std::string& line,
                                            const std::string& source) const {
    LogEntry entry;
    entry.source = "straylight";
    entry.service = source;

    // Format: "[2024-03-15 10:30:45.123] [LEVEL] [service] message"
    std::regex sl_re(
        R"(^\[(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}(?:\.\d+)?)\]\s+\[(\w+)\]\s+(?:\[(\w+)\]\s+)?(.*)$)");
    std::smatch m;

    if (std::regex_match(line, m, sl_re)) {
        std::string ts_str = m[1].str();
        std::tm tm{};
        std::istringstream ss(ts_str);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        entry.timestamp = std::chrono::system_clock::from_time_t(mktime(&tm));

        entry.level = parse_level(m[2].str());
        if (m[3].matched && !m[3].str().empty()) {
            entry.service = m[3].str();
        }
        entry.message = m[4].str();
    } else {
        entry.message = line;
    }

    return entry;
}

Result<std::vector<LogEntry>, std::string> LogEngine::read_straylight_logs(
    const std::string& service) const {

    std::vector<LogEntry> entries;
    std::string log_dir = "/var/log/straylight";
    if (!fs::exists(log_dir)) {
        return Result<std::vector<LogEntry>, std::string>::ok(entries);
    }

    for (const auto& entry_path : fs::directory_iterator(log_dir)) {
        if (!entry_path.is_regular_file()) continue;
        std::string fname = entry_path.path().filename().string();
        if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".log") continue;

        std::string log_service = fname.substr(0, fname.size() - 4);
        if (!service.empty() && log_service != service) continue;

        std::ifstream file(entry_path.path().string());
        if (!file.is_open()) continue;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            entries.push_back(parse_straylight_line(line, log_service));
        }
    }

    return Result<std::vector<LogEntry>, std::string>::ok(entries);
}

// ---------------------------------------------------------------------------
// query
// ---------------------------------------------------------------------------

Result<std::vector<LogEntry>, std::string> LogEngine::query(
    const std::string& service,
    const std::string& since,
    const std::string& until,
    LogEntry::Level min_level,
    int max_entries) const {

    std::vector<LogEntry> all_entries;

    // Read from journalctl
    {
        std::ostringstream args;
        if (!service.empty()) args << " -u " << service << " --identifier=" << service;
        if (!since.empty()) args << " --since='" << since << "'";
        if (!until.empty()) args << " --until='" << until << "'";
        args << " -n " << max_entries;

        auto res = read_journald(args.str());
        if (res.has_value()) {
            auto& entries = res.value();
            all_entries.insert(all_entries.end(), entries.begin(), entries.end());
        }
    }

    // Read from syslog
    {
        auto res = read_syslog(since, until);
        if (res.has_value()) {
            for (auto& e : res.value()) {
                if (!service.empty() && e.service != service) continue;
                all_entries.push_back(e);
            }
        }
    }

    // Read from straylight logs
    {
        auto res = read_straylight_logs(service);
        if (res.has_value()) {
            auto& entries = res.value();
            all_entries.insert(all_entries.end(), entries.begin(), entries.end());
        }
    }

    // Filter by level
    std::vector<LogEntry> filtered;
    for (const auto& e : all_entries) {
        if (static_cast<int>(e.level) >= static_cast<int>(min_level)) {
            filtered.push_back(e);
        }
    }

    // Sort by timestamp
    std::sort(filtered.begin(), filtered.end(),
              [](const auto& a, const auto& b) {
                  return a.timestamp < b.timestamp;
              });

    // Limit entries
    if (static_cast<int>(filtered.size()) > max_entries) {
        filtered.resize(max_entries);
    }

    return Result<std::vector<LogEntry>, std::string>::ok(filtered);
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

Result<std::vector<LogEntry>, std::string> LogEngine::search(
    const std::string& pattern,
    const std::string& service,
    const std::string& since,
    int max_entries) const {

    std::regex re;
    try {
        re = std::regex(pattern, std::regex_constants::icase);
    } catch (const std::regex_error& e) {
        return Result<std::vector<LogEntry>, std::string>::error(
            "invalid regex: " + std::string(e.what()));
    }

    // First try journalctl grep
    std::ostringstream args;
    args << " --grep='" << pattern << "'";
    if (!service.empty()) args << " -u " << service;
    if (!since.empty()) args << " --since='" << since << "'";
    args << " -n " << max_entries;

    auto jctl_res = read_journald(args.str());
    std::vector<LogEntry> results;

    if (jctl_res.has_value()) {
        results = jctl_res.value();
    }

    // Also search syslog and straylight logs
    auto all_res = query(service, since, "", LogEntry::Level::Debug, max_entries * 10);
    if (all_res.has_value()) {
        for (const auto& entry : all_res.value()) {
            if (entry.source == "journald") continue; // already searched
            if (std::regex_search(entry.message, re)) {
                results.push_back(entry);
            }
        }
    }

    // Sort and limit
    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) {
                  return a.timestamp < b.timestamp;
              });

    if (static_cast<int>(results.size()) > max_entries) {
        results.resize(max_entries);
    }

    return Result<std::vector<LogEntry>, std::string>::ok(results);
}

// ---------------------------------------------------------------------------
// follow
// ---------------------------------------------------------------------------

Result<void, std::string> LogEngine::follow(
    const std::string& service,
    std::function<bool(const LogEntry&)> callback) const {

    std::string cmd = "journalctl --output=json --no-pager -f";
    if (!service.empty()) {
        cmd += " -u " + service + " --identifier=" + service;
    }
    cmd += " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<void, std::string>::error(
            "cannot start log following: " + std::string(strerror(errno)));
    }

    std::array<char, 8192> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        std::string line = buffer.data();
        if (line.empty() || line[0] != '{') continue;

        LogEntry entry = parse_journald_json(line);
        if (!callback(entry)) break;
    }

    pclose(pipe);
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------

Result<std::vector<LogStats>, std::string> LogEngine::stats(
    const std::string& since) const {

    std::map<std::string, LogStats> stats_map;

    // Query all entries
    auto res = query("", since, "", LogEntry::Level::Debug, 100000);
    if (!res.has_value()) {
        return Result<std::vector<LogStats>, std::string>::error(res.error());
    }

    for (const auto& entry : res.value()) {
        auto& s = stats_map[entry.service];
        s.service = entry.service;
        s.total_entries++;
        s.bytes_total += entry.message.size();

        switch (entry.level) {
            case LogEntry::Level::Debug:    s.debug_count++; break;
            case LogEntry::Level::Info:     s.info_count++; break;
            case LogEntry::Level::Warning:  s.warn_count++; break;
            case LogEntry::Level::Error:    s.error_count++; break;
            case LogEntry::Level::Critical: s.critical_count++; break;
        }

        if (s.total_entries == 1) {
            s.first_entry = entry.timestamp;
            s.last_entry = entry.timestamp;
        } else {
            if (entry.timestamp < s.first_entry) s.first_entry = entry.timestamp;
            if (entry.timestamp > s.last_entry) s.last_entry = entry.timestamp;
        }
    }

    std::vector<LogStats> result;
    for (auto& [name, stat] : stats_map) {
        result.push_back(stat);
    }

    // Sort by total entries descending
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) {
                  return a.total_entries > b.total_entries;
              });

    return Result<std::vector<LogStats>, std::string>::ok(result);
}

// ---------------------------------------------------------------------------
// export
// ---------------------------------------------------------------------------

Result<std::string, std::string> LogEngine::export_logs(
    const std::string& format,
    const std::string& service,
    const std::string& since,
    const std::string& until,
    int max_entries) const {

    auto res = query(service, since, until, LogEntry::Level::Debug, max_entries);
    if (!res.has_value()) {
        return Result<std::string, std::string>::error(res.error());
    }

    const auto& entries = res.value();
    std::ostringstream out;

    auto format_ts = [](std::chrono::system_clock::time_point tp) -> std::string {
        auto tt = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
        localtime_r(&tt, &tm);
        std::ostringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        return ss.str();
    };

    if (format == "json") {
        out << "[\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            out << "  {\n"
                << "    \"timestamp\": \"" << format_ts(e.timestamp) << "\",\n"
                << "    \"service\": \"" << e.service << "\",\n"
                << "    \"level\": \"" << level_to_string(e.level) << "\",\n"
                << "    \"message\": \"";
            // Escape message for JSON
            for (char c : e.message) {
                if (c == '"') out << "\\\"";
                else if (c == '\\') out << "\\\\";
                else if (c == '\n') out << "\\n";
                else if (c == '\t') out << "\\t";
                else out << c;
            }
            out << "\",\n"
                << "    \"source\": \"" << e.source << "\",\n"
                << "    \"pid\": " << e.pid << "\n"
                << "  }";
            if (i + 1 < entries.size()) out << ",";
            out << "\n";
        }
        out << "]\n";
    } else if (format == "csv") {
        out << "timestamp,service,level,pid,source,message\n";
        for (const auto& e : entries) {
            out << format_ts(e.timestamp) << ","
                << e.service << ","
                << level_to_string(e.level) << ","
                << e.pid << ","
                << e.source << ",\"";
            // Escape CSV
            for (char c : e.message) {
                if (c == '"') out << "\"\"";
                else out << c;
            }
            out << "\"\n";
        }
    } else {
        // Plain text
        for (const auto& e : entries) {
            out << format_ts(e.timestamp) << " "
                << std::setw(5) << level_to_string(e.level) << " "
                << std::setw(20) << std::left << e.service << " "
                << e.message << "\n";
        }
    }

    return Result<std::string, std::string>::ok(out.str());
}

// ---------------------------------------------------------------------------
// Alerts
// ---------------------------------------------------------------------------

std::string LogEngine::alerts_config_path() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/log-alerts.json";
}

void LogEngine::load_alerts() {
    alerts_.clear();
    std::string path = alerts_config_path();
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto arr_start = content.find('[');
    auto arr_end = content.rfind(']');
    if (arr_start == std::string::npos || arr_end == std::string::npos) return;

    std::string arr = content.substr(arr_start + 1, arr_end - arr_start - 1);
    size_t pos = 0;

    while (true) {
        auto obj_start = arr.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < arr.size() && depth > 0) {
            if (arr[obj_end] == '{') ++depth;
            else if (arr[obj_end] == '}') --depth;
            ++obj_end;
        }

        std::string entry = arr.substr(obj_start, obj_end - obj_start);
        AlertRule rule;

        std::regex id_re(R"("id"\s*:\s*(\d+))");
        std::regex pat_re(R"("pattern"\s*:\s*"([^"]*)")");
        std::regex svc_re(R"("service"\s*:\s*"([^"]*)")");
        std::regex act_re(R"("action"\s*:\s*"([^"]*)")");
        std::regex en_re(R"("enabled"\s*:\s*(true|false))");
        std::regex cnt_re(R"("trigger_count"\s*:\s*(\d+))");

        std::smatch m;
        if (std::regex_search(entry, m, id_re)) rule.id = std::stoul(m[1].str());
        if (std::regex_search(entry, m, pat_re)) rule.pattern = m[1].str();
        if (std::regex_search(entry, m, svc_re)) rule.service = m[1].str();
        if (std::regex_search(entry, m, act_re)) rule.action = m[1].str();
        if (std::regex_search(entry, m, en_re)) rule.enabled = (m[1].str() == "true");
        if (std::regex_search(entry, m, cnt_re)) rule.trigger_count = std::stoul(m[1].str());

        if (rule.id >= next_alert_id_) next_alert_id_ = rule.id + 1;
        alerts_.push_back(rule);
        pos = obj_end;
    }
}

void LogEngine::save_alerts() const {
    std::string path = alerts_config_path();
    fs::create_directories(fs::path(path).parent_path());

    std::ofstream out(path);
    if (!out.is_open()) return;

    out << "{ \"alerts\": [\n";
    for (size_t i = 0; i < alerts_.size(); ++i) {
        const auto& r = alerts_[i];
        out << "  {\n"
            << "    \"id\": " << r.id << ",\n"
            << "    \"pattern\": \"" << r.pattern << "\",\n"
            << "    \"service\": \"" << r.service << "\",\n"
            << "    \"action\": \"" << r.action << "\",\n"
            << "    \"enabled\": " << (r.enabled ? "true" : "false") << ",\n"
            << "    \"trigger_count\": " << r.trigger_count << "\n"
            << "  }";
        if (i + 1 < alerts_.size()) out << ",";
        out << "\n";
    }
    out << "] }\n";
}

Result<void, std::string> LogEngine::add_alert(const AlertRule& rule) {
    // Validate regex
    try {
        std::regex test(rule.pattern);
    } catch (const std::regex_error& e) {
        return Result<void, std::string>::error(
            "invalid regex pattern: " + std::string(e.what()));
    }

    AlertRule new_rule = rule;
    new_rule.id = next_alert_id_++;
    alerts_.push_back(new_rule);
    save_alerts();
    return Result<void, std::string>::ok();
}

Result<void, std::string> LogEngine::remove_alert(uint32_t rule_id) {
    auto it = std::find_if(alerts_.begin(), alerts_.end(),
                           [rule_id](const AlertRule& r) { return r.id == rule_id; });
    if (it == alerts_.end()) {
        return Result<void, std::string>::error(
            "alert rule not found: " + std::to_string(rule_id));
    }
    alerts_.erase(it);
    save_alerts();
    return Result<void, std::string>::ok();
}

std::vector<AlertRule> LogEngine::list_alerts() const {
    return alerts_;
}

void LogEngine::check_alerts(const LogEntry& entry) {
    for (auto& rule : alerts_) {
        if (!rule.enabled) continue;
        if (!rule.service.empty() && rule.service != entry.service) continue;
        if (static_cast<int>(entry.level) < static_cast<int>(rule.min_level)) continue;

        try {
            std::regex re(rule.pattern, std::regex_constants::icase);
            if (std::regex_search(entry.message, re)) {
                rule.trigger_count++;
                fire_alert(rule, entry);
            }
        } catch (...) {}
    }
}

void LogEngine::fire_alert(const AlertRule& rule, const LogEntry& entry) const {
    if (rule.action == "notify") {
        // Send desktop notification
        std::string cmd = "notify-send 'StrayLight Log Alert' '"
                          + entry.service + ": " + entry.message + "' 2>/dev/null";
        std::system(cmd.c_str());
    } else if (rule.action.rfind("exec:", 0) == 0) {
        std::string cmd = rule.action.substr(5);
        // Replace placeholders
        std::string full_cmd = cmd;
        auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = str.find(from, pos)) != std::string::npos) {
                str.replace(pos, from.length(), to);
                pos += to.length();
            }
        };
        replace_all(full_cmd, "{service}", entry.service);
        replace_all(full_cmd, "{message}", entry.message);
        replace_all(full_cmd, "{level}", level_to_string(entry.level));
        std::system((full_cmd + " 2>/dev/null &").c_str());
    } else if (rule.action.rfind("log:", 0) == 0) {
        std::string log_path = rule.action.substr(4);
        std::ofstream log(log_path, std::ios::app);
        if (log.is_open()) {
            auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
            std::tm tm{};
            localtime_r(&tt, &tm);
            log << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " "
                << level_to_string(entry.level) << " "
                << entry.service << ": " << entry.message << "\n";
        }
    }
}

} // namespace straylight
