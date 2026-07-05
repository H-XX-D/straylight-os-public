// tools/update/main.cpp
// CLI front-end for straylight-update — system update management.

#include "update_manager.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-update — system update manager\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-update check                            Check for available updates\n"
        << "  straylight-update upgrade [--auto-snapshot] [--security-only] [--dry-run]\n"
        << "                                                     Perform system upgrade\n"
        << "  straylight-update rollback                         Rollback last update\n"
        << "  straylight-update history                          Show update history\n"
        << "  straylight-update schedule <cron-expr>             Schedule automatic updates\n"
        << "  straylight-update unschedule                       Remove scheduled updates\n"
        << "  straylight-update hold <package>                   Prevent package from upgrading\n"
        << "  straylight-update unhold <package>                 Allow package to upgrade\n"
        << "  straylight-update holds                            List held packages\n"
        << "  straylight-update changelog <package>              Show package changelog\n"
        << "  straylight-update clean                            Clean package cache\n";
}

static bool has_flag(int argc, char* argv[], const std::string& flag) {
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::string format_time(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
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

    straylight::UpdateManager mgr;

    // -----------------------------------------------------------------------
    // check
    // -----------------------------------------------------------------------
    if (command == "check") {
        std::cout << "Checking for updates...\n";
        auto res = mgr.check();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& updates = res.value();
        if (updates.empty()) {
            std::cout << "System is up to date.\n";
            return 0;
        }

        int security_count = 0;
        uint64_t total_size = 0;
        for (const auto& u : updates) {
            if (u.is_security) ++security_count;
            total_size += u.download_size;
        }

        std::cout << updates.size() << " update(s) available";
        if (security_count > 0) {
            std::cout << " (" << security_count << " security)";
        }
        std::cout << "\n\n";

        std::cout << std::left
                  << std::setw(30) << "PACKAGE"
                  << std::setw(18) << "CURRENT"
                  << std::setw(18) << "NEW"
                  << "SECTION\n";
        std::cout << std::string(74, '-') << "\n";

        for (const auto& u : updates) {
            std::cout << std::left
                      << std::setw(30) << u.name
                      << std::setw(18) << u.current_version
                      << std::setw(18) << u.new_version
                      << u.section;
            if (u.is_security) std::cout << " [SECURITY]";
            std::cout << "\n";
        }

        if (total_size > 0) {
            std::cout << "\nTotal download size: " << human_size(total_size) << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // upgrade
    // -----------------------------------------------------------------------
    if (command == "upgrade") {
        bool auto_snapshot = has_flag(argc, argv, "--auto-snapshot");
        bool security_only = has_flag(argc, argv, "--security-only");
        bool dry_run = has_flag(argc, argv, "--dry-run");

        if (dry_run) {
            std::cout << "Performing dry-run upgrade...\n";
        } else {
            std::cout << "Starting system upgrade...\n";
            if (auto_snapshot) {
                std::cout << "Creating pre-update snapshot...\n";
            }
        }

        auto res = mgr.upgrade(auto_snapshot, security_only, dry_run);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& record = res.value();
        if (record.packages_upgraded.empty()) {
            std::cout << "System is already up to date.\n";
            return 0;
        }

        std::cout << "\nUpgrade " << (dry_run ? "would apply" : "complete") << ":\n";
        std::cout << "  " << record.packages_upgraded.size() << " package(s) upgraded\n";
        if (!record.packages_installed.empty()) {
            std::cout << "  " << record.packages_installed.size()
                      << " new package(s) installed\n";
        }
        if (!record.snapshot_name.empty()) {
            std::cout << "  Snapshot: " << record.snapshot_name << "\n";
        }
        std::cout << "  ID: " << record.id << "\n";

        return 0;
    }

    // -----------------------------------------------------------------------
    // rollback
    // -----------------------------------------------------------------------
    if (command == "rollback") {
        std::cout << "Rolling back to pre-update state...\n";
        auto res = mgr.rollback();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Rollback complete. A reboot may be required.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // history
    // -----------------------------------------------------------------------
    if (command == "history") {
        auto res = mgr.history();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& records = res.value();
        if (records.empty()) {
            std::cout << "No update history.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(22) << "ID"
                  << std::setw(22) << "DATE"
                  << std::setw(10) << "STATUS"
                  << "PACKAGES\n";
        std::cout << std::string(62, '-') << "\n";

        for (const auto& r : records) {
            std::cout << std::left
                      << std::setw(22) << r.id
                      << std::setw(22) << format_time(r.timestamp)
                      << std::setw(10) << (r.success ? "OK" : "FAILED")
                      << r.packages_upgraded.size() << " upgraded";
            if (!r.packages_installed.empty()) {
                std::cout << ", " << r.packages_installed.size() << " new";
            }
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // schedule / unschedule
    // -----------------------------------------------------------------------
    if (command == "schedule") {
        if (argc < 3) {
            // Show current schedule
            auto res = mgr.get_schedule();
            if (!res.has_value()) {
                std::cout << "No scheduled updates.\n";
                return 0;
            }
            std::cout << "Scheduled updates: " << res.value() << "\n";
            return 0;
        }
        auto res = mgr.schedule(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Automatic updates scheduled: " << argv[2] << "\n";
        return 0;
    }

    if (command == "unschedule") {
        auto res = mgr.unschedule();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Scheduled updates removed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // hold / unhold
    // -----------------------------------------------------------------------
    if (command == "hold") {
        if (argc < 3) {
            std::cerr << "Error: 'hold' requires a package name\n";
            return 1;
        }
        auto res = mgr.hold(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Package '" << argv[2] << "' held.\n";
        return 0;
    }

    if (command == "unhold") {
        if (argc < 3) {
            std::cerr << "Error: 'unhold' requires a package name\n";
            return 1;
        }
        auto res = mgr.unhold(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Package '" << argv[2] << "' unheld.\n";
        return 0;
    }

    if (command == "holds") {
        auto res = mgr.list_holds();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& holds = res.value();
        if (holds.empty()) {
            std::cout << "No held packages.\n";
            return 0;
        }
        std::cout << std::left
                  << std::setw(30) << "PACKAGE"
                  << "VERSION\n";
        std::cout << std::string(50, '-') << "\n";
        for (const auto& h : holds) {
            std::cout << std::left
                      << std::setw(30) << h.name
                      << h.version << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // changelog
    // -----------------------------------------------------------------------
    if (command == "changelog") {
        if (argc < 3) {
            std::cerr << "Error: 'changelog' requires a package name\n";
            return 1;
        }
        auto res = mgr.changelog(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // clean
    // -----------------------------------------------------------------------
    if (command == "clean") {
        auto est = mgr.clean_estimate();
        if (est.has_value() && est.value() > 0) {
            std::cout << "Freeing " << human_size(est.value()) << " of cached packages...\n";
        }
        auto res = mgr.clean();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Package cache cleaned.\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
