// tools/link/main.cpp
// CLI front-end for straylight-link -- intelligent symlink and resource management.

#include "link_manager.h"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-link -- intelligent symlink and resource management\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-link create <target> <link>          Create managed symlink\n"
        << "  straylight-link dotfiles <repo-dir>             Auto-symlink dotfiles\n"
        << "  straylight-link audit [<root>]                  Find broken symlinks\n"
        << "  straylight-link graph                           Show link dependency graph\n"
        << "  straylight-link watch <dir> [--pattern P]       Watch and auto-link\n"
        << "  straylight-link restore                         Restore all managed links\n"
        << "  straylight-link list                            List managed links\n"
        << "  straylight-link remove <link-path>              Untrack and remove link\n";
}

static std::string format_time(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
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

    straylight::LinkManager mgr;

    // -----------------------------------------------------------------------
    // create
    // -----------------------------------------------------------------------
    if (command == "create") {
        if (argc < 4) {
            std::cerr << "Error: 'create' requires <target> and <link>\n";
            return 1;
        }
        std::string target = argv[2];
        std::string link = argv[3];
        std::string tag;
        for (int i = 4; i < argc; ++i) {
            if ((std::strcmp(argv[i], "--tag") == 0 ||
                 std::strcmp(argv[i], "-t") == 0) &&
                i + 1 < argc) {
                tag = argv[++i];
            }
        }

        auto res = mgr.create(target, link, tag);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& ml = res.value();
        std::cout << "Created managed symlink:\n"
                  << "  Link:    " << ml.link_path << "\n"
                  << "  Target:  " << ml.target_path << "\n"
                  << "  Created: " << format_time(ml.created) << "\n";
        if (!ml.tag.empty()) {
            std::cout << "  Tag:     " << ml.tag << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // dotfiles
    // -----------------------------------------------------------------------
    if (command == "dotfiles") {
        if (argc < 3) {
            std::cerr << "Error: 'dotfiles' requires <repo-dir>\n";
            return 1;
        }
        std::string repo_dir = argv[2];
        auto res = mgr.dotfiles(repo_dir);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& created = res.value();
        std::cout << "Linked " << created.size() << " dotfiles:\n";
        for (const auto& ml : created) {
            std::cout << "  " << ml.link_path << " -> " << ml.target_path << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // audit
    // -----------------------------------------------------------------------
    if (command == "audit") {
        std::string root = "/";
        if (argc >= 3 && argv[2][0] != '-') {
            root = argv[2];
        }
        std::cout << "Scanning for broken symlinks under " << root << " ...\n";
        auto res = mgr.audit(root);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& broken = res.value();
        if (broken.empty()) {
            std::cout << "No broken symlinks found.\n";
        } else {
            std::cout << "Found " << broken.size() << " broken symlink(s):\n";
            for (const auto& b : broken) {
                std::cout << "  " << b << "\n";
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // graph
    // -----------------------------------------------------------------------
    if (command == "graph") {
        auto res = mgr.graph();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // watch
    // -----------------------------------------------------------------------
    if (command == "watch") {
        if (argc < 3) {
            std::cerr << "Error: 'watch' requires <dir>\n";
            return 1;
        }
        std::string dir = argv[2];
        std::string pattern = "*";
        std::string dest;

        for (int i = 3; i < argc; ++i) {
            if ((std::strcmp(argv[i], "--pattern") == 0 ||
                 std::strcmp(argv[i], "-p") == 0) &&
                i + 1 < argc) {
                pattern = argv[++i];
            } else if ((std::strcmp(argv[i], "--dest") == 0 ||
                        std::strcmp(argv[i], "-d") == 0) &&
                       i + 1 < argc) {
                dest = argv[++i];
            }
        }

        auto res = mgr.watch(dir, pattern, dest);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // restore
    // -----------------------------------------------------------------------
    if (command == "restore") {
        auto res = mgr.restore();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Restored " << res.value() << " managed link(s).\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto links = mgr.list();
        if (links.empty()) {
            std::cout << "No managed links.\n";
            return 0;
        }
        std::cout << std::left
                  << std::setw(40) << "LINK"
                  << std::setw(40) << "TARGET"
                  << std::setw(8) << "STATUS"
                  << "TAG\n";
        std::cout << std::string(96, '-') << "\n";
        for (const auto& ml : links) {
            std::cout << std::left
                      << std::setw(40) << ml.link_path
                      << std::setw(40) << ml.target_path
                      << std::setw(8) << (ml.alive ? "ok" : "BROKEN")
                      << ml.tag << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // remove
    // -----------------------------------------------------------------------
    if (command == "remove") {
        if (argc < 3) {
            std::cerr << "Error: 'remove' requires <link-path>\n";
            return 1;
        }
        std::string link_path = argv[2];
        auto res = mgr.remove(link_path);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Removed: " << link_path << "\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
