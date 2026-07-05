// tools/clip/main.cpp
// CLI front-end for straylight-clip — clipboard manager.

#include "clip_manager.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-clip — clipboard manager CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-clip paste                        Read current clipboard\n"
        << "  straylight-clip copy <text>                  Copy text to clipboard\n"
        << "  straylight-clip history [--search=X]         Show clipboard history\n"
        << "  straylight-clip pin <id>                     Pin a history entry\n"
        << "  straylight-clip unpin <id>                   Unpin a history entry\n"
        << "  straylight-clip clear                        Clear non-pinned history\n"
        << "  straylight-clip list                         List recent entries\n"
        << "  straylight-clip remove <id>                  Delete specific entry\n"
        << "  straylight-clip expire [--hours=168]         Expire old entries\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static std::string type_label(straylight::ClipType t) {
    switch (t) {
        case straylight::ClipType::Text: return "text";
        case straylight::ClipType::Image: return "image";
        case straylight::ClipType::Url: return "url";
        default: return "?";
    }
}

static void print_entry(const straylight::ClipEntry& e) {
    std::string pin_mark = e.pinned ? " [pinned]" : "";
    std::cout << std::left
              << std::setw(6) << e.id
              << std::setw(8) << type_label(e.type)
              << std::setw(22) << e.timestamp
              << pin_mark
              << "\n";
    std::cout << "  " << e.preview << "\n";
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

    straylight::ClipManager mgr;

    // -----------------------------------------------------------------------
    // paste
    // -----------------------------------------------------------------------
    if (command == "paste") {
        auto res = mgr.paste();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& entry = res.value();
        std::cout << entry.content << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // copy <text>
    // -----------------------------------------------------------------------
    if (command == "copy") {
        if (argc < 3) {
            std::cerr << "Error: 'copy' requires text argument\n";
            return 1;
        }
        // Join all remaining args as the text
        std::string text;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) text += " ";
            text += argv[i];
        }
        auto res = mgr.copy(text);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Copied to clipboard.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // history [--search=X] [--limit=N]
    // -----------------------------------------------------------------------
    if (command == "history") {
        std::string search = get_arg(argc, argv, "--search=");
        std::string limit_str = get_arg(argc, argv, "--limit=");
        int limit = limit_str.empty() ? 50 : std::atoi(limit_str.c_str());

        auto res = mgr.history(search, limit);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& entries = res.value();
        if (entries.empty()) {
            std::cout << "No clipboard history";
            if (!search.empty()) std::cout << " matching '" << search << "'";
            std::cout << ".\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(6) << "ID"
                  << std::setw(8) << "TYPE"
                  << std::setw(22) << "TIMESTAMP"
                  << "FLAGS\n";
        std::cout << std::string(60, '-') << "\n";

        for (const auto& e : entries) {
            print_entry(e);
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // pin <id>
    // -----------------------------------------------------------------------
    if (command == "pin") {
        if (argc < 3) {
            std::cerr << "Error: 'pin' requires an entry ID\n";
            return 1;
        }
        uint64_t id = std::stoull(argv[2]);
        auto res = mgr.pin(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Entry " << id << " pinned.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // unpin <id>
    // -----------------------------------------------------------------------
    if (command == "unpin") {
        if (argc < 3) {
            std::cerr << "Error: 'unpin' requires an entry ID\n";
            return 1;
        }
        uint64_t id = std::stoull(argv[2]);
        auto res = mgr.unpin(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Entry " << id << " unpinned.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // clear
    // -----------------------------------------------------------------------
    if (command == "clear") {
        auto res = mgr.clear();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Clipboard history cleared (pinned entries kept).\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        std::string limit_str = get_arg(argc, argv, "--limit=");
        int limit = limit_str.empty() ? 20 : std::atoi(limit_str.c_str());

        auto res = mgr.list(limit);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& entries = res.value();
        if (entries.empty()) {
            std::cout << "No clipboard entries.\n";
            return 0;
        }

        for (const auto& e : entries) {
            print_entry(e);
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // remove <id>
    // -----------------------------------------------------------------------
    if (command == "remove") {
        if (argc < 3) {
            std::cerr << "Error: 'remove' requires an entry ID\n";
            return 1;
        }
        uint64_t id = std::stoull(argv[2]);
        auto res = mgr.remove(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Entry " << id << " removed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // expire [--hours=168]
    // -----------------------------------------------------------------------
    if (command == "expire") {
        std::string hours_str = get_arg(argc, argv, "--hours=");
        int hours = hours_str.empty() ? 168 : std::atoi(hours_str.c_str());

        auto res = mgr.expire(hours);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Expired " << res.value() << " entries older than "
                  << hours << " hours.\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
