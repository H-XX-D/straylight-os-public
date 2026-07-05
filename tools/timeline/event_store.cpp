// tools/timeline/event_store.cpp
// SQLite-backed event store using sqlite3 CLI for zero library dependency.

#include "event_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

EventStore::EventStore() = default;
EventStore::~EventStore() { close(); }

std::string EventStore::escape(const std::string& s) const {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

int64_t EventStore::to_epoch(std::chrono::system_clock::time_point tp) const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               tp.time_since_epoch())
               .count();
}

std::chrono::system_clock::time_point EventStore::from_epoch(int64_t epoch) const {
    return std::chrono::system_clock::from_time_t(static_cast<time_t>(epoch));
}

Result<std::string, std::string>
EventStore::sql_run(const std::string& sql) const {
    if (!opened_) {
        return Result<std::string, std::string>::error("database not opened");
    }
    // Write SQL to a temp file to avoid shell escaping issues.
    std::string tmp_path = "/tmp/straylight_timeline_sql.tmp";
    {
        std::ofstream tmp(tmp_path);
        if (!tmp.is_open()) {
            return Result<std::string, std::string>::error(
                "cannot create temp SQL file");
        }
        tmp << sql;
        tmp.close();
    }

    std::string cmd = "sqlite3 '" + db_path_ + "' < '" + tmp_path + "' 2>&1";
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error("popen failed");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    // Remove temp file.
    std::remove(tmp_path.c_str());

    if (rc != 0 && !output.empty() && output.find("Error") != std::string::npos) {
        return Result<std::string, std::string>::error("SQL error: " + output);
    }
    return Result<std::string, std::string>::ok(output);
}

Result<std::vector<std::vector<std::string>>, std::string>
EventStore::sql_query(const std::string& sql) const {
    if (!opened_) {
        return Result<std::vector<std::vector<std::string>>, std::string>::error(
            "database not opened");
    }
    // Write SQL to temp file with separator directive.
    std::string tmp_path = "/tmp/straylight_timeline_sql.tmp";
    {
        std::ofstream tmp(tmp_path);
        if (!tmp.is_open()) {
            return Result<std::vector<std::vector<std::string>>, std::string>::error(
                "cannot create temp SQL file");
        }
        tmp << ".separator '|'\n" << sql;
        tmp.close();
    }

    std::string cmd = "sqlite3 '" + db_path_ + "' < '" + tmp_path + "' 2>&1";
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::remove(tmp_path.c_str());
        return Result<std::vector<std::vector<std::string>>, std::string>::error(
            "popen failed");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    pclose(pipe);
    std::remove(tmp_path.c_str());

    std::vector<std::vector<std::string>> rows;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::istringstream ls(line);
        std::string col;
        while (std::getline(ls, col, '|')) {
            cols.push_back(col);
        }
        rows.push_back(std::move(cols));
    }

    return Result<std::vector<std::vector<std::string>>, std::string>::ok(
        std::move(rows));
}

Result<void, std::string> EventStore::open(const std::string& db_path) {
    db_path_ = db_path;

    // Ensure parent directory exists.
    std::error_code ec;
    fs::create_directories(fs::path(db_path).parent_path(), ec);

    opened_ = true;

    // Create table if not exists.
    auto res = sql_run(
        "CREATE TABLE IF NOT EXISTS events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  category TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  subject TEXT NOT NULL,"
        "  detail TEXT DEFAULT '',"
        "  timestamp INTEGER NOT NULL"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_events_ts ON events(timestamp);\n"
        "CREATE INDEX IF NOT EXISTS idx_events_cat ON events(category);\n"
        "DELETE FROM events WHERE id NOT IN ("
        "  SELECT MIN(id) FROM events "
        "  GROUP BY category, action, subject, detail, timestamp"
        ");\n"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_events_unique "
        "ON events(category, action, subject, detail, timestamp);\n");

    if (!res.has_value()) {
        opened_ = false;
        return Result<void, std::string>::error(res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> EventStore::insert(const TimelineEvent& event) {
    std::string sql =
        "INSERT OR IGNORE INTO events (category, action, subject, detail, timestamp) "
        "VALUES ('" +
        escape(event.category) + "', '" +
        escape(event.action) + "', '" +
        escape(event.subject) + "', '" +
        escape(event.detail) + "', " +
        std::to_string(to_epoch(event.timestamp)) + ");\n";

    auto res = sql_run(sql);
    if (!res.has_value()) {
        return Result<void, std::string>::error(res.error());
    }
    return Result<void, std::string>::ok();
}

Result<int, std::string>
EventStore::insert_batch(const std::vector<TimelineEvent>& events) {
    if (events.empty()) {
        return Result<int, std::string>::ok(0);
    }

    std::ostringstream sql;
    sql << "BEGIN TRANSACTION;\n";
    for (const auto& e : events) {
        sql << "INSERT OR IGNORE INTO events "
               "(category, action, subject, detail, timestamp) VALUES ('"
            << escape(e.category) << "', '"
            << escape(e.action) << "', '"
            << escape(e.subject) << "', '"
            << escape(e.detail) << "', "
            << to_epoch(e.timestamp) << ");\n";
    }
    sql << "COMMIT;\n";

    auto res = sql_run(sql.str());
    if (!res.has_value()) {
        return Result<int, std::string>::error(res.error());
    }
    return Result<int, std::string>::ok(static_cast<int>(events.size()));
}

Result<std::vector<TimelineEvent>, std::string>
EventStore::query(std::chrono::system_clock::time_point from,
                   std::chrono::system_clock::time_point to,
                   const std::string& category,
                   const std::string& pattern) const {
    std::ostringstream sql;
    sql << "SELECT id, category, action, subject, detail, timestamp "
           "FROM events WHERE 1=1";

    if (to_epoch(from) > 0) {
        sql << " AND timestamp >= " << to_epoch(from);
    }
    if (to_epoch(to) > 0) {
        sql << " AND timestamp <= " << to_epoch(to);
    }
    if (!category.empty()) {
        sql << " AND category = '" << escape(category) << "'";
    }
    if (!pattern.empty()) {
        std::string esc = escape(pattern);
        sql << " AND (subject LIKE '%" << esc << "%'"
            << " OR detail LIKE '%" << esc << "%'"
            << " OR action LIKE '%" << esc << "%')";
    }
    sql << " ORDER BY timestamp DESC LIMIT 1000;\n";

    auto res = sql_query(sql.str());
    if (!res.has_value()) {
        return Result<std::vector<TimelineEvent>, std::string>::error(res.error());
    }

    std::vector<TimelineEvent> events;
    for (const auto& row : res.value()) {
        if (row.size() < 6) continue;
        TimelineEvent e;
        e.id = std::atoll(row[0].c_str());
        e.category = row[1];
        e.action = row[2];
        e.subject = row[3];
        e.detail = row[4];
        e.timestamp = from_epoch(std::atoll(row[5].c_str()));
        events.push_back(std::move(e));
    }

    return Result<std::vector<TimelineEvent>, std::string>::ok(std::move(events));
}

Result<std::vector<TimelineEvent>, std::string> EventStore::today() const {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&now_t, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    auto start = std::chrono::system_clock::from_time_t(mktime(&tm));
    return query(start, now);
}

Result<std::vector<TimelineEvent>, std::string>
EventStore::search(const std::string& pattern,
                    std::chrono::system_clock::time_point from,
                    std::chrono::system_clock::time_point to) const {
    return query(from, to, "", pattern);
}

Result<int, std::string> EventStore::purge(int days) {
    auto cutoff = std::chrono::system_clock::now() -
                  std::chrono::hours(24 * days);
    std::string sql = "DELETE FROM events WHERE timestamp < " +
                      std::to_string(to_epoch(cutoff)) + ";\n";
    auto res = sql_run(sql);
    if (!res.has_value()) {
        return Result<int, std::string>::error(res.error());
    }

    // Get count of deleted rows.
    auto count_res = sql_query("SELECT changes();\n");
    int deleted = 0;
    if (count_res.has_value() && !count_res.value().empty() &&
        !count_res.value()[0].empty()) {
        deleted = std::atoi(count_res.value()[0][0].c_str());
    }
    return Result<int, std::string>::ok(deleted);
}

int64_t EventStore::count() const {
    auto res = sql_query("SELECT COUNT(*) FROM events;\n");
    if (res.has_value() && !res.value().empty() && !res.value()[0].empty()) {
        return std::atoll(res.value()[0][0].c_str());
    }
    return 0;
}

Result<std::string, std::string> EventStore::export_json() const {
    auto res = sql_query(
        "SELECT id, category, action, subject, detail, timestamp "
        "FROM events ORDER BY timestamp ASC;\n");
    if (!res.has_value()) {
        return Result<std::string, std::string>::error(res.error());
    }

    std::ostringstream out;
    out << "[\n";
    const auto& rows = res.value();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() < 6) continue;
        out << "  {";
        out << "\"id\": " << rows[i][0] << ", ";
        out << "\"category\": \"" << rows[i][1] << "\", ";
        out << "\"action\": \"" << rows[i][2] << "\", ";
        out << "\"subject\": \"" << rows[i][3] << "\", ";
        out << "\"detail\": \"" << rows[i][4] << "\", ";
        out << "\"timestamp\": " << rows[i][5];
        out << "}";
        if (i + 1 < rows.size()) out << ",";
        out << "\n";
    }
    out << "]\n";

    return Result<std::string, std::string>::ok(out.str());
}

void EventStore::close() {
    opened_ = false;
}

} // namespace straylight
