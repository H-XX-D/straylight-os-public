// tools/git-helper/main.cpp
// CLI front-end for straylight-git — git workflow helper.

#include "git_helper.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-git — git workflow helper CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-git status                            Colored grouped status\n"
        << "  straylight-git commit [--type=feat|fix|docs] <msg>  Conventional commit\n"
        << "  straylight-git branch [list|create|switch|delete] [name]\n"
        << "  straylight-git stash [save|pop|list] [name]      Named stashes\n"
        << "  straylight-git log [--graph] [--count=N]         Commit log\n"
        << "  straylight-git diff [--staged]                   Diff summary\n"
        << "  straylight-git sync                              Pull + push\n";
}

static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static bool has_flag(int argc, char* argv[], const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::string color(const std::string& code, const std::string& text) {
    return "\033[" + code + "m" + text + "\033[0m";
}

static std::string state_color(const std::string& state) {
    if (state == "conflict") return color("1;31", state);   // bold red
    if (state == "modified") return color("33", state);      // yellow
    if (state == "added") return color("32", state);         // green
    if (state == "deleted") return color("31", state);       // red
    if (state == "renamed") return color("36", state);       // cyan
    if (state == "untracked") return color("90", state);     // gray
    return state;
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

    straylight::GitHelper git;

    // -----------------------------------------------------------------------
    // status
    // -----------------------------------------------------------------------
    if (command == "status") {
        auto repo = git.find_repo();
        if (repo.has_value()) {
            std::cout << color("1", "Repository: ") << repo.value() << "\n\n";
        }

        auto res = git.status();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& files = res.value();
        if (files.empty()) {
            std::cout << color("32", "Working tree clean.") << "\n";
            return 0;
        }

        std::string last_state;
        for (const auto& f : files) {
            if (f.state != last_state) {
                if (!last_state.empty()) std::cout << "\n";
                std::cout << color("1", "  " + f.state + ":") << "\n";
                last_state = f.state;
            }

            std::string staged_mark = f.staged ? color("32", "[staged] ") : "         ";
            std::cout << "    " << staged_mark << state_color(f.state) << "  " << f.path;
            if (!f.old_path.empty()) std::cout << " <- " << f.old_path;
            std::cout << "\n";
        }
        std::cout << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // commit [--type=X] [--all] <message>
    // -----------------------------------------------------------------------
    if (command == "commit") {
        std::string type = get_arg(argc, argv, "--type=");
        bool all = has_flag(argc, argv, "--all") || has_flag(argc, argv, "-a");

        // Collect non-flag arguments as the message
        std::string message;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) == 0 || arg == "-a") continue;
            if (!message.empty()) message += " ";
            message += arg;
        }

        if (message.empty()) {
            std::cerr << "Error: commit requires a message\n";
            return 1;
        }

        auto res = git.commit(message, type, all);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << color("32", "Committed ") << "[" << res.value() << "] "
                  << (type.empty() ? "" : type + ": ") << message << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // branch [list|create|switch|delete] [name]
    // -----------------------------------------------------------------------
    if (command == "branch") {
        std::string sub = (argc >= 3) ? argv[2] : "list";

        if (sub == "list") {
            auto res = git.branch_list();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            for (const auto& b : res.value()) {
                std::string marker = b.current ? "* " : "  ";
                std::string name_str = b.current ? color("32", b.name) : b.name;
                std::cout << marker << std::left << std::setw(30) << name_str;
                if (!b.upstream.empty()) {
                    std::cout << " [" << b.upstream;
                    if (b.ahead > 0) std::cout << " +" << b.ahead;
                    if (b.behind > 0) std::cout << " -" << b.behind;
                    std::cout << "]";
                }
                if (!b.last_commit.empty()) {
                    std::cout << " " << color("90", b.last_commit);
                }
                std::cout << "\n";
            }
            return 0;
        }

        if (sub == "create" || sub == "switch" || sub == "delete") {
            if (argc < 4) {
                std::cerr << "Error: 'branch " << sub << "' requires a name\n";
                return 1;
            }
            std::string name = argv[3];
            Result<void, std::string> res = (sub == "create") ? git.branch_create(name) :
                                             (sub == "switch") ? git.branch_switch(name) :
                                             git.branch_delete(name);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Branch '" << name << "' " << sub << "d.\n";
            return 0;
        }

        // If sub looks like a branch name, treat as switch
        auto res = git.branch_switch(sub);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Switched to '" << sub << "'\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // stash [save|pop|list] [name]
    // -----------------------------------------------------------------------
    if (command == "stash") {
        std::string sub = (argc >= 3) ? argv[2] : "list";

        if (sub == "save") {
            std::string name;
            for (int i = 3; i < argc; ++i) {
                if (!name.empty()) name += " ";
                name += argv[i];
            }
            auto res = git.stash_save(name);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Stashed" << (name.empty() ? "" : ": " + name) << "\n";
            return 0;
        }

        if (sub == "pop") {
            int index = (argc >= 4) ? std::atoi(argv[3]) : 0;
            auto res = git.stash_pop(index);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Stash applied.\n";
            return 0;
        }

        if (sub == "list") {
            auto res = git.stash_list();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& stashes = res.value();
            if (stashes.empty()) {
                std::cout << "No stashes.\n";
                return 0;
            }
            for (const auto& s : stashes) {
                std::cout << color("33", s.name) << " " << s.message;
                if (!s.timestamp.empty()) std::cout << " (" << s.timestamp << ")";
                std::cout << "\n";
            }
            return 0;
        }

        std::cerr << "Error: unknown stash subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // log [--graph] [--count=N]
    // -----------------------------------------------------------------------
    if (command == "log") {
        bool graph = has_flag(argc, argv, "--graph");
        std::string count_str = get_arg(argc, argv, "--count=");
        int count = count_str.empty() ? 20 : std::atoi(count_str.c_str());

        auto res = git.log(count, graph);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        if (graph) {
            // Graph mode: single entry with raw output
            if (!res.value().empty()) {
                std::cout << res.value()[0].message;
            }
            return 0;
        }

        for (const auto& entry : res.value()) {
            std::cout << color("33", entry.short_hash) << " "
                      << entry.message;
            if (!entry.refs.empty()) {
                std::cout << " " << color("31", "(" + entry.refs + ")");
            }
            std::cout << "\n"
                      << "  " << color("90", entry.author + " - " + entry.date) << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // diff [--staged]
    // -----------------------------------------------------------------------
    if (command == "diff") {
        bool staged = has_flag(argc, argv, "--staged") || has_flag(argc, argv, "--cached");
        auto res = git.diff(staged);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // sync
    // -----------------------------------------------------------------------
    if (command == "sync") {
        std::cout << "Syncing...\n";
        auto res = git.sync();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
