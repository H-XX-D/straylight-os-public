// apps/media_library/catalog.h
// SQLite-backed media catalog with tag/album/artist indexing and full-text search.
#pragma once

#include "scanner.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <filesystem>
#include <string>
#include <vector>

// Forward-declare sqlite3 to avoid pulling the full header here.
struct sqlite3;

namespace straylight::media {

/// Query filters for searching the catalog.
struct CatalogQuery {
    std::string text;       ///< FTS match against title/artist/album/path
    MediaType   type        = MediaType::Unknown; ///< filter by type (Unknown = all)
    std::string artist;
    std::string album;
    std::string genre;
    int         year_min    = 0;
    int         year_max    = 0;
    int         limit       = 200;
    int         offset      = 0;
};

/// Thin wrapper that represents a row returned by search queries.
using CatalogRow = MediaEntry;

/// Manages an SQLite database containing indexed media entries.
class Catalog {
public:
    Catalog() = default;
    ~Catalog();

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    /// Open (or create) the catalog database at `path`.
    Result<void, SLError> open(const fs::path& path);

    /// Close the database.
    void close();

    /// Insert or update a media entry (upsert by canonical path).
    Result<void, SLError> upsert(const MediaEntry& e);

    /// Remove an entry by path.
    Result<void, SLError> remove(const fs::path& path);

    /// Remove all entries whose source path no longer exists on disk.
    Result<size_t, SLError> prune();

    /// Search the catalog according to the given query.
    Result<std::vector<CatalogRow>, SLError> search(const CatalogQuery& q) const;

    /// Return all distinct artist names.
    Result<std::vector<std::string>, SLError> artists() const;

    /// Return all distinct album names (optionally filtered by artist).
    Result<std::vector<std::string>, SLError> albums(const std::string& artist = {}) const;

    /// Return all distinct genres.
    Result<std::vector<std::string>, SLError> genres() const;

    /// Return total entry count.
    Result<size_t, SLError> count() const;

    /// Begin / commit / rollback a transaction (for bulk inserts).
    Result<void, SLError> begin();
    Result<void, SLError> commit();
    Result<void, SLError> rollback();

private:
    sqlite3* db_ = nullptr;

    /// Execute a plain SQL statement (no bindings).
    Result<void, SLError> exec_sql(const char* sql) const;

    /// Create table schema and FTS virtual table if they don't exist.
    Result<void, SLError> init_schema();

    /// Populate a MediaEntry from a prepared statement that is in "row ready" state.
    static CatalogRow row_to_entry(struct sqlite3_stmt* stmt);
};

} // namespace straylight::media
