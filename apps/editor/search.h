// apps/editor/search.h
// StrayLight Editor — find/replace with regex support
#pragma once

#include "buffer.h"

#include <regex>
#include <string>
#include <vector>

namespace straylight::editor {

/// A single search match location.
struct Match {
    Position start;
    Position end;

    bool operator==(const Match&) const = default;
};

/// Options controlling search behaviour.
struct SearchOptions {
    bool case_sensitive  = true;
    bool whole_word      = false;
    bool use_regex       = false;
    bool wrap_around     = true;
};

/// Incremental find/replace engine operating over a Buffer.
class SearchEngine {
public:
    /// Set the query string. Recompiles the regex if use_regex is set.
    /// Returns false if the regex is invalid.
    bool set_query(std::string_view query, const SearchOptions& opts);

    const std::string& query()   const { return query_; }
    const SearchOptions& options() const { return opts_; }

    /// Find all matches in the buffer.
    std::vector<Match> find_all(const Buffer& buf) const;

    /// Find the next match after `from` (inclusive).
    /// Returns an empty optional if none found (and wrap_around is off).
    std::optional<Match> find_next(const Buffer& buf, Position from) const;

    /// Find the previous match before `from` (exclusive).
    std::optional<Match> find_prev(const Buffer& buf, Position from) const;

    /// Replace a single match. Returns the position after the replacement.
    Position replace_one(Buffer& buf,
                         const Match& m,
                         std::string_view replacement) const;

    /// Replace all matches. Returns the count replaced.
    int replace_all(Buffer& buf, std::string_view replacement) const;

    /// True when the last set_query succeeded.
    bool valid() const { return valid_; }

private:
    std::string    query_;
    SearchOptions  opts_;
    std::regex     regex_;
    bool           valid_ = false;

    // Plain-text search helpers
    std::optional<Match> plain_search_forward (const Buffer& buf,
                                               size_t start_logical) const;
    std::optional<Match> plain_search_backward(const Buffer& buf,
                                               size_t start_logical) const;

    // Regex search helpers
    std::optional<Match> regex_search_forward (const Buffer& buf,
                                               size_t start_logical) const;

    // Shared utility: logical offset → Position (uses buffer's line data).
    static Position offset_to_pos(const Buffer& buf, size_t offset);
    static size_t   pos_to_offset(const Buffer& buf, Position pos);

    bool matches_word_boundary(const std::string& full_text,
                                size_t start, size_t len) const;
};

} // namespace straylight::editor
