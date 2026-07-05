// tools/timeline/main.cpp
// CLI front-end for straylight-timeline -- activity & event timeline.

#include "collectors.h"
#include "event_store.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-timeline -- activity & event timeline\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-timeline today                        What happened today\n"
        << "  straylight-timeline search <pattern> [--from F] [--to T]\n"
        << "  straylight-timeline apps                         App usage timeline\n"
        << "  straylight-timeline files [--modified] [--accessed]\n"
        << "  straylight-timeline logins                       Login/logout history\n"
        << "  straylight-timeline packages                     Package history\n"
        << "  straylight-timeline commands                     Command history\n"
        << "  straylight-timeline services                     Service events\n"
        << "  straylight-timeline git                          Git commit history\n"
        << "  straylight-timeline collect                      Run all collectors\n"
        << "  straylight-timeline export <file.json>           Export to JSON\n"
        << "  straylight-timeline purge <days>                 Remove old events\n"
        << "  straylight-timeline stats                        Show database stats\n";
}

static std::string format_time(std::chrono::system_clock::time_point tp) {
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     tp.time_since_epoch())
                     .count();
    if (epoch <= 0) return "(unknown)";

    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string db_path() {
    const char* home = std::getenv("HOME");
    std::string h = home ? home : "/root";
    return h + "/.config/straylight/timeline.db";
}

static void print_events(const std::vector<straylight::TimelineEvent>& events) {
    if (events.empty()) {
        std::cout << "No events found.\n";
        return;
    }

    std::cout << std::left
              << std::setw(20) << "TIMESTAMP"
              << std::setw(10) << "CATEGORY"
              << std::setw(12) << "ACTION"
              << "SUBJECT\n";
    std::cout << std::string(80, '-') << "\n";

    for (const auto& e : events) {
        std::cout << std::left
                  << std::setw(20) << format_time(e.timestamp)
                  << std::setw(10) << e.category
                  << std::setw(12) << e.action
                  << e.subject << "\n";
    }
    std::cout << "\n" << events.size() << " event(s)\n";
}

static void print_and_store(straylight::EventStore& store,
                             std::vector<straylight::TimelineEvent>& events) {
    if (!events.empty()) {
        auto res = store.insert_batch(events);
        if (res.has_value()) {
            // Stored successfully.
        }
    }
    print_events(events);
}

static std::chrono::system_clock::time_point parse_date(const std::string& s) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (!ss.fail()) {
        return std::chrono::system_clock::from_time_t(mktime(&tm));
    }
    // Try epoch.
    long long epoch = std::atoll(s.c_str());
    if (epoch > 0) {
        return std::chrono::system_clock::from_time_t(static_cast<time_t>(epoch));
    }
    return {};
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

    straylight::EventStore store;
    auto open_res = store.open(db_path());
    if (!open_res.has_value()) {
        std::cerr << "Error: cannot open database: " << open_res.error() << "\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // today
    // -----------------------------------------------------------------------
    if (command == "today") {
        // First collect fresh data.
        auto events = straylight::Collectors::collect_all();
        store.insert_batch(events);

        // Then query today's events from the database.
        auto res = store.today();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        print_events(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // search
    // -----------------------------------------------------------------------
    if (command == "search") {
        if (argc < 3) {
            std::cerr << "Error: 'search' requires <pattern>\n";
            return 1;
        }
        std::string pattern = argv[2];
        std::chrono::system_clock::time_point from{}, to{};

        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
                from = parse_date(argv[++i]);
            } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
                to = parse_date(argv[++i]);
            }
        }

        auto res = store.search(pattern, from, to);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        print_events(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // apps
    // -----------------------------------------------------------------------
    if (command == "apps") {
        auto events = straylight::Collectors::collect_apps();
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // files
    // -----------------------------------------------------------------------
    if (command == "files") {
        bool modified = false, accessed = false;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--modified") == 0) modified = true;
            if (std::strcmp(argv[i], "--accessed") == 0) accessed = true;
        }
        // Default to modified if neither specified.
        if (!modified && !accessed) modified = true;

        auto events = straylight::Collectors::collect_files(modified, accessed);
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // logins
    // -----------------------------------------------------------------------
    if (command == "logins") {
        auto events = straylight::Collectors::collect_logins();
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // packages
    // -----------------------------------------------------------------------
    if (command == "packages") {
        auto events = straylight::Collectors::collect_packages();
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // commands
    // -----------------------------------------------------------------------
    if (command == "commands") {
        auto events = straylight::Collectors::collect_commands();
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // services
    // -----------------------------------------------------------------------
    if (command == "services") {
        auto events = straylight::Collectors::collect_services();
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // git
    // -----------------------------------------------------------------------
    if (command == "git") {
        auto events = straylight::Collectors::collect_git();
        print_and_store(store, events);
        return 0;
    }

    // -----------------------------------------------------------------------
    // collect
    // -----------------------------------------------------------------------
    if (command == "collect") {
        std::cout << "Running all collectors...\n";
        auto events = straylight::Collectors::collect_all();
        auto res = store.insert_batch(events);
        if (res.has_value()) {
            std::cout << "Collected " << res.value() << " event(s).\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Total events in database: " << store.count() << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // export
    // -----------------------------------------------------------------------
    if (command == "export") {
        if (argc < 3) {
            std::cerr << "Error: 'export' requires <file.json>\n";
            return 1;
        }
        std::string file = argv[2];
        auto res = store.export_json();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::ofstream out(file);
        if (!out.is_open()) {
            std::cerr << "Error: cannot write to " << file << "\n";
            return 1;
        }
        out << res.value();
        out.close();
        std::cout << "Exported to " << file << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // purge
    // -----------------------------------------------------------------------
    if (command == "purge") {
        if (argc < 3) {
            std::cerr << "Error: 'purge' requires <days>\n";
            return 1;
        }
        int days = std::atoi(argv[2]);
        if (days <= 0) {
            std::cerr << "Error: days must be positive\n";
            return 1;
        }
        auto res = store.purge(days);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Purged " << res.value() << " event(s) older than "
                  << days << " day(s).\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // stats
    // -----------------------------------------------------------------------
    if (command == "stats") {
        std::cout << "Database: " << db_path() << "\n"
                  << "Total events: " << store.count() << "\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
