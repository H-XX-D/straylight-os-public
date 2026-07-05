// apps/editor/search.cpp
// StrayLight Editor — find/replace implementation
#include "search.h"

#include <algorithm>
#include <cctype>

namespace straylight::editor {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Position SearchEngine::offset_to_pos(const Buffer& buf, size_t offset) {
    const int total_lines = buf.line_count();
    // Walk lines to find which one contains the offset
    size_t accumulated = 0;
    for (int li = 0; li < total_lines; ++li) {
        const size_t line_bytes =
            static_cast<size_t>(buf.line_length(li)) + 1; // +1 for '\n'
        if (offset < accumulated + line_bytes) {
            int col = static_cast<int>(offset - accumulated);
            // Clamp col to actual line length
            col = std::min(col, buf.line_length(li));
            return {li, col};
        }
        accumulated += line_bytes;
    }
    // End of buffer
    return {total_lines - 1, buf.line_length(total_lines - 1)};
}

size_t SearchEngine::pos_to_offset(const Buffer& buf, Position pos) {
    size_t offset = 0;
    for (int li = 0; li < pos.line && li < buf.line_count(); ++li) {
        offset += static_cast<size_t>(buf.line_length(li)) + 1; // +1 for '\n'
    }
    offset += static_cast<size_t>(std::max(0, pos.col));
    return offset;
}

bool SearchEngine::matches_word_boundary(const std::string& text,
                                          size_t start, size_t len) const {
    auto is_word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    const bool before_ok = (start == 0) || !is_word(text[start - 1]);
    const bool after_ok  = (start + len >= text.size()) ||
                            !is_word(text[start + len]);
    return before_ok && after_ok;
}

// ---------------------------------------------------------------------------
// set_query
// ---------------------------------------------------------------------------

bool SearchEngine::set_query(std::string_view query, const SearchOptions& opts) {
    query_ = std::string(query);
    opts_  = opts;
    valid_ = false;

    if (query_.empty()) {
        valid_ = true;
        return true;
    }

    if (opts_.use_regex) {
        try {
            auto flags = std::regex::ECMAScript | std::regex::optimize;
            if (!opts_.case_sensitive) {
                flags |= std::regex::icase;
            }
            regex_ = std::regex(query_, flags);
            valid_ = true;
        } catch (const std::regex_error&) {
            valid_ = false;
        }
    } else {
        valid_ = true;
    }

    return valid_;
}

// ---------------------------------------------------------------------------
// find_all
// ---------------------------------------------------------------------------

std::vector<Match> SearchEngine::find_all(const Buffer& buf) const {
    if (!valid_ || query_.empty()) return {};

    std::vector<Match> results;
    const std::string full = buf.text();

    if (opts_.use_regex) {
        auto flags = std::regex_constants::match_default;
        auto begin = std::sregex_iterator(full.begin(), full.end(), regex_, flags);
        const auto end = std::sregex_iterator{};

        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            const size_t s = static_cast<size_t>(m.position(0));
            const size_t e = s + static_cast<size_t>(m.length(0));
            if (opts_.whole_word && !matches_word_boundary(full, s, e - s)) continue;
            results.push_back({offset_to_pos(buf, s), offset_to_pos(buf, e)});
        }
    } else {
        std::string haystack = full;
        std::string needle   = query_;

        if (!opts_.case_sensitive) {
            std::transform(haystack.begin(), haystack.end(),
                           haystack.begin(), [](unsigned char c){ return std::tolower(c); });
            std::transform(needle.begin(), needle.end(),
                           needle.begin(), [](unsigned char c){ return std::tolower(c); });
        }

        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            if (!opts_.whole_word || matches_word_boundary(haystack, pos, needle.size())) {
                const size_t e = pos + needle.size();
                results.push_back({offset_to_pos(buf, pos), offset_to_pos(buf, e)});
            }
            pos += needle.empty() ? 1 : needle.size();
        }
    }

    return results;
}

// ---------------------------------------------------------------------------
// find_next / find_prev
// ---------------------------------------------------------------------------

std::optional<Match> SearchEngine::find_next(const Buffer& buf,
                                              Position from) const {
    if (!valid_ || query_.empty()) return std::nullopt;

    const size_t start_offset = pos_to_offset(buf, from);

    if (opts_.use_regex) {
        auto result = regex_search_forward(buf, start_offset);
        if (!result && opts_.wrap_around) {
            result = regex_search_forward(buf, 0);
        }
        return result;
    } else {
        auto result = plain_search_forward(buf, start_offset);
        if (!result && opts_.wrap_around) {
            result = plain_search_forward(buf, 0);
        }
        return result;
    }
}

std::optional<Match> SearchEngine::find_prev(const Buffer& buf,
                                              Position from) const {
    if (!valid_ || query_.empty()) return std::nullopt;

    const size_t from_offset = pos_to_offset(buf, from);

    auto result = plain_search_backward(buf, from_offset);
    if (!result && opts_.wrap_around) {
        result = plain_search_backward(buf, buf.text().size());
    }
    return result;
}

// ---------------------------------------------------------------------------
// Plain-text helpers
// ---------------------------------------------------------------------------

std::optional<Match> SearchEngine::plain_search_forward(const Buffer& buf,
                                                          size_t start) const {
    std::string haystack = buf.text();
    std::string needle   = query_;

    if (!opts_.case_sensitive) {
        std::transform(haystack.begin(), haystack.end(),
                       haystack.begin(), [](unsigned char c){ return std::tolower(c); });
        std::transform(needle.begin(), needle.end(),
                       needle.begin(), [](unsigned char c){ return std::tolower(c); });
    }

    if (start > haystack.size()) start = 0;
    const size_t pos = haystack.find(needle, start);
    if (pos == std::string::npos) return std::nullopt;
    if (opts_.whole_word && !matches_word_boundary(haystack, pos, needle.size()))
        return std::nullopt;

    const size_t e = pos + needle.size();
    return Match{offset_to_pos(buf, pos), offset_to_pos(buf, e)};
}

std::optional<Match> SearchEngine::plain_search_backward(const Buffer& buf,
                                                           size_t start) const {
    std::string haystack = buf.text();
    std::string needle   = query_;

    if (!opts_.case_sensitive) {
        std::transform(haystack.begin(), haystack.end(),
                       haystack.begin(), [](unsigned char c){ return std::tolower(c); });
        std::transform(needle.begin(), needle.end(),
                       needle.begin(), [](unsigned char c){ return std::tolower(c); });
    }

    if (start > haystack.size()) start = haystack.size();
    if (start < needle.size()) return std::nullopt;

    const size_t search_end = start - needle.size();
    size_t pos = haystack.rfind(needle, search_end);
    if (pos == std::string::npos) return std::nullopt;
    if (opts_.whole_word && !matches_word_boundary(haystack, pos, needle.size()))
        return std::nullopt;

    const size_t e = pos + needle.size();
    return Match{offset_to_pos(buf, pos), offset_to_pos(buf, e)};
}

std::optional<Match> SearchEngine::regex_search_forward(const Buffer& buf,
                                                          size_t start) const {
    const std::string full = buf.text();
    if (start > full.size()) start = 0;

    const std::string sub = full.substr(start);
    std::smatch m;
    if (!std::regex_search(sub, m, regex_)) return std::nullopt;

    const size_t s = start + static_cast<size_t>(m.position(0));
    const size_t e = s + static_cast<size_t>(m.length(0));
    if (opts_.whole_word && !matches_word_boundary(full, s, e - s))
        return std::nullopt;

    return Match{offset_to_pos(buf, s), offset_to_pos(buf, e)};
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

Position SearchEngine::replace_one(Buffer& buf,
                                    const Match& m,
                                    std::string_view replacement) const {
    // Build replacement string (handle regex backreferences if needed)
    std::string rep_str;
    if (opts_.use_regex) {
        // Get matched text for backreference expansion
        const std::string full = buf.text();
        const size_t s = pos_to_offset(buf, m.start);
        const size_t e = pos_to_offset(buf, m.end);
        const std::string matched = full.substr(s, e - s);
        std::smatch sm;
        if (std::regex_match(matched, sm, regex_)) {
            rep_str = sm.format(std::string(replacement));
        } else {
            rep_str = std::string(replacement);
        }
    } else {
        rep_str = std::string(replacement);
    }

    buf.erase(m.start, m.end);
    buf.insert(m.start, rep_str);

    // Return position after replacement
    Position after = m.start;
    after.col += static_cast<int>(rep_str.size());
    return after;
}

int SearchEngine::replace_all(Buffer& buf, std::string_view replacement) const {
    auto matches = find_all(buf);
    if (matches.empty()) return 0;

    // Replace in reverse order to preserve earlier positions
    std::reverse(matches.begin(), matches.end());
    for (const auto& m : matches) {
        replace_one(buf, m, replacement);
    }

    return static_cast<int>(matches.size());
}

} // namespace straylight::editor
