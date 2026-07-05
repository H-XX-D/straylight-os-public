// tools/journal/main.cpp
// CLI front-end for straylight-journal — developer journal.

#include "journal_engine.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-journal — developer journal\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-journal add <title> [--body=X] [--tags=a,b] [--project=X]\n"
        << "  straylight-journal show <id>                          View entry\n"
        << "  straylight-journal list [--tag=X] [--project=X] [--query=X] [--limit=N]\n"
        << "  straylight-journal edit <id> [--title=X] [--body=X] [--tags=a,b] [--project=X]\n"
        << "  straylight-journal remove <id>                        Delete entry\n"
        << "  straylight-journal pin <id>                           Pin entry\n"
        << "  straylight-journal unpin <id>                         Unpin entry\n"
        << "  straylight-journal tags                               List all tags\n"
        << "  straylight-journal projects                           List all projects\n"
        << "  straylight-journal stats                              Journal statistics\n"
        << "  straylight-journal export [--tag=X] [--project=X]     Export as markdown\n";
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

static std::string get_arg(int argc, char* argv[],
                            const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static std::vector<std::string> parse_csv(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

static std::string pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

// ===========================================================================
// main
// ===========================================================================

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

    straylight::JournalEngine engine;

    // -----------------------------------------------------------------------
    // add <title> [--body=X] [--tags=a,b] [--project=X]
    // -----------------------------------------------------------------------
    if (command == "add") {
        if (argc < 3) {
            std::cerr << "Error: 'add' requires a title\n";
            return 1;
        }
        std::string title = argv[2];
        std::string body = get_arg(argc, argv, "--body=", 3);
        std::string tags_str = get_arg(argc, argv, "--tags=", 3);
        std::string project = get_arg(argc, argv, "--project=", 3);
        auto tags = parse_csv(tags_str);

        // If no body given, read from stdin
        if (body.empty()) {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (!body.empty()) body += "\n";
                body += line;
            }
        }

        auto res = engine.add(title, body, tags, project);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& e = res.value();
        std::cout << "Entry #" << e.id << " created: " << e.title << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // show <id>
    // -----------------------------------------------------------------------
    if (command == "show") {
        if (argc < 3) {
            std::cerr << "Error: 'show' requires an entry ID\n";
            return 1;
        }
        uint64_t id = std::stoull(argv[2]);
        auto res = engine.get(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& e = res.value();
        std::cout << "Entry #" << e.id;
        if (e.pinned) std::cout << " [pinned]";
        std::cout << "\n"
                  << "Title:   " << e.title << "\n"
                  << "Date:    " << e.timestamp << "\n";
        if (!e.project.empty())
            std::cout << "Project: " << e.project << "\n";
        if (!e.tags.empty()) {
            std::cout << "Tags:   ";
            for (const auto& t : e.tags) std::cout << " " << t;
            std::cout << "\n";
        }
        std::cout << "\n" << e.body << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // list [options]
    // -----------------------------------------------------------------------
    if (command == "list") {
        straylight::JournalFilter filter;
        filter.tag = get_arg(argc, argv, "--tag=", 2);
        filter.project = get_arg(argc, argv, "--project=", 2);
        filter.query = get_arg(argc, argv, "--query=", 2);
        filter.since = get_arg(argc, argv, "--since=", 2);
        filter.until = get_arg(argc, argv, "--until=", 2);
        std::string limit_str = get_arg(argc, argv, "--limit=", 2);
        if (!limit_str.empty()) filter.limit = std::stoi(limit_str);

        auto res = engine.list(filter);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& entries = res.value();
        if (entries.empty()) {
            std::cout << "No entries found.\n";
            return 0;
        }

        std::cout << pad("ID", 6) << pad("DATE", 22) << pad("PROJECT", 16)
                  << pad("TAGS", 24) << "TITLE\n"
                  << std::string(90, '-') << "\n";

        for (const auto& e : entries) {
            std::string tags_str;
            for (const auto& t : e.tags) {
                if (!tags_str.empty()) tags_str += ",";
                tags_str += t;
            }
            std::string id_str = std::to_string(e.id);
            if (e.pinned) id_str += "*";

            std::cout << pad(id_str, 6)
                      << pad(e.timestamp, 22)
                      << pad(e.project.empty() ? "-" : e.project, 16)
                      << pad(tags_str.empty() ? "-" : tags_str, 24)
                      << e.title << "\n";
        }
        std::cout << "\n" << entries.size() << " entries\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // edit <id> [options]
    // -----------------------------------------------------------------------
    if (command == "edit") {
        if (argc < 3) {
            std::cerr << "Error: 'edit' requires an entry ID\n";
            return 1;
        }
        uint64_t id = std::stoull(argv[2]);
        std::string title = get_arg(argc, argv, "--title=", 3);
        std::string body = get_arg(argc, argv, "--body=", 3);
        std::string tags_str = get_arg(argc, argv, "--tags=", 3);
        std::string project = get_arg(argc, argv, "--project=", 3);
        auto tags = parse_csv(tags_str);

        auto res = engine.edit(id, title, body, tags, project);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Entry #" << id << " updated.\n";
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
        auto res = engine.remove(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Entry #" << id << " removed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // pin / unpin <id>
    // -----------------------------------------------------------------------
    if (command == "pin" || command == "unpin") {
        if (argc < 3) {
            std::cerr << "Error: '" << command << "' requires an entry ID\n";
            return 1;
        }
        uint64_t id = std::stoull(argv[2]);
        bool pinned = (command == "pin");
        auto res = engine.pin(id, pinned);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Entry #" << id << (pinned ? " pinned" : " unpinned") << ".\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // tags
    // -----------------------------------------------------------------------
    if (command == "tags") {
        auto res = engine.tags();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& tags = res.value();
        if (tags.empty()) {
            std::cout << "No tags found.\n";
            return 0;
        }
        for (const auto& t : tags) std::cout << "  " << t << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // projects
    // -----------------------------------------------------------------------
    if (command == "projects") {
        auto res = engine.projects();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& projects = res.value();
        if (projects.empty()) {
            std::cout << "No projects found.\n";
            return 0;
        }
        for (const auto& p : projects) std::cout << "  " << p << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // stats
    // -----------------------------------------------------------------------
    if (command == "stats") {
        auto res = engine.stats();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& s = res.value();
        std::cout << "Journal Statistics:\n"
                  << "  Total entries:      " << s.total_entries << "\n"
                  << "  Unique tags:        " << s.total_tags << "\n"
                  << "  Projects:           " << s.total_projects << "\n"
                  << "  This week:          " << s.entries_this_week << "\n"
                  << "  This month:         " << s.entries_this_month << "\n";
        if (!s.oldest_entry.empty())
            std::cout << "  Oldest:             " << s.oldest_entry << "\n"
                      << "  Newest:             " << s.newest_entry << "\n";
        if (!s.top_tags.empty()) {
            std::cout << "\n  Top Tags:\n";
            for (const auto& [tag, count] : s.top_tags)
                std::cout << "    " << pad(tag, 20) << count << "\n";
        }
        if (!s.top_projects.empty()) {
            std::cout << "\n  Top Projects:\n";
            for (const auto& [proj, count] : s.top_projects)
                std::cout << "    " << pad(proj, 20) << count << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // export [options]
    // -----------------------------------------------------------------------
    if (command == "export") {
        straylight::JournalFilter filter;
        filter.tag = get_arg(argc, argv, "--tag=", 2);
        filter.project = get_arg(argc, argv, "--project=", 2);
        filter.query = get_arg(argc, argv, "--query=", 2);
        filter.limit = 0; // export all

        auto res = engine.export_markdown(filter);
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
