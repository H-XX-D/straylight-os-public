// tools/git-helper/git_helper.cpp
// Full git workflow helper implementation for StrayLight OS.

#include "git_helper.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

GitHelper::GitHelper() {
    auto res = find_repo();
    if (res.has_value()) {
        repo_root_ = res.value();
    }
}

GitHelper::~GitHelper() = default;

Result<std::string, std::string> GitHelper::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0 && output.empty()) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

Result<std::string, std::string> GitHelper::run_git(const std::string& args) const {
    std::string cmd = "git";
    if (!repo_root_.empty()) {
        cmd += " -C '" + repo_root_ + "'";
    }
    cmd += " " + args + " 2>&1";
    return run_cmd(cmd);
}

Result<std::string, std::string> GitHelper::find_repo() const {
    auto res = run_cmd("git rev-parse --show-toplevel 2>/dev/null");
    if (!res.has_value()) {
        return Result<std::string, std::string>::error("not inside a git repository");
    }
    std::string root = res.value();
    if (!root.empty() && root.back() == '\n') root.pop_back();
    return Result<std::string, std::string>::ok(root);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

Result<std::vector<GitFileStatus>, std::string> GitHelper::status() const {
    auto res = run_git("status --porcelain=v2 --branch");
    if (!res.has_value()) {
        return Result<std::vector<GitFileStatus>, std::string>::error(res.error());
    }

    std::vector<GitFileStatus> files;
    std::istringstream stream(res.value());
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        GitFileStatus entry;

        if (line[0] == '1') {
            // Ordinary changed entry: 1 XY sub mH mI mW hH hI path
            char xy[3] = {};
            if (line.size() > 3) {
                xy[0] = line[2];
                xy[1] = line[3];
            }

            // Extract path (last field after spaces)
            auto fields = 0;
            size_t pos = 0;
            for (int f = 0; f < 8 && pos < line.size(); ++f) {
                pos = line.find(' ', pos);
                if (pos != std::string::npos) ++pos;
            }
            if (pos < line.size()) entry.path = line.substr(pos);

            // Interpret XY
            if (xy[0] == 'A') { entry.state = "added"; entry.staged = true; }
            else if (xy[0] == 'M') { entry.state = "modified"; entry.staged = true; }
            else if (xy[0] == 'D') { entry.state = "deleted"; entry.staged = true; }
            else if (xy[1] == 'M') { entry.state = "modified"; entry.staged = false; }
            else if (xy[1] == 'D') { entry.state = "deleted"; entry.staged = false; }
            else if (xy[1] == 'A') { entry.state = "added"; entry.staged = false; }

            files.push_back(entry);
        } else if (line[0] == '2') {
            // Renamed entry: 2 XY sub mH mI mW hH hI Xscore path\toldpath
            entry.state = "renamed";
            entry.staged = true;

            auto tab = line.find('\t');
            if (tab != std::string::npos) {
                entry.old_path = line.substr(tab + 1);
                // path is before tab, last space-separated field
                auto fields = 0;
                size_t pos = 0;
                for (int f = 0; f < 9 && pos < line.size(); ++f) {
                    pos = line.find(' ', pos);
                    if (pos != std::string::npos) ++pos;
                }
                if (pos < tab) entry.path = line.substr(pos, tab - pos);
            }
            files.push_back(entry);
        } else if (line[0] == 'u') {
            // Unmerged entry
            entry.state = "conflict";
            auto fields = 0;
            size_t pos = 0;
            for (int f = 0; f < 10 && pos < line.size(); ++f) {
                pos = line.find(' ', pos);
                if (pos != std::string::npos) ++pos;
            }
            if (pos < line.size()) entry.path = line.substr(pos);
            files.push_back(entry);
        } else if (line[0] == '?') {
            // Untracked
            entry.state = "untracked";
            if (line.size() > 2) entry.path = line.substr(2);
            files.push_back(entry);
        }
    }

    // Sort: conflicts first, then staged, then modified, then untracked
    auto priority = [](const std::string& state) -> int {
        if (state == "conflict") return 0;
        if (state == "added") return 1;
        if (state == "modified") return 2;
        if (state == "deleted") return 3;
        if (state == "renamed") return 4;
        if (state == "untracked") return 5;
        return 6;
    };

    std::sort(files.begin(), files.end(),
              [&](const auto& a, const auto& b) {
                  int pa = priority(a.state);
                  int pb = priority(b.state);
                  if (pa != pb) return pa < pb;
                  return a.path < b.path;
              });

    return Result<std::vector<GitFileStatus>, std::string>::ok(files);
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

Result<std::string, std::string> GitHelper::commit(const std::string& message,
                                                     const std::string& type,
                                                     bool all) {
    std::string full_msg = message;
    if (!type.empty()) {
        full_msg = type + ": " + message;
    }

    std::string cmd = "commit";
    if (all) cmd += " -a";
    cmd += " -m '" + full_msg + "'";

    auto res = run_git(cmd);
    if (!res.has_value()) {
        return Result<std::string, std::string>::error("commit failed: " + res.error());
    }

    // Get the resulting commit hash
    auto hash_res = run_git("rev-parse --short HEAD");
    std::string hash = hash_res.has_value() ? hash_res.value() : "unknown";
    if (!hash.empty() && hash.back() == '\n') hash.pop_back();

    return Result<std::string, std::string>::ok(hash);
}

// ---------------------------------------------------------------------------
// Branches
// ---------------------------------------------------------------------------

Result<std::vector<GitBranch>, std::string> GitHelper::branch_list() const {
    auto res = run_git("branch -vv --no-color");
    if (!res.has_value()) {
        return Result<std::vector<GitBranch>, std::string>::error(res.error());
    }

    std::vector<GitBranch> branches;
    std::istringstream stream(res.value());
    std::string line;

    while (std::getline(stream, line)) {
        if (line.size() < 2) continue;

        GitBranch branch;
        branch.current = (line[0] == '*');

        std::string rest = line.substr(2);
        auto pos = rest.find_first_not_of(" \t");
        if (pos != std::string::npos) rest = rest.substr(pos);

        // Name is first word
        auto space = rest.find(' ');
        branch.name = rest.substr(0, space);

        if (space != std::string::npos) {
            std::string info = rest.substr(space);

            // Extract upstream tracking [origin/main: ahead 2, behind 1]
            std::regex track_re(R"(\[([^\]]+)\])");
            std::smatch m;
            if (std::regex_search(info, m, track_re)) {
                std::string tracking = m[1].str();
                auto colon = tracking.find(':');
                if (colon != std::string::npos) {
                    branch.upstream = tracking.substr(0, colon);
                    std::string stats = tracking.substr(colon + 1);
                    std::regex ahead_re(R"(ahead\s+(\d+))");
                    std::regex behind_re(R"(behind\s+(\d+))");
                    if (std::regex_search(stats, m, ahead_re)) branch.ahead = std::stoi(m[1].str());
                    if (std::regex_search(stats, m, behind_re)) branch.behind = std::stoi(m[1].str());
                } else {
                    branch.upstream = tracking;
                }
            }

            // Extract last commit hash and message
            std::regex commit_re(R"(([0-9a-f]{7,})\s+(.+))");
            if (std::regex_search(info, m, commit_re)) {
                branch.last_commit = m[2].str();
            }
        }

        branches.push_back(branch);
    }

    return Result<std::vector<GitBranch>, std::string>::ok(branches);
}

Result<void, std::string> GitHelper::branch_create(const std::string& name) {
    auto res = run_git("checkout -b " + name);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to create branch: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> GitHelper::branch_switch(const std::string& name) {
    auto res = run_git("checkout " + name);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to switch branch: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> GitHelper::branch_delete(const std::string& name) {
    auto res = run_git("branch -d " + name);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to delete branch: " + res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Stash
// ---------------------------------------------------------------------------

Result<void, std::string> GitHelper::stash_save(const std::string& name) {
    std::string cmd = "stash push";
    if (!name.empty()) {
        cmd += " -m '" + name + "'";
    }
    auto res = run_git(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("stash failed: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> GitHelper::stash_pop(int index) {
    auto res = run_git("stash pop stash@{" + std::to_string(index) + "}");
    if (!res.has_value()) {
        return Result<void, std::string>::error("stash pop failed: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<std::vector<GitStash>, std::string> GitHelper::stash_list() const {
    auto res = run_git("stash list --format='%gd|%gs|%cr'");
    if (!res.has_value()) {
        return Result<std::vector<GitStash>, std::string>::ok(std::vector<GitStash>{});
    }

    std::vector<GitStash> stashes;
    std::istringstream stream(res.value());
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        GitStash stash;
        std::istringstream ls(line);
        std::string field;

        if (std::getline(ls, field, '|')) {
            // stash@{N}
            std::regex idx_re(R"(\{(\d+)\})");
            std::smatch m;
            if (std::regex_search(field, m, idx_re)) {
                stash.index = std::stoi(m[1].str());
            }
            stash.name = field;
        }
        if (std::getline(ls, field, '|')) stash.message = field;
        if (std::getline(ls, field, '|')) stash.timestamp = field;

        stashes.push_back(stash);
    }

    return Result<std::vector<GitStash>, std::string>::ok(stashes);
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

Result<std::vector<GitLogEntry>, std::string> GitHelper::log(int count, bool graph) const {
    std::string fmt = "--format='%H|%h|%an|%cr|%s|%D'";
    std::string cmd = "log -" + std::to_string(count) + " " + fmt;
    if (graph) cmd += " --graph --oneline --decorate --all";

    // If graph mode, return raw output as a single entry
    if (graph) {
        auto res = run_git("log -" + std::to_string(count) +
                           " --graph --oneline --decorate --all --no-color");
        if (!res.has_value()) {
            return Result<std::vector<GitLogEntry>, std::string>::error(res.error());
        }
        // Return as single entry with the graph as message
        GitLogEntry entry;
        entry.message = res.value();
        return Result<std::vector<GitLogEntry>, std::string>::ok(
            std::vector<GitLogEntry>{entry});
    }

    auto res = run_git(cmd);
    if (!res.has_value()) {
        return Result<std::vector<GitLogEntry>, std::string>::error(res.error());
    }

    std::vector<GitLogEntry> entries;
    std::istringstream stream(res.value());
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        // Remove surrounding quotes if present
        if (line.front() == '\'') line = line.substr(1);
        if (!line.empty() && line.back() == '\'') line.pop_back();

        GitLogEntry entry;
        std::istringstream ls(line);
        std::string field;

        if (std::getline(ls, field, '|')) entry.hash = field;
        if (std::getline(ls, field, '|')) entry.short_hash = field;
        if (std::getline(ls, field, '|')) entry.author = field;
        if (std::getline(ls, field, '|')) entry.date = field;
        if (std::getline(ls, field, '|')) entry.message = field;
        if (std::getline(ls, field, '|')) entry.refs = field;

        entries.push_back(entry);
    }

    return Result<std::vector<GitLogEntry>, std::string>::ok(entries);
}

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------

Result<std::string, std::string> GitHelper::diff(bool staged) const {
    std::string cmd = "diff --stat";
    if (staged) cmd += " --cached";
    cmd += " --no-color";

    auto res = run_git(cmd);
    if (!res.has_value()) {
        return Result<std::string, std::string>::error(res.error());
    }

    // Also get full diff for detail
    std::string full_cmd = "diff";
    if (staged) full_cmd += " --cached";
    full_cmd += " --no-color";

    auto full_res = run_git(full_cmd);
    std::string output = res.value();
    if (full_res.has_value()) {
        output += "\n" + full_res.value();
    }

    return Result<std::string, std::string>::ok(output);
}

// ---------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------

Result<std::string, std::string> GitHelper::sync() const {
    std::string output;

    auto pull_res = run_git("pull --rebase 2>&1");
    if (pull_res.has_value()) {
        output += "Pull: " + pull_res.value();
    } else {
        output += "Pull failed: " + pull_res.error() + "\n";
    }

    auto push_res = run_git("push 2>&1");
    if (push_res.has_value()) {
        output += "Push: " + push_res.value();
    } else {
        output += "Push failed: " + push_res.error() + "\n";
    }

    return Result<std::string, std::string>::ok(output);
}

} // namespace straylight
