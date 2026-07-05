// tools/alias/main.cpp
// straylight-alias — command alias manager CLI.
#include "alias_manager.h"

#include <cstring>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-alias — command alias manager\n\n"
        << "Usage:\n"
        << "  straylight-alias add <name> <command> [--category <cat>] [--desc <d>]\n"
        << "  straylight-alias remove <name>\n"
        << "  straylight-alias list [--category=<cat>]\n"
        << "  straylight-alias search <query>\n"
        << "  straylight-alias import [<rc-file>]\n"
        << "  straylight-alias export\n"
        << "  straylight-alias sync\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    std::string command = argv[1];
    if (command == "--help" || command == "-h") { print_usage(); return 0; }

    straylight::AliasManager mgr;
    mgr.load();

    if (command == "add") {
        if (argc < 4) { std::cerr << "Error: need name and command\n"; return 1; }
        std::string cat = "custom", desc;
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--category") == 0 && i+1 < argc) cat = argv[++i];
            else if (std::strcmp(argv[i], "--desc") == 0 && i+1 < argc) desc = argv[++i];
        }
        auto r = mgr.add(argv[2], argv[3], cat, desc);
        if (r.has_value()) std::cout << "Alias '" << argv[2] << "' added.\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (command == "remove") {
        if (argc < 3) { std::cerr << "Error: need name\n"; return 1; }
        auto r = mgr.remove(argv[2]);
        if (r.has_value()) std::cout << "Alias '" << argv[2] << "' removed.\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (command == "list") {
        std::string cat;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--category=", 0) == 0) cat = a.substr(11);
        }
        auto aliases = mgr.list(cat);
        if (aliases.empty()) { std::cout << "No aliases found.\n"; return 0; }
        std::string cc;
        for (const auto& a : aliases) {
            if (a.category != cc) { cc = a.category; std::cout << "\n\033[1;33m[" << cc << "]\033[0m\n"; }
            std::cout << "  \033[36m" << a.name << "\033[0m = " << a.command;
            if (!a.description.empty()) std::cout << "  \033[90m# " << a.description << "\033[0m";
            std::cout << "\n";
        }
        return 0;
    }

    if (command == "search") {
        if (argc < 3) { std::cerr << "Error: need query\n"; return 1; }
        auto results = mgr.search(argv[2]);
        if (results.empty()) { std::cout << "No matches.\n"; return 0; }
        for (const auto& a : results)
            std::cout << "  \033[36m" << a.name << "\033[0m = " << a.command
                      << "  \033[90m[" << a.category << "]\033[0m\n";
        return 0;
    }

    if (command == "import") {
        std::string rc;
        if (argc >= 3) { rc = argv[2]; }
        else {
            const char* home = std::getenv("HOME");
            if (home) {
                std::string z = std::string(home) + "/.zshrc";
                std::string b = std::string(home) + "/.bashrc";
                if (std::filesystem::exists(z)) rc = z;
                else if (std::filesystem::exists(b)) rc = b;
            }
        }
        if (rc.empty()) { std::cerr << "Error: no rc file found\n"; return 1; }
        auto r = mgr.import_from_shell(rc);
        if (r.has_value()) std::cout << "Imported " << r.value() << " aliases from " << rc << "\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (command == "export") { std::cout << mgr.export_shell(); return 0; }
    if (command == "sync") {
        auto r = mgr.sync();
        if (r.has_value()) std::cout << "Aliases reloaded.\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
