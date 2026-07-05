// tools/migrate/main.cpp
// straylight-migrate — System migration tool for StrayLight OS.
#include "migrator.h"
#include "diff_engine.h"

#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <string>

using straylight::DiffEngine;
using straylight::Migrator;

static void print_usage() {
    std::cerr
        << "straylight-migrate — System migration tool\n\n"
        << "Usage:\n"
        << "  straylight-migrate export <archive.tar.zst>    Export system config\n"
        << "  straylight-migrate import <archive.tar.zst>    Import system config\n"
        << "  straylight-migrate sync <user@host>            Live sync to remote\n"
        << "  straylight-migrate diff <user@host>            Show differences\n"
        << "  straylight-migrate manifest                    Print local manifest\n"
        << "\nOptions:\n"
        << "  --include-ssh     Include SSH keys in export\n"
        << "  --dry-run         Show what would be imported without applying\n"
        << "\nExamples:\n"
        << "  straylight-migrate export backup.tar.zst\n"
        << "  straylight-migrate import backup.tar.zst --dry-run\n"
        << "  straylight-migrate diff root@newbox\n"
        << "  straylight-migrate sync root@newbox\n";
}

static void progress_callback(const std::string& phase, int percent) {
    std::cerr << "\r[" << percent << "%] " << phase;
    if (percent >= 100) std::cerr << "\n";
    else std::cerr << "...";
    std::cerr.flush();
}

static std::pair<std::string, std::string> parse_user_host(const std::string& spec) {
    auto at = spec.find('@');
    if (at != std::string::npos) {
        return {spec.substr(0, at), spec.substr(at + 1)};
    }
    return {"root", spec};
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    Migrator migrator;
    migrator.set_progress(progress_callback);

    if (cmd == "export") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-migrate export <archive.tar.zst>\n";
            return 1;
        }
        std::string output = argv[2];
        bool include_ssh = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--include-ssh") include_ssh = true;
        }

        auto r = migrator.export_archive(output, include_ssh);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error() << "\n";
            return 1;
        }

        auto size = r.value();
        if (size >= 1024 * 1024) {
            std::cout << "Exported " << (size / (1024 * 1024)) << " MB to " << output << "\n";
        } else if (size >= 1024) {
            std::cout << "Exported " << (size / 1024) << " KB to " << output << "\n";
        } else {
            std::cout << "Exported " << size << " bytes to " << output << "\n";
        }

    } else if (cmd == "import") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-migrate import <archive.tar.zst>\n";
            return 1;
        }
        std::string archive = argv[2];
        bool dry_run = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--dry-run") dry_run = true;
        }

        auto r = migrator.import_archive(archive, dry_run);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error() << "\n";
            return 1;
        }

        if (dry_run) {
            std::cout << "Would import " << r.value() << " files (dry run)\n";
        } else {
            std::cout << "Imported " << r.value() << " files\n";
        }

    } else if (cmd == "sync") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-migrate sync <user@host>\n";
            return 1;
        }
        auto [user, host] = parse_user_host(argv[2]);

        auto r = migrator.sync_to_remote(host, user);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error() << "\n";
            return 1;
        }
        std::cout << "Sync complete to " << user << "@" << host << "\n";

    } else if (cmd == "diff") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-migrate diff <user@host>\n";
            return 1;
        }
        auto [user, host] = parse_user_host(argv[2]);

        auto r = migrator.diff_remote(host, user);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error() << "\n";
            return 1;
        }
        std::cout << DiffEngine::format_diff(r.value());

    } else if (cmd == "manifest") {
        DiffEngine engine;
        auto r = engine.build_local_manifest();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error() << "\n";
            return 1;
        }
        std::cout << DiffEngine::manifest_to_json(r.value()) << "\n";

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
