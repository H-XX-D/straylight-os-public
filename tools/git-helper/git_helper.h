// tools/git-helper/git_helper.h
// Git workflow helper for StrayLight OS — project-aware git operations.
#pragma once

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight {

/// A file entry in git status, grouped by state.
struct GitFileStatus {
    std::string path;
    std::string state;      // "modified", "added", "deleted", "renamed", "untracked", "conflict"
    bool        staged = false;
    std::string old_path;   // for renames
};

/// Branch information.
struct GitBranch {
    std::string name;
    bool        current = false;
    std::string upstream;
    int         ahead = 0;
    int         behind = 0;
    std::string last_commit;
    std::string last_author;
};

/// Stash entry.
struct GitStash {
    int         index = 0;
    std::string name;
    std::string branch;
    std::string message;
    std::string timestamp;
};

/// Commit log entry.
struct GitLogEntry {
    std::string hash;
    std::string short_hash;
    std::string author;
    std::string date;
    std::string message;
    std::string refs;       // branch/tag decorations
};

class GitHelper {
public:
    GitHelper();
    ~GitHelper();

    /// Auto-detect git repository root from current directory.
    Result<std::string, std::string> find_repo() const;

    /// Get colored, grouped status.
    Result<std::vector<GitFileStatus>, std::string> status() const;

    /// Commit with conventional commit type (feat, fix, docs, refactor, test, chore).
    Result<std::string, std::string> commit(const std::string& message,
                                              const std::string& type = "",
                                              bool all = false);

    /// Branch operations.
    Result<std::vector<GitBranch>, std::string> branch_list() const;
    Result<void, std::string> branch_create(const std::string& name);
    Result<void, std::string> branch_switch(const std::string& name);
    Result<void, std::string> branch_delete(const std::string& name);

    /// Stash operations.
    Result<void, std::string> stash_save(const std::string& name = "");
    Result<void, std::string> stash_pop(int index = 0);
    Result<std::vector<GitStash>, std::string> stash_list() const;

    /// Log with optional graph format.
    Result<std::vector<GitLogEntry>, std::string> log(int count = 20, bool graph = false) const;

    /// Diff summary.
    Result<std::string, std::string> diff(bool staged = false) const;

    /// Sync: pull + push in one operation.
    Result<std::string, std::string> sync() const;

private:
    Result<std::string, std::string> run_git(const std::string& args) const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    std::string repo_root_;
};

} // namespace straylight
