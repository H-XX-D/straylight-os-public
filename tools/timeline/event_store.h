// tools/timeline/event_store.h
// SQLite-backed event store for the StrayLight timeline.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// A single timeline event.
struct TimelineEvent {
    int64_t id = 0;
    std::string category;   // "login", "package", "file", "service", "command", "app", "git"
    std::string action;      // "login", "logout", "install", "remove", "modify", etc.
    std::string subject;     // username, package name, file path, etc.
    std::string detail;      // Additional detail text.
    std::chrono::system_clock::time_point timestamp;
};

/// Manages the SQLite timeline database.
class EventStore {
public:
    EventStore();
    ~EventStore();

    /// Open or create the database at the given path.
    Result<void, std::string> open(const std::string& db_path);

    /// Insert an event.
    Result<void, std::string> insert(const TimelineEvent& event);

    /// Insert a batch of events.
    Result<int, std::string> insert_batch(const std::vector<TimelineEvent>& events);

    /// Query events in a time range, optionally filtered by category or pattern.
    Result<std::vector<TimelineEvent>, std::string>
    query(std::chrono::system_clock::time_point from,
          std::chrono::system_clock::time_point to,
          const std::string& category = "",
          const std::string& pattern = "") const;

    /// Get all events from today.
    Result<std::vector<TimelineEvent>, std::string> today() const;

    /// Search events matching a pattern across all fields.
    Result<std::vector<TimelineEvent>, std::string>
    search(const std::string& pattern,
           std::chrono::system_clock::time_point from = {},
           std::chrono::system_clock::time_point to = {}) const;

    /// Purge events older than N days.
    Result<int, std::string> purge(int days);

    /// Get total event count.
    int64_t count() const;

    /// Export all events to a JSON string.
    Result<std::string, std::string> export_json() const;

    /// Close the database.
    void close();

private:
    // We use a raw C-style SQLite interface via popen("sqlite3 ...") to avoid
    // linking libsqlite3 directly. The database is a standard SQLite3 file.
    std::string db_path_;
    bool opened_ = false;

    Result<std::string, std::string> sql_run(const std::string& sql) const;
    Result<std::vector<std::vector<std::string>>, std::string>
    sql_query(const std::string& sql) const;
    std::string escape(const std::string& s) const;
    int64_t to_epoch(std::chrono::system_clock::time_point tp) const;
    std::chrono::system_clock::time_point from_epoch(int64_t epoch) const;
};

} // namespace straylight
