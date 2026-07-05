// tools/snapshot/main.cpp
// CLI front-end for straylight-snapshot — instant system state capture & restore.

#include "snapshot_manager.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-snapshot — instant system state capture & restore\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-snapshot save <name> [--description \"...\"]  Create named snapshot\n"
        << "  straylight-snapshot restore <name>                      Restore to snapshot\n"
        << "  straylight-snapshot list                                List all snapshots\n"
        << "  straylight-snapshot diff <name>                         Show changes since snapshot\n"
        << "  straylight-snapshot delete <name>                       Delete a snapshot\n"
        << "  straylight-snapshot auto [--interval 1h] [--keep 24]   Enable auto-snapshots\n"
        << "  straylight-snapshot rollback                            Undo last restore\n";
}

static std::string format_time(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string human_size(size_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 4) {
        val /= 1024.0;
        ++idx;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << val << " " << units[idx];
    return ss.str();
}

/// Parse an interval string like "1h", "30m", "2h30m", "3600" into seconds.
static int parse_interval(const std::string& s) {
    if (s.empty()) return 3600;
    // Pure numeric -> treat as seconds.
    bool all_digits = true;
    for (char c : s) {
        if (!isdigit(c)) { all_digits = false; break; }
    }
    if (all_digits) return std::atoi(s.c_str());

    int total = 0;
    int accum = 0;
    for (char c : s) {
        if (isdigit(c)) {
            accum = accum * 10 + (c - '0');
        } else if (c == 'h' || c == 'H') {
            total += accum * 3600;
            accum = 0;
        } else if (c == 'm' || c == 'M') {
            total += accum * 60;
            accum = 0;
        } else if (c == 's' || c == 'S') {
            total += accum;
            accum = 0;
        }
    }
    total += accum; // trailing number with no unit treated as seconds
    return total > 0 ? total : 3600;
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

    straylight::SnapshotManager mgr;

    // -----------------------------------------------------------------------
    // save
    // -----------------------------------------------------------------------
    if (command == "save") {
        if (argc < 3) {
            std::cerr << "Error: 'save' requires a snapshot name\n";
            return 1;
        }
        std::string name = argv[2];
        std::string description;
        for (int i = 3; i < argc; ++i) {
            if ((std::strcmp(argv[i], "--description") == 0 ||
                 std::strcmp(argv[i], "-d") == 0) &&
                i + 1 < argc) {
                description = argv[++i];
            }
        }
        auto res = mgr.save(name, description);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& snap = res.value();
        std::cout << "Snapshot created:\n"
                  << "  Name:    " << snap.name << "\n"
                  << "  Path:    " << snap.btrfs_path << "\n"
                  << "  Size:    " << human_size(snap.size_bytes) << "\n"
                  << "  Created: " << format_time(snap.created) << "\n";
        if (!snap.description.empty()) {
            std::cout << "  Desc:    " << snap.description << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // restore
    // -----------------------------------------------------------------------
    if (command == "restore") {
        if (argc < 3) {
            std::cerr << "Error: 'restore' requires a snapshot name\n";
            return 1;
        }
        std::string name = argv[2];
        auto res = mgr.restore(name);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "System restored to snapshot '" << name << "'.\n"
                  << "A reboot may be required to complete the restore.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // rollback
    // -----------------------------------------------------------------------
    if (command == "rollback") {
        auto res = mgr.rollback();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Rollback complete — system restored to pre-restore state.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto snaps = mgr.list();
        if (snaps.empty()) {
            std::cout << "No snapshots found.\n";
            return 0;
        }
        // Header.
        std::cout << std::left
                  << std::setw(30) << "NAME"
                  << std::setw(22) << "CREATED"
                  << std::setw(12) << "SIZE"
                  << "DESCRIPTION" << "\n";
        std::cout << std::string(80, '-') << "\n";
        for (const auto& snap : snaps) {
            std::string flags;
            if (snap.is_auto) flags += "[auto] ";
            std::cout << std::left
                      << std::setw(30) << snap.name
                      << std::setw(22) << format_time(snap.created)
                      << std::setw(12) << human_size(snap.size_bytes)
                      << flags << snap.description << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // diff
    // -----------------------------------------------------------------------
    if (command == "diff") {
        if (argc < 3) {
            std::cerr << "Error: 'diff' requires a snapshot name\n";
            return 1;
        }
        std::string name = argv[2];
        auto res = mgr.diff(name);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // delete
    // -----------------------------------------------------------------------
    if (command == "delete") {
        if (argc < 3) {
            std::cerr << "Error: 'delete' requires a snapshot name\n";
            return 1;
        }
        std::string name = argv[2];
        auto res = mgr.remove(name);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Snapshot '" << name << "' deleted.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // auto
    // -----------------------------------------------------------------------
    if (command == "auto") {
        int interval_secs = 3600;  // default 1 hour
        int keep_count = 24;       // default keep 24

        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
                interval_secs = parse_interval(argv[++i]);
            } else if (std::strcmp(argv[i], "--keep") == 0 && i + 1 < argc) {
                keep_count = std::atoi(argv[++i]);
            }
        }

        auto res = mgr.auto_enable(interval_secs, keep_count);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Auto-snapshots enabled: every " << interval_secs
                  << "s, keeping " << keep_count << " most recent.\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
