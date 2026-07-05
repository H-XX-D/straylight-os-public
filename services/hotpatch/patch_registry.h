/**
 * StrayLight Hotpatch — Patch Registry
 *
 * Tracks every patch applied to the system in a JSON registry file.
 * No SQLite dependency — just atomic JSON read/write to a flat file.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace straylight::hotpatch {

enum class PatchType { Kernel, Daemon, Config };
enum class PatchStatus { Applied, RolledBack, Failed };

inline std::string patch_type_str(PatchType t) {
    switch (t) {
        case PatchType::Kernel: return "kernel";
        case PatchType::Daemon: return "daemon";
        case PatchType::Config: return "config";
    }
    return "unknown";
}

inline PatchType patch_type_from_str(const std::string& s) {
    if (s == "kernel") return PatchType::Kernel;
    if (s == "daemon") return PatchType::Daemon;
    return PatchType::Config;
}

inline std::string patch_status_str(PatchStatus s) {
    switch (s) {
        case PatchStatus::Applied:    return "applied";
        case PatchStatus::RolledBack: return "rolled_back";
        case PatchStatus::Failed:     return "failed";
    }
    return "unknown";
}

inline PatchStatus patch_status_from_str(const std::string& s) {
    if (s == "applied")     return PatchStatus::Applied;
    if (s == "rolled_back") return PatchStatus::RolledBack;
    return PatchStatus::Failed;
}

struct PatchRecord {
    std::string patch_id;
    PatchType   type{PatchType::Config};
    std::string target;            // module name, service name, or config path
    std::string patch_source;      // path to patch file or inline diff
    std::string applied_at;        // ISO 8601 timestamp
    PatchStatus status{PatchStatus::Applied};
    std::string rollback_data;     // data needed to undo the patch
    std::string description;
};

/**
 * Minimal JSON serializer/deserializer for the patch registry.
 * Avoids nlohmann dependency — the format is simple enough for hand-rolled IO.
 */
class PatchRegistry {
public:
    static constexpr const char* DEFAULT_PATH =
        "/var/lib/straylight/hotpatch/registry.json";

    explicit PatchRegistry(std::string path = DEFAULT_PATH)
        : path_(std::move(path)) {}

    /** Load registry from disk. Creates the file if it doesn't exist. */
    VoidResult<> load() {
        std::lock_guard<std::mutex> lock(mu_);
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(path_).parent_path(), ec);

        if (!fs::exists(path_, ec)) {
            records_.clear();
            return save_locked();
        }

        std::ifstream in(path_);
        if (!in) return VoidResult<>::error("cannot open " + path_);

        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        return parse(content);
    }

    /** Add a new record and persist. */
    VoidResult<> add(PatchRecord rec) {
        std::lock_guard<std::mutex> lock(mu_);
        records_.push_back(std::move(rec));
        return save_locked();
    }

    /** Update status of an existing patch. */
    VoidResult<> update_status(const std::string& patch_id, PatchStatus status) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = find_locked(patch_id);
        if (it == records_.end())
            return VoidResult<>::error("patch not found: " + patch_id);
        it->status = status;
        return save_locked();
    }

    /** Look up a patch record by ID. */
    Result<PatchRecord, std::string> get(const std::string& patch_id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = find_locked(patch_id);
        if (it == records_.end())
            return Result<PatchRecord, std::string>::error(
                "patch not found: " + patch_id);
        return Result<PatchRecord, std::string>::ok(*it);
    }

    /** Return all records. */
    std::vector<PatchRecord> all() const {
        std::lock_guard<std::mutex> lock(mu_);
        return records_;
    }

    /** Generate a unique patch ID. */
    static std::string generate_id() {
        static std::mt19937_64 rng(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::uniform_int_distribution<uint64_t> dist;
        std::ostringstream ss;
        ss << "hp-" << std::hex << dist(rng);
        return ss.str();
    }

    /** Current ISO 8601 timestamp. */
    static std::string now_iso8601() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

private:
    std::string path_;
    mutable std::mutex mu_;
    std::vector<PatchRecord> records_;

    using Iter = std::vector<PatchRecord>::iterator;
    using CIter = std::vector<PatchRecord>::const_iterator;

    Iter find_locked(const std::string& id) {
        return std::find_if(records_.begin(), records_.end(),
            [&](const PatchRecord& r) { return r.patch_id == id; });
    }

    CIter find_locked(const std::string& id) const {
        return std::find_if(records_.cbegin(), records_.cend(),
            [&](const PatchRecord& r) { return r.patch_id == id; });
    }

    // ── Minimal JSON serialization ──────────────────────────────────

    static std::string escape_json(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;
            }
        }
        return out;
    }

    static std::string unescape_json(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[i + 1]) {
                    case '"':  out += '"';  ++i; break;
                    case '\\': out += '\\'; ++i; break;
                    case 'n':  out += '\n'; ++i; break;
                    case 'r':  out += '\r'; ++i; break;
                    case 't':  out += '\t'; ++i; break;
                    default:   out += s[i];
                }
            } else {
                out += s[i];
            }
        }
        return out;
    }

    VoidResult<> save_locked() {
        std::ofstream out(path_, std::ios::trunc);
        if (!out) return VoidResult<>::error("cannot write " + path_);

        out << "[\n";
        for (size_t i = 0; i < records_.size(); ++i) {
            const auto& r = records_[i];
            out << "  {\n"
                << "    \"patch_id\": \"" << escape_json(r.patch_id) << "\",\n"
                << "    \"type\": \"" << patch_type_str(r.type) << "\",\n"
                << "    \"target\": \"" << escape_json(r.target) << "\",\n"
                << "    \"patch_source\": \"" << escape_json(r.patch_source) << "\",\n"
                << "    \"applied_at\": \"" << escape_json(r.applied_at) << "\",\n"
                << "    \"status\": \"" << patch_status_str(r.status) << "\",\n"
                << "    \"rollback_data\": \"" << escape_json(r.rollback_data) << "\",\n"
                << "    \"description\": \"" << escape_json(r.description) << "\"\n"
                << "  }";
            if (i + 1 < records_.size()) out << ",";
            out << "\n";
        }
        out << "]\n";
        return VoidResult<>::ok();
    }

    // Minimal JSON array-of-objects parser.
    VoidResult<> parse(const std::string& json) {
        records_.clear();
        size_t pos = 0;
        auto skip_ws = [&]() {
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                   json[pos] == '\r' || json[pos] == '\t'))
                ++pos;
        };

        skip_ws();
        if (pos >= json.size() || json[pos] != '[')
            return VoidResult<>::error("expected '[' at start");
        ++pos;

        while (true) {
            skip_ws();
            if (pos >= json.size()) break;
            if (json[pos] == ']') break;
            if (json[pos] == ',') { ++pos; continue; }
            if (json[pos] != '{')
                return VoidResult<>::error("expected '{' for record");

            auto rec_result = parse_object(json, pos);
            if (!rec_result)
                return VoidResult<>::error(rec_result.err());
            records_.push_back(std::move(rec_result.value()));
        }
        return VoidResult<>::ok();
    }

    Result<PatchRecord, std::string> parse_object(
            const std::string& json, size_t& pos) {
        PatchRecord rec;
        ++pos; // skip '{'
        auto skip_ws = [&]() {
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                   json[pos] == '\r' || json[pos] == '\t'))
                ++pos;
        };

        while (true) {
            skip_ws();
            if (pos >= json.size()) break;
            if (json[pos] == '}') { ++pos; break; }
            if (json[pos] == ',') { ++pos; continue; }

            // Parse key
            auto key_res = parse_string(json, pos);
            if (!key_res)
                return Result<PatchRecord, std::string>::error(key_res.err());
            std::string key = std::move(key_res.value());

            skip_ws();
            if (pos >= json.size() || json[pos] != ':')
                return Result<PatchRecord, std::string>::error("expected ':'");
            ++pos;
            skip_ws();

            auto val_res = parse_string(json, pos);
            if (!val_res)
                return Result<PatchRecord, std::string>::error(val_res.err());
            std::string val = std::move(val_res.value());

            if (key == "patch_id")       rec.patch_id = val;
            else if (key == "type")      rec.type = patch_type_from_str(val);
            else if (key == "target")    rec.target = val;
            else if (key == "patch_source") rec.patch_source = val;
            else if (key == "applied_at")   rec.applied_at = val;
            else if (key == "status")       rec.status = patch_status_from_str(val);
            else if (key == "rollback_data") rec.rollback_data = val;
            else if (key == "description")   rec.description = val;
        }

        return Result<PatchRecord, std::string>::ok(std::move(rec));
    }

    Result<std::string, std::string> parse_string(
            const std::string& json, size_t& pos) {
        if (pos >= json.size() || json[pos] != '"')
            return Result<std::string, std::string>::error("expected '\"'");
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                val += json[pos];
                val += json[pos + 1];
                pos += 2;
            } else {
                val += json[pos];
                ++pos;
            }
        }
        if (pos < json.size()) ++pos; // skip closing '"'
        return Result<std::string, std::string>::ok(unescape_json(val));
    }
};

} // namespace straylight::hotpatch
