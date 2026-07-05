// tools/journal/journal_engine.cpp
// Full journal engine implementation for StrayLight OS.

#include "journal_engine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

static constexpr const char* JOURNAL_BASE = "/var/lib/straylight/journal";

JournalEngine::JournalEngine()
    : journal_dir_(JOURNAL_BASE)
    , index_path_(std::string(JOURNAL_BASE) + "/index") {}

JournalEngine::~JournalEngine() = default;

Result<void, std::string> JournalEngine::ensure_dirs() const {
    try {
        fs::create_directories(journal_dir_);
        fs::create_directories(journal_dir_ + "/entries");
    } catch (const std::exception& e) {
        return Result<void, std::string>::error("failed to create journal dirs: " + std::string(e.what()));
    }
    return Result<void, std::string>::ok();
}

std::string JournalEngine::entry_path(uint64_t id) const {
    return journal_dir_ + "/entries/" + std::to_string(id) + ".journal";
}

std::string JournalEngine::now_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

uint64_t JournalEngine::next_id() const {
    uint64_t max_id = 0;
    try {
        for (const auto& entry : fs::directory_iterator(journal_dir_ + "/entries")) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().stem().string();
            try {
                uint64_t id = std::stoull(name);
                if (id > max_id) max_id = id;
            } catch (...) {}
        }
    } catch (...) {}
    return max_id + 1;
}

Result<void, std::string> JournalEngine::save_entry(const JournalEntry& entry) const {
    auto dir_res = ensure_dirs();
    if (!dir_res.has_value()) return dir_res;

    std::string path = entry_path(entry.id);
    std::ofstream f(path);
    if (!f.is_open())
        return Result<void, std::string>::error("cannot write entry: " + path);

    f << "id: " << entry.id << "\n"
      << "timestamp: " << entry.timestamp << "\n"
      << "title: " << entry.title << "\n"
      << "project: " << entry.project << "\n"
      << "pinned: " << (entry.pinned ? "true" : "false") << "\n"
      << "tags:";
    for (const auto& t : entry.tags) f << " " << t;
    f << "\n"
      << "---\n"
      << entry.body << "\n";

    if (f.fail())
        return Result<void, std::string>::error("write failed: " + path);
    return Result<void, std::string>::ok();
}

Result<JournalEntry, std::string> JournalEngine::load_entry(const std::string& path) const {
    std::ifstream f(path);
    if (!f.is_open())
        return Result<JournalEntry, std::string>::error("cannot read: " + path);

    JournalEntry entry;
    std::string line;
    bool in_body = false;

    while (std::getline(f, line)) {
        if (in_body) {
            if (!entry.body.empty()) entry.body += "\n";
            entry.body += line;
            continue;
        }
        if (line == "---") {
            in_body = true;
            continue;
        }
        auto colon = line.find(": ");
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 2);

        if (key == "id") { try { entry.id = std::stoull(val); } catch (...) {} }
        else if (key == "timestamp") entry.timestamp = val;
        else if (key == "title") entry.title = val;
        else if (key == "project") entry.project = val;
        else if (key == "pinned") entry.pinned = (val == "true");
        else if (key == "tags") {
            std::istringstream ss(val);
            std::string tag;
            while (ss >> tag) entry.tags.push_back(tag);
        }
    }
    return Result<JournalEntry, std::string>::ok(entry);
}

Result<std::vector<JournalEntry>, std::string> JournalEngine::load_all() const {
    std::vector<JournalEntry> entries;
    std::string entries_dir = journal_dir_ + "/entries";
    if (!fs::exists(entries_dir))
        return Result<std::vector<JournalEntry>, std::string>::ok(entries);

    try {
        for (const auto& file : fs::directory_iterator(entries_dir)) {
            if (!file.is_regular_file()) continue;
            if (file.path().extension() != ".journal") continue;
            auto res = load_entry(file.path().string());
            if (res.has_value()) entries.push_back(res.value());
        }
    } catch (const std::exception& e) {
        return Result<std::vector<JournalEntry>, std::string>::error(
            "failed to read entries: " + std::string(e.what()));
    }

    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.id > b.id; });
    return Result<std::vector<JournalEntry>, std::string>::ok(entries);
}

bool JournalEngine::matches_filter(const JournalEntry& entry,
                                    const JournalFilter& filter) const {
    if (filter.pinned_only && !entry.pinned) return false;

    if (!filter.tag.empty()) {
        bool found = false;
        for (const auto& t : entry.tags) {
            if (t == filter.tag) { found = true; break; }
        }
        if (!found) return false;
    }

    if (!filter.project.empty() && entry.project != filter.project) return false;

    if (!filter.since.empty() && entry.timestamp < filter.since) return false;
    if (!filter.until.empty() && entry.timestamp > filter.until) return false;

    if (!filter.query.empty()) {
        std::string q = filter.query;
        std::string title = entry.title;
        std::string body = entry.body;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);
        std::transform(body.begin(), body.end(), body.begin(), ::tolower);
        if (title.find(q) == std::string::npos &&
            body.find(q) == std::string::npos) return false;
    }
    return true;
}

Result<JournalEntry, std::string> JournalEngine::add(const std::string& title,
                                                      const std::string& body,
                                                      const std::vector<std::string>& tags,
                                                      const std::string& project) {
    auto dir_res = ensure_dirs();
    if (!dir_res.has_value())
        return Result<JournalEntry, std::string>::error(dir_res.error());

    JournalEntry entry;
    entry.id = next_id();
    entry.timestamp = now_timestamp();
    entry.title = title;
    entry.body = body;
    entry.tags = tags;
    entry.project = project;

    auto save_res = save_entry(entry);
    if (!save_res.has_value())
        return Result<JournalEntry, std::string>::error(save_res.error());

    return Result<JournalEntry, std::string>::ok(entry);
}

Result<JournalEntry, std::string> JournalEngine::get(uint64_t id) const {
    std::string path = entry_path(id);
    if (!fs::exists(path))
        return Result<JournalEntry, std::string>::error("entry not found: " + std::to_string(id));
    return load_entry(path);
}

Result<std::vector<JournalEntry>, std::string> JournalEngine::list(
    const JournalFilter& filter) const {
    auto all_res = load_all();
    if (!all_res.has_value())
        return all_res;

    std::vector<JournalEntry> filtered;
    for (const auto& e : all_res.value()) {
        if (matches_filter(e, filter)) {
            filtered.push_back(e);
            if (filter.limit > 0 && static_cast<int>(filtered.size()) >= filter.limit)
                break;
        }
    }
    return Result<std::vector<JournalEntry>, std::string>::ok(filtered);
}

Result<void, std::string> JournalEngine::remove(uint64_t id) {
    std::string path = entry_path(id);
    if (!fs::exists(path))
        return Result<void, std::string>::error("entry not found: " + std::to_string(id));
    try { fs::remove(path); }
    catch (const std::exception& e) {
        return Result<void, std::string>::error("failed to remove: " + std::string(e.what()));
    }
    return Result<void, std::string>::ok();
}

Result<JournalEntry, std::string> JournalEngine::edit(uint64_t id,
                                                       const std::string& title,
                                                       const std::string& body,
                                                       const std::vector<std::string>& tags,
                                                       const std::string& project) {
    auto entry_res = get(id);
    if (!entry_res.has_value())
        return entry_res;

    JournalEntry entry = entry_res.value();
    if (!title.empty()) entry.title = title;
    if (!body.empty()) entry.body = body;
    if (!tags.empty()) entry.tags = tags;
    if (!project.empty()) entry.project = project;

    auto save_res = save_entry(entry);
    if (!save_res.has_value())
        return Result<JournalEntry, std::string>::error(save_res.error());

    return Result<JournalEntry, std::string>::ok(entry);
}

Result<void, std::string> JournalEngine::pin(uint64_t id, bool pinned) {
    auto entry_res = get(id);
    if (!entry_res.has_value())
        return Result<void, std::string>::error(entry_res.error());

    JournalEntry entry = entry_res.value();
    entry.pinned = pinned;
    return save_entry(entry);
}

Result<std::vector<std::string>, std::string> JournalEngine::tags() const {
    auto all_res = load_all();
    if (!all_res.has_value())
        return Result<std::vector<std::string>, std::string>::error(all_res.error());

    std::set<std::string> tag_set;
    for (const auto& e : all_res.value())
        for (const auto& t : e.tags) tag_set.insert(t);

    std::vector<std::string> result(tag_set.begin(), tag_set.end());
    return Result<std::vector<std::string>, std::string>::ok(result);
}

Result<std::vector<std::string>, std::string> JournalEngine::projects() const {
    auto all_res = load_all();
    if (!all_res.has_value())
        return Result<std::vector<std::string>, std::string>::error(all_res.error());

    std::set<std::string> proj_set;
    for (const auto& e : all_res.value())
        if (!e.project.empty()) proj_set.insert(e.project);

    std::vector<std::string> result(proj_set.begin(), proj_set.end());
    return Result<std::vector<std::string>, std::string>::ok(result);
}

Result<JournalStats, std::string> JournalEngine::stats() const {
    auto all_res = load_all();
    if (!all_res.has_value())
        return Result<JournalStats, std::string>::error(all_res.error());

    const auto& entries = all_res.value();
    JournalStats s;
    s.total_entries = static_cast<int>(entries.size());

    std::map<std::string, int> tag_counts;
    std::map<std::string, int> proj_counts;
    std::string now = now_timestamp();
    std::string week_ago, month_ago;

    // Compute week_ago and month_ago
    auto tp = std::chrono::system_clock::now();
    {
        auto wa = tp - std::chrono::hours(7 * 24);
        auto wa_t = std::chrono::system_clock::to_time_t(wa);
        std::tm tm{}; localtime_r(&wa_t, &tm);
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        week_ago = buf;
    }
    {
        auto ma = tp - std::chrono::hours(30 * 24);
        auto ma_t = std::chrono::system_clock::to_time_t(ma);
        std::tm tm{}; localtime_r(&ma_t, &tm);
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        month_ago = buf;
    }

    for (const auto& e : entries) {
        for (const auto& t : e.tags) tag_counts[t]++;
        if (!e.project.empty()) proj_counts[e.project]++;
        if (e.timestamp >= week_ago) s.entries_this_week++;
        if (e.timestamp >= month_ago) s.entries_this_month++;
    }

    if (!entries.empty()) {
        s.newest_entry = entries.front().timestamp;
        s.oldest_entry = entries.back().timestamp;
    }

    s.total_tags = static_cast<int>(tag_counts.size());
    s.total_projects = static_cast<int>(proj_counts.size());

    // Top tags
    std::vector<std::pair<std::string, int>> tv(tag_counts.begin(), tag_counts.end());
    std::sort(tv.begin(), tv.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min(tv.size(), size_t(10)); ++i)
        s.top_tags.push_back(tv[i]);

    // Top projects
    std::vector<std::pair<std::string, int>> pv(proj_counts.begin(), proj_counts.end());
    std::sort(pv.begin(), pv.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min(pv.size(), size_t(10)); ++i)
        s.top_projects.push_back(pv[i]);

    return Result<JournalStats, std::string>::ok(s);
}

Result<std::string, std::string> JournalEngine::export_markdown(
    const JournalFilter& filter) const {
    auto list_res = list(filter);
    if (!list_res.has_value())
        return Result<std::string, std::string>::error(list_res.error());

    std::ostringstream md;
    md << "# Developer Journal Export\n\n";
    md << "Generated: " << now_timestamp() << "\n\n";

    for (const auto& e : list_res.value()) {
        md << "## " << e.title;
        if (e.pinned) md << " [pinned]";
        md << "\n\n";
        md << "**Date:** " << e.timestamp << "  \n";
        if (!e.project.empty()) md << "**Project:** " << e.project << "  \n";
        if (!e.tags.empty()) {
            md << "**Tags:**";
            for (const auto& t : e.tags) md << " `" << t << "`";
            md << "  \n";
        }
        md << "\n" << e.body << "\n\n---\n\n";
    }

    return Result<std::string, std::string>::ok(md.str());
}

} // namespace straylight
