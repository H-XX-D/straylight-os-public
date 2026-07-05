// tools/keybind/main.cpp
// straylight-keybind — keyboard shortcut manager CLI.
#include "keybind_manager.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-keybind — keyboard shortcut manager\n\n"
        << "Usage:\n"
        << "  straylight-keybind list [--category=<cat>]\n"
        << "  straylight-keybind set <action> <keys>\n"
        << "  straylight-keybind remove <action>\n"
        << "  straylight-keybind conflicts\n"
        << "  straylight-keybind reset [action|all]\n"
        << "  straylight-keybind export [--output <file>]\n"
        << "  straylight-keybind import <file>\n"
        << "  straylight-keybind apply\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") { print_usage(); return 0; }

    straylight::KeybindManager mgr;
    mgr.load();

    if (cmd == "list") {
        std::string cat;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--category=", 0) == 0) cat = a.substr(11);
        }
        auto binds = mgr.list(cat);
        if (binds.empty()) { std::cout << "No keybindings.\n"; return 0; }
        std::string cc;
        for (const auto& kb : binds) {
            if (kb.category != cc) { cc = kb.category; std::cout << "\n\033[1;33m[" << cc << "]\033[0m\n"; }
            std::string mod = kb.is_default ? "" : " \033[35m(modified)\033[0m";
            printf("  \033[36m%-25s\033[0m  %-25s  %s%s\n",
                   kb.action.c_str(), kb.keys.c_str(), kb.description.c_str(), mod.c_str());
        }
        return 0;
    }

    if (cmd == "set") {
        if (argc < 4) { std::cerr << "Error: need action and keys\n"; return 1; }
        auto r = mgr.set(argv[2], argv[3]);
        if (r.has_value()) {
            std::cout << "Bound '" << argv[2] << "' to " << argv[3] << "\n";
            for (const auto& c : mgr.find_conflicts())
                if (c.action1 == argv[2] || c.action2 == argv[2])
                    std::cout << "\033[33mWarning: conflict with '"
                              << (c.action1 == argv[2] ? c.action2 : c.action1)
                              << "' (" << c.keys << ")\033[0m\n";
        } else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (cmd == "remove") {
        if (argc < 3) { std::cerr << "Error: need action\n"; return 1; }
        auto r = mgr.remove(argv[2]);
        if (r.has_value()) std::cout << "Removed '" << argv[2] << "'.\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (cmd == "conflicts") {
        auto c = mgr.find_conflicts();
        if (c.empty()) std::cout << "\033[32mNo conflicts.\033[0m\n";
        else {
            std::cout << "\033[31mConflicts:\033[0m\n";
            for (const auto& x : c)
                std::cout << "  " << x.keys << ": '" << x.action1 << "' vs '" << x.action2 << "'\n";
        }
        return 0;
    }

    if (cmd == "reset") {
        std::string target;
        if (argc >= 3) target = argv[2];
        if (target == "all") target = "";
        auto r = mgr.reset(target);
        if (r.has_value()) std::cout << (target.empty() ? "All reset." : target + " reset.") << "\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (cmd == "export") {
        std::string out;
        for (int i = 2; i < argc; ++i)
            if (std::strcmp(argv[i], "--output") == 0 && i+1 < argc) out = argv[++i];
        std::string json = mgr.export_json();
        if (out.empty()) std::cout << json << "\n";
        else { std::ofstream ofs(out); if (!ofs) { std::cerr << "Cannot write\n"; return 1; }
               ofs << json << "\n"; std::cout << "Exported to " << out << "\n"; }
        return 0;
    }

    if (cmd == "import") {
        if (argc < 3) { std::cerr << "Error: need file\n"; return 1; }
        std::ifstream ifs(argv[2]);
        if (!ifs) { std::cerr << "Cannot read " << argv[2] << "\n"; return 1; }
        std::string c((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        auto r = mgr.import_json(c);
        if (r.has_value()) std::cout << "Imported " << r.value() << " keybindings.\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (cmd == "apply") {
        auto r = mgr.apply_to_compositor();
        if (r.has_value()) std::cout << "\033[32mKeybindings applied.\033[0m\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage();
    return 1;
}
