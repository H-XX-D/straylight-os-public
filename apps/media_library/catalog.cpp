// apps/media_library/catalog.cpp
// SQLite-backed media catalog implementation
#include "catalog.h"

#include <straylight/log.h>

#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <sstream>

namespace straylight::media {

namespace {

inline SLError make_err(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}

int64_t tp_to_epoch(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point epoch_to_tp(int64_t s) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{s}};
}

std::string type_to_str(MediaType t) {
    switch (t) {
        case MediaType::Image: return "image";
        case MediaType::Audio: return "audio";
        case MediaType::Video: return "video";
        default:               return "unknown";
    }
}

MediaType str_to_type(const char* s) {
    if (!s) return MediaType::Unknown;
    if (std::strcmp(s, "image") == 0) return MediaType::Image;
    if (std::strcmp(s, "audio") == 0) return MediaType::Audio;
    if (std::strcmp(s, "video") == 0) return MediaType::Video;
    return MediaType::Unknown;
}

/// RAII wrapper around sqlite3_stmt.
struct Stmt {
    sqlite3_stmt* p = nullptr;
    ~Stmt() { if (p) sqlite3_finalize(p); }
};

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Catalog::~Catalog() { close(); }

void Catalog::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Result<void, SLError> Catalog::exec_sql(const char* sql) const {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal, "SQLite: " + msg));
    }
    return Result<void, SLError>::ok();
}

Result<void, SLError> Catalog::init_schema() {
    const char* create_media = R"sql(
        CREATE TABLE IF NOT EXISTS media (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            path        TEXT    NOT NULL UNIQUE,
            type        TEXT    NOT NULL DEFAULT 'unknown',
            mime        TEXT    NOT NULL DEFAULT '',
            title       TEXT    NOT NULL DEFAULT '',
            artist      TEXT    NOT NULL DEFAULT '',
            album       TEXT    NOT NULL DEFAULT '',
            genre       TEXT    NOT NULL DEFAULT '',
            year        INTEGER NOT NULL DEFAULT 0,
            track       INTEGER NOT NULL DEFAULT 0,
            duration_ms INTEGER NOT NULL DEFAULT 0,
            width       INTEGER NOT NULL DEFAULT 0,
            height      INTEGER NOT NULL DEFAULT 0,
            file_size   INTEGER NOT NULL DEFAULT 0,
            mtime       INTEGER NOT NULL DEFAULT 0
        );
    )sql";

    const char* idx_artist = "CREATE INDEX IF NOT EXISTS idx_artist ON media(artist);";
    const char* idx_album  = "CREATE INDEX IF NOT EXISTS idx_album  ON media(album);";
    const char* idx_genre  = "CREATE INDEX IF NOT EXISTS idx_genre  ON media(genre);";
    const char* idx_type   = "CREATE INDEX IF NOT EXISTS idx_type   ON media(type);";

    const char* create_fts = R"sql(
        CREATE VIRTUAL TABLE IF NOT EXISTS media_fts USING fts5(
            path,
            title,
            artist,
            album,
            content='media',
            content_rowid='id'
        );
    )sql";

    const char* trg_insert = R"sql(
        CREATE TRIGGER IF NOT EXISTS media_ai AFTER INSERT ON media BEGIN
            INSERT INTO media_fts(rowid, path, title, artist, album)
            VALUES (new.id, new.path, new.title, new.artist, new.album);
        END;
    )sql";
    const char* trg_delete = R"sql(
        CREATE TRIGGER IF NOT EXISTS media_ad AFTER DELETE ON media BEGIN
            INSERT INTO media_fts(media_fts, rowid, path, title, artist, album)
            VALUES ('delete', old.id, old.path, old.title, old.artist, old.album);
        END;
    )sql";
    const char* trg_update = R"sql(
        CREATE TRIGGER IF NOT EXISTS media_au AFTER UPDATE ON media BEGIN
            INSERT INTO media_fts(media_fts, rowid, path, title, artist, album)
            VALUES ('delete', old.id, old.path, old.title, old.artist, old.album);
            INSERT INTO media_fts(rowid, path, title, artist, album)
            VALUES (new.id, new.path, new.title, new.artist, new.album);
        END;
    )sql";

    for (const char* sql : {create_media, idx_artist, idx_album, idx_genre,
                            idx_type, create_fts, trg_insert, trg_delete, trg_update}) {
        auto r = exec_sql(sql);
        if (!r.has_value()) return r;
    }
    return Result<void, SLError>::ok();
}

Result<void, SLError> Catalog::open(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot open catalog: " + msg));
    }

    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;",  nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=-8192;",   nullptr, nullptr, nullptr);

    return init_schema();
}

// ---------------------------------------------------------------------------
// Transaction control
// ---------------------------------------------------------------------------

Result<void, SLError> Catalog::begin()    { return exec_sql("BEGIN;"); }
Result<void, SLError> Catalog::commit()   { return exec_sql("COMMIT;"); }
Result<void, SLError> Catalog::rollback() { return exec_sql("ROLLBACK;"); }

// ---------------------------------------------------------------------------
// upsert()
// ---------------------------------------------------------------------------

Result<void, SLError> Catalog::upsert(const MediaEntry& e) {
    const char* sql = R"sql(
        INSERT INTO media (path, type, mime, title, artist, album, genre,
                           year, track, duration_ms, width, height, file_size, mtime)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(path) DO UPDATE SET
            type        = excluded.type,
            mime        = excluded.mime,
            title       = excluded.title,
            artist      = excluded.artist,
            album       = excluded.album,
            genre       = excluded.genre,
            year        = excluded.year,
            track       = excluded.track,
            duration_ms = excluded.duration_ms,
            width       = excluded.width,
            height      = excluded.height,
            file_size   = excluded.file_size,
            mtime       = excluded.mtime;
    )sql";

    Stmt st;
    if (sqlite3_prepare_v2(db_, sql, -1, &st.p, nullptr) != SQLITE_OK) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal,
                     std::string("prepare upsert: ") + sqlite3_errmsg(db_)));
    }

    const std::string path_s = e.path.string();
    const std::string type_s = type_to_str(e.type);
    int64_t mtime_i = tp_to_epoch(e.mtime);

    sqlite3_bind_text(st.p,  1, path_s.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.p,  2, type_s.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.p,  3, e.mime.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.p,  4, e.title.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.p,  5, e.artist.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.p,  6, e.album.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.p,  7, e.genre.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st.p,  8, e.year);
    sqlite3_bind_int (st.p,  9, e.track);
    sqlite3_bind_int64(st.p, 10, static_cast<int64_t>(e.duration_ms));
    sqlite3_bind_int (st.p, 11, static_cast<int>(e.width));
    sqlite3_bind_int (st.p, 12, static_cast<int>(e.height));
    sqlite3_bind_int64(st.p, 13, static_cast<int64_t>(e.file_size));
    sqlite3_bind_int64(st.p, 14, mtime_i);

    int rc = sqlite3_step(st.p);
    if (rc != SQLITE_DONE) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal,
                     std::string("upsert step: ") + sqlite3_errmsg(db_)));
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// remove()
// ---------------------------------------------------------------------------

Result<void, SLError> Catalog::remove(const fs::path& path) {
    const char* sql = "DELETE FROM media WHERE path = ?;";
    Stmt st;
    if (sqlite3_prepare_v2(db_, sql, -1, &st.p, nullptr) != SQLITE_OK) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal, sqlite3_errmsg(db_)));
    }
    const std::string s = path.string();
    sqlite3_bind_text(st.p, 1, s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st.p);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// prune()
// ---------------------------------------------------------------------------

Result<size_t, SLError> Catalog::prune() {
    const char* sel = "SELECT path FROM media;";
    Stmt st;
    if (sqlite3_prepare_v2(db_, sel, -1, &st.p, nullptr) != SQLITE_OK) {
        return Result<size_t, SLError>::error(
            make_err(SLErrorCode::Internal, sqlite3_errmsg(db_)));
    }

    std::vector<std::string> to_remove;
    while (sqlite3_step(st.p) == SQLITE_ROW) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        if (p && !fs::exists(fs::path(p))) {
            to_remove.emplace_back(p);
        }
    }

    for (const auto& p : to_remove) {
        remove(fs::path(p));
    }
    return Result<size_t, SLError>::ok(to_remove.size());
}

// ---------------------------------------------------------------------------
// row_to_entry() - static helper
// ---------------------------------------------------------------------------

CatalogRow Catalog::row_to_entry(sqlite3_stmt* stmt) {
    CatalogRow e;
    auto col_text = [&](int i) -> std::string {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return v ? v : "";
    };
    e.path        = col_text(0);
    e.type        = str_to_type(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    e.mime        = col_text(2);
    e.title       = col_text(3);
    e.artist      = col_text(4);
    e.album       = col_text(5);
    e.genre       = col_text(6);
    e.year        = sqlite3_column_int(stmt, 7);
    e.track       = sqlite3_column_int(stmt, 8);
    e.duration_ms = static_cast<uint64_t>(sqlite3_column_int64(stmt, 9));
    e.width       = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
    e.height      = static_cast<uint32_t>(sqlite3_column_int(stmt, 11));
    e.file_size   = static_cast<uint64_t>(sqlite3_column_int64(stmt, 12));
    e.mtime       = epoch_to_tp(sqlite3_column_int64(stmt, 13));
    return e;
}

// ---------------------------------------------------------------------------
// search()
// ---------------------------------------------------------------------------

Result<std::vector<CatalogRow>, SLError> Catalog::search(const CatalogQuery& q) const {
    std::string sql;
    std::vector<std::string> binds;

    if (!q.text.empty()) {
        sql = R"sql(
            SELECT m.path, m.type, m.mime, m.title, m.artist, m.album, m.genre,
                   m.year, m.track, m.duration_ms, m.width, m.height, m.file_size, m.mtime
            FROM media m
            JOIN media_fts f ON m.id = f.rowid
            WHERE media_fts MATCH ?
        )sql";
        binds.push_back(q.text + "*");
    } else {
        sql = R"sql(
            SELECT path, type, mime, title, artist, album, genre,
                   year, track, duration_ms, width, height, file_size, mtime
            FROM media WHERE 1=1
        )sql";
    }

    if (q.type != MediaType::Unknown) {
        sql += " AND type = ?";
        binds.push_back(type_to_str(q.type));
    }
    if (!q.artist.empty()) { sql += " AND artist = ?"; binds.push_back(q.artist); }
    if (!q.album.empty())  { sql += " AND album = ?";  binds.push_back(q.album);  }
    if (!q.genre.empty())  { sql += " AND genre = ?";  binds.push_back(q.genre);  }
    if (q.year_min > 0) {
        sql += " AND year >= ?";
        binds.push_back(std::to_string(q.year_min));
    }
    if (q.year_max > 0) {
        sql += " AND year <= ?";
        binds.push_back(std::to_string(q.year_max));
    }
    sql += " LIMIT ? OFFSET ?;";

    Stmt st;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.p, nullptr) != SQLITE_OK) {
        return Result<std::vector<CatalogRow>, SLError>::error(
            make_err(SLErrorCode::Internal,
                     std::string("prepare search: ") + sqlite3_errmsg(db_)));
    }

    int bind_idx = 1;
    for (const auto& b : binds) {
        sqlite3_bind_text(st.p, bind_idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st.p, bind_idx++, q.limit);
    sqlite3_bind_int(st.p, bind_idx,   q.offset);

    std::vector<CatalogRow> rows;
    while (sqlite3_step(st.p) == SQLITE_ROW) {
        rows.push_back(row_to_entry(st.p));
    }
    return Result<std::vector<CatalogRow>, SLError>::ok(std::move(rows));
}

// ---------------------------------------------------------------------------
// Distinct string column helpers
// ---------------------------------------------------------------------------

static Result<std::vector<std::string>, SLError>
distinct_column(sqlite3* db, const char* col,
                const std::string& filter_col = {},
                const std::string& filter_val = {}) {
    std::string sql = std::string("SELECT DISTINCT ") + col +
                      " FROM media WHERE " + col + " != ''";
    if (!filter_col.empty() && !filter_val.empty()) {
        sql += " AND " + filter_col + " = ?";
    }
    sql += " ORDER BY " + std::string(col) + " COLLATE NOCASE;";

    Stmt st;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st.p, nullptr) != SQLITE_OK) {
        return Result<std::vector<std::string>, SLError>::error(
            SLError{SLErrorCode::Internal, sqlite3_errmsg(db)});
    }
    if (!filter_val.empty()) {
        sqlite3_bind_text(st.p, 1, filter_val.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<std::string> out;
    while (sqlite3_step(st.p) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st.p, 0));
        if (v) out.emplace_back(v);
    }
    return Result<std::vector<std::string>, SLError>::ok(std::move(out));
}

Result<std::vector<std::string>, SLError> Catalog::artists() const {
    return distinct_column(db_, "artist");
}

Result<std::vector<std::string>, SLError> Catalog::albums(const std::string& artist) const {
    return distinct_column(db_, "album", artist.empty() ? "" : "artist", artist);
}

Result<std::vector<std::string>, SLError> Catalog::genres() const {
    return distinct_column(db_, "genre");
}

Result<size_t, SLError> Catalog::count() const {
    Stmt st;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM media;", -1, &st.p, nullptr);
    if (sqlite3_step(st.p) == SQLITE_ROW) {
        return Result<size_t, SLError>::ok(
            static_cast<size_t>(sqlite3_column_int64(st.p, 0)));
    }
    return Result<size_t, SLError>::ok(0);
}

} // namespace straylight::media
