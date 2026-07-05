// tools/journal/journal_engine.h
// Developer journal engine for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// A single journal entry.
struct JournalEntry {
    uint64_t    id = 0;
    std::string timestamp;
    std::string title;
    std::string body;
    std::vector<std::string> tags;
    std::string project;
    bool        pinned = false;
};

/// Search filter for queries.
struct JournalFilter {
    std::string query;
    std::string tag;
    std::string project;
    std::string since;
    std::string until;
    int         limit = 50;
    bool        pinned_only = false;
};

/// Journal statistics.
struct JournalStats {
    int total_entries = 0;
    int total_tags = 0;
    int total_projects = 0;
    int entries_this_week = 0;
    int entries_this_month = 0;
    std::string oldest_entry;
    std::string newest_entry;
    std::vector<std::pair<std::string, int>> top_tags;
    std::vector<std::pair<std::string, int>> top_projects;
};

class JournalEngine {
public:
    JournalEngine();
    ~JournalEngine();

    Result<JournalEntry, std::string> add(const std::string& title,
                                           const std::string& body,
                                           const std::vector<std::string>& tags,
                                           const std::string& project);
    Result<JournalEntry, std::string> get(uint64_t id) const;
    Result<std::vector<JournalEntry>, std::string> list(const JournalFilter& filter) const;
    Result<void, std::string> remove(uint64_t id);
    Result<JournalEntry, std::string> edit(uint64_t id,
                                            const std::string& title,
                                            const std::string& body,
                                            const std::vector<std::string>& tags,
                                            const std::string& project);
    Result<void, std::string> pin(uint64_t id, bool pinned);
    Result<std::vector<std::string>, std::string> tags() const;
    Result<std::vector<std::string>, std::string> projects() const;
    Result<JournalStats, std::string> stats() const;
    Result<std::string, std::string> export_markdown(const JournalFilter& filter) const;

private:
    std::string journal_dir_;
    std::string index_path_;

    Result<void, std::string> ensure_dirs() const;
    std::string entry_path(uint64_t id) const;
    Result<std::vector<JournalEntry>, std::string> load_all() const;
    Result<void, std::string> save_entry(const JournalEntry& entry) const;
    Result<JournalEntry, std::string> load_entry(const std::string& path) const;
    uint64_t next_id() const;
    std::string now_timestamp() const;
    bool matches_filter(const JournalEntry& entry, const JournalFilter& filter) const;
};

} // namespace straylight
