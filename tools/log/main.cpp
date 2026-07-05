// tools/log/main.cpp
// CLI front-end for straylight-log — unified log viewer and search.

#include "log_engine.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-log — unified log viewer\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-log view [--service=X] [--since=TIME] [--until=TIME] [--level=warn]\n"
        << "                      [--max=N]                     View logs with filters\n"
        << "  straylight-log follow [--service=X]               Follow logs in real-time\n"
        << "  straylight-log search <pattern> [--service=X] [--since=TIME] [--max=N]\n"
        << "                                                    Search with regex\n"
        << "  straylight-log stats [--since=TIME]               Show log statistics\n"
        << "  straylight-log export <json|csv|text> [--service=X] [--since=TIME]\n"
        << "                                                    Export logs\n"
        << "  straylight-log alert add <pattern> <action>       Add alert rule\n"
        << "  straylight-log alert list                         List alert rules\n"
        << "  straylight-log alert remove <id>                  Remove alert rule\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static std::string format_time(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static const char* level_color(straylight::LogEntry::Level level) {
    switch (level) {
        case straylight::LogEntry::Level::Debug:    return "\033[90m";   // gray
        case straylight::LogEntry::Level::Info:     return "\033[0m";    // default
        case straylight::LogEntry::Level::Warning:  return "\033[33m";   // yellow
        case straylight::LogEntry::Level::Error:    return "\033[31m";   // red
        case straylight::LogEntry::Level::Critical: return "\033[1;31m"; // bold red
    }
    return "\033[0m";
}

static const char* level_name(straylight::LogEntry::Level level) {
    switch (level) {
        case straylight::LogEntry::Level::Debug:    return "DEBUG";
        case straylight::LogEntry::Level::Info:     return "INFO ";
        case straylight::LogEntry::Level::Warning:  return "WARN ";
        case straylight::LogEntry::Level::Error:    return "ERROR";
        case straylight::LogEntry::Level::Critical: return "CRIT ";
    }
    return "INFO ";
}

static straylight::LogEntry::Level parse_level_arg(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "debug") return straylight::LogEntry::Level::Debug;
    if (lower == "info") return straylight::LogEntry::Level::Info;
    if (lower == "warn" || lower == "warning") return straylight::LogEntry::Level::Warning;
    if (lower == "error" || lower == "err") return straylight::LogEntry::Level::Error;
    if (lower == "critical" || lower == "crit") return straylight::LogEntry::Level::Critical;
    return straylight::LogEntry::Level::Debug;
}

static bool is_tty() {
    return isatty(fileno(stdout));
}

static std::string human_size(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 3) { val /= 1024.0; ++idx; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
    return buf;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::LogEngine engine;
    bool use_color = is_tty();

    // -----------------------------------------------------------------------
    // view
    // -----------------------------------------------------------------------
    if (command == "view") {
        std::string service = get_arg(argc, argv, "--service=");
        std::string since = get_arg(argc, argv, "--since=");
        std::string until = get_arg(argc, argv, "--until=");
        std::string level_str = get_arg(argc, argv, "--level=");
        std::string max_str = get_arg(argc, argv, "--max=");

        auto min_level = level_str.empty() ? straylight::LogEntry::Level::Debug
                                            : parse_level_arg(level_str);
        int max_entries = max_str.empty() ? 1000 : std::atoi(max_str.c_str());

        auto res = engine.query(service, since, until, min_level, max_entries);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        for (const auto& entry : res.value()) {
            if (use_color) std::cout << level_color(entry.level);
            std::cout << format_time(entry.timestamp) << " "
                      << level_name(entry.level) << " "
                      << std::setw(20) << std::left << entry.service << " "
                      << entry.message;
            if (use_color) std::cout << "\033[0m";
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // follow
    // -----------------------------------------------------------------------
    if (command == "follow") {
        std::string service = get_arg(argc, argv, "--service=");

        std::cout << "Following logs" << (service.empty() ? "" : " for " + service)
                  << " (Ctrl+C to stop)...\n\n";

        auto res = engine.follow(service, [&](const straylight::LogEntry& entry) -> bool {
            if (use_color) std::cout << level_color(entry.level);
            std::cout << format_time(entry.timestamp) << " "
                      << level_name(entry.level) << " "
                      << std::setw(20) << std::left << entry.service << " "
                      << entry.message;
            if (use_color) std::cout << "\033[0m";
            std::cout << "\n" << std::flush;

            // Check alerts
            engine.check_alerts(entry);
            return true;
        });

        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // search <pattern>
    // -----------------------------------------------------------------------
    if (command == "search") {
        if (argc < 3) {
            std::cerr << "Error: 'search' requires a pattern\n";
            return 1;
        }
        std::string pattern = argv[2];
        std::string service = get_arg(argc, argv, "--service=", 3);
        std::string since = get_arg(argc, argv, "--since=", 3);
        std::string max_str = get_arg(argc, argv, "--max=", 3);
        int max_entries = max_str.empty() ? 500 : std::atoi(max_str.c_str());

        auto res = engine.search(pattern, service, since, max_entries);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& entries = res.value();
        if (entries.empty()) {
            std::cout << "No matching log entries found.\n";
            return 0;
        }

        std::cout << entries.size() << " match(es) found:\n\n";

        // Highlight matches
        std::regex highlight_re(pattern, std::regex_constants::icase);
        for (const auto& entry : entries) {
            if (use_color) std::cout << level_color(entry.level);
            std::cout << format_time(entry.timestamp) << " "
                      << level_name(entry.level) << " "
                      << std::setw(20) << std::left << entry.service << " ";

            if (use_color) {
                // Highlight matching parts
                std::string highlighted = std::regex_replace(
                    entry.message, highlight_re, "\033[1;33m$&" +
                    std::string(level_color(entry.level)));
                std::cout << highlighted;
            } else {
                std::cout << entry.message;
            }

            if (use_color) std::cout << "\033[0m";
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // stats
    // -----------------------------------------------------------------------
    if (command == "stats") {
        std::string since = get_arg(argc, argv, "--since=");

        auto res = engine.stats(since);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& stats = res.value();
        if (stats.empty()) {
            std::cout << "No log entries found.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(24) << "SERVICE"
                  << std::setw(10) << "TOTAL"
                  << std::setw(8) << "DEBUG"
                  << std::setw(8) << "INFO"
                  << std::setw(8) << "WARN"
                  << std::setw(8) << "ERROR"
                  << std::setw(8) << "CRIT"
                  << "SIZE\n";
        std::cout << std::string(82, '-') << "\n";

        uint64_t grand_total = 0;
        for (const auto& s : stats) {
            std::cout << std::left
                      << std::setw(24) << s.service
                      << std::setw(10) << s.total_entries
                      << std::setw(8) << s.debug_count
                      << std::setw(8) << s.info_count
                      << std::setw(8) << s.warn_count;

            if (use_color && s.error_count > 0) std::cout << "\033[31m";
            std::cout << std::setw(8) << s.error_count;
            if (use_color) std::cout << "\033[0m";

            if (use_color && s.critical_count > 0) std::cout << "\033[1;31m";
            std::cout << std::setw(8) << s.critical_count;
            if (use_color) std::cout << "\033[0m";

            std::cout << human_size(s.bytes_total) << "\n";
            grand_total += s.total_entries;
        }

        std::cout << std::string(82, '-') << "\n"
                  << "Total: " << grand_total << " entries across "
                  << stats.size() << " service(s)\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // export <format>
    // -----------------------------------------------------------------------
    if (command == "export") {
        if (argc < 3) {
            std::cerr << "Error: 'export' requires a format (json/csv/text)\n";
            return 1;
        }
        std::string format = argv[2];
        std::string service = get_arg(argc, argv, "--service=", 3);
        std::string since = get_arg(argc, argv, "--since=", 3);
        std::string until = get_arg(argc, argv, "--until=", 3);
        std::string output_file = get_arg(argc, argv, "--output=", 3);

        auto res = engine.export_logs(format, service, since, until);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        if (!output_file.empty()) {
            std::ofstream out(output_file);
            if (!out.is_open()) {
                std::cerr << "Error: cannot write to " << output_file << "\n";
                return 1;
            }
            out << res.value();
            std::cout << "Exported to " << output_file << "\n";
        } else {
            std::cout << res.value();
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // alert <add|list|remove>
    // -----------------------------------------------------------------------
    if (command == "alert") {
        if (argc < 3) {
            std::cerr << "Error: 'alert' requires a subcommand\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "add") {
            if (argc < 5) {
                std::cerr << "Error: 'alert add' requires <pattern> <action>\n"
                          << "  Actions: notify, exec:<command>, log:<file>\n";
                return 1;
            }
            straylight::AlertRule rule;
            rule.pattern = argv[3];
            rule.action = argv[4];
            rule.service = get_arg(argc, argv, "--service=", 5);

            auto res = engine.add_alert(rule);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Alert rule added for pattern '" << rule.pattern << "'\n";
            return 0;
        }

        if (sub == "list") {
            auto alerts = engine.list_alerts();
            if (alerts.empty()) {
                std::cout << "No alert rules configured.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(6) << "ID"
                      << std::setw(30) << "PATTERN"
                      << std::setw(15) << "SERVICE"
                      << std::setw(20) << "ACTION"
                      << std::setw(8) << "FIRED"
                      << "ENABLED\n";
            std::cout << std::string(79, '-') << "\n";
            for (const auto& r : alerts) {
                std::cout << std::left
                          << std::setw(6) << r.id
                          << std::setw(30) << r.pattern
                          << std::setw(15) << (r.service.empty() ? "*" : r.service)
                          << std::setw(20) << r.action
                          << std::setw(8) << r.trigger_count
                          << (r.enabled ? "yes" : "no") << "\n";
            }
            return 0;
        }

        if (sub == "remove") {
            if (argc < 4) {
                std::cerr << "Error: 'alert remove' requires a rule ID\n";
                return 1;
            }
            auto res = engine.remove_alert(std::stoul(argv[3]));
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Alert rule " << argv[3] << " removed.\n";
            return 0;
        }

        std::cerr << "Error: unknown alert subcommand '" << sub << "'\n";
        return 1;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
