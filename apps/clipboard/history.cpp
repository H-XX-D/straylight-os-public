// apps/clipboard/history.cpp
// ClipHistory — ring-buffer clipboard storage with JSON persistence.
#include "history.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

// Base64 encode/decode for image blobs
namespace {

static const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kB64Table[(v >> 18) & 0x3F]);
        out.push_back(kB64Table[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? kB64Table[(v >>  6) & 0x3F] : '=');
        out.push_back((i + 2 < data.size()) ? kB64Table[(v      ) & 0x3F] : '=');
    }
    return out;
}

std::vector<uint8_t> base64_decode(const std::string& s) {
    static const int kDecTable[256] = [] {
        int t[256];
        std::fill(std::begin(t), std::end(t), -1);
        for (int i = 0; i < 64; ++i) t[(uint8_t)kB64Table[i]] = i;
        return std::array<int, 256>(t.begin(), t.end());
    }()[0]; // workaround: just use a lambda
    // Simple approach:
    std::vector<uint8_t> out;
    out.reserve(s.size() / 4 * 3);
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=') break;
        const char* pos = std::strchr(kB64Table, c);
        if (!pos) continue;
        val = (val << 6) + static_cast<int>(pos - kB64Table);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

} // namespace

namespace straylight::clipboard {

// ---------------------------------------------------------------------------
// ClipEntry::preview
// ---------------------------------------------------------------------------

std::string ClipEntry::preview(size_t max_chars) const {
    if (kind == EntryKind::Image) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[Image %zu bytes]", image_data.size());
        return buf;
    }
    // Truncate and strip newlines
    std::string out;
    out.reserve(max_chars + 4);
    for (char c : text) {
        if (out.size() >= max_chars) { out += "..."; break; }
        out.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// storage_path
// ---------------------------------------------------------------------------

std::filesystem::path ClipHistory::storage_path() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base =
        home ? std::filesystem::path(home) / ".local" / "share" / "straylight" / "clipboard"
             : std::filesystem::path("/tmp/straylight-clipboard");
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base / "history.json";
}

// ---------------------------------------------------------------------------
// push_text
// ---------------------------------------------------------------------------

void ClipHistory::push_text(std::string text, const std::string& mime) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    // Don't add duplicate of current top entry
    if (!entries_.empty() &&
        entries_.front().kind == EntryKind::Text &&
        entries_.front().text == text) return;

    ClipEntry e;
    e.kind      = EntryKind::Text;
    e.text      = std::move(text);
    e.mime      = mime;
    e.timestamp = std::chrono::system_clock::now();
    entries_.push_front(std::move(e));

    // Evict oldest non-pinned entries if over limit
    while (entries_.size() > kMaxEntries) {
        // Find last non-pinned
        for (auto it = entries_.end(); it != entries_.begin(); ) {
            --it;
            if (!it->pinned) { entries_.erase(it); break; }
        }
        // Safety: if all pinned somehow just break
        if (entries_.size() > kMaxEntries + 100) break;
    }
}

// ---------------------------------------------------------------------------
// push_image
// ---------------------------------------------------------------------------

void ClipHistory::push_image(std::vector<uint8_t> png_data, const std::string& mime) {
    if (png_data.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);

    ClipEntry e;
    e.kind       = EntryKind::Image;
    e.image_data = std::move(png_data);
    e.mime       = mime;
    e.timestamp  = std::chrono::system_clock::now();
    entries_.push_front(std::move(e));

    while (entries_.size() > kMaxEntries) {
        for (auto it = entries_.end(); it != entries_.begin(); ) {
            --it;
            if (!it->pinned) { entries_.erase(it); break; }
        }
        if (entries_.size() > kMaxEntries + 100) break;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::vector<ClipEntry> ClipHistory::entries() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return {entries_.begin(), entries_.end()};
}

size_t ClipHistory::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entries_.size();
}

void ClipHistory::toggle_pin(size_t index) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index < entries_.size()) {
        auto it = std::next(entries_.begin(), static_cast<std::ptrdiff_t>(index));
        it->pinned = !it->pinned;
    }
}

void ClipHistory::remove(size_t index) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index < entries_.size()) {
        entries_.erase(std::next(entries_.begin(), static_cast<std::ptrdiff_t>(index)));
    }
}

void ClipHistory::clear_unpinned() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = entries_.begin();
    while (it != entries_.end()) {
        if (!it->pinned) it = entries_.erase(it);
        else ++it;
    }
}

void ClipHistory::clear_all() {
    std::lock_guard<std::mutex> lk(mtx_);
    entries_.clear();
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

Result<void, SLError> ClipHistory::save() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json j = nlohmann::json::array();

    for (const auto& e : entries_) {
        nlohmann::json je;
        je["kind"]    = (e.kind == EntryKind::Text) ? "text" : "image";
        je["mime"]    = e.mime;
        je["pinned"]  = e.pinned;
        je["ts"]      = std::chrono::duration_cast<std::chrono::seconds>(
                             e.timestamp.time_since_epoch()).count();

        if (e.kind == EntryKind::Text) {
            je["text"] = e.text;
        } else if (e.pinned && !e.image_data.empty()) {
            // Only persist pinned images (can be large)
            je["data_b64"] = base64_encode(e.image_data);
        }
        j.push_back(std::move(je));
    }

    const auto path = storage_path();
    // Atomic write: write to .tmp then rename
    const auto tmp  = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream f(tmp);
        if (!f) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, "Cannot write: " + tmp.string()});
        }
        f << j.dump(2);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Rename failed: " + ec.message()});
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

Result<void, SLError> ClipHistory::load() {
    const auto path = storage_path();
    std::ifstream f(path);
    if (!f) {
        // No history file yet — not an error
        return Result<void, SLError>::ok();
    }

    nlohmann::json j;
    try { f >> j; } catch (const nlohmann::json::exception& ex) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError,
                    std::string("JSON parse error: ") + ex.what()});
    }

    if (!j.is_array()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, "Expected JSON array"});
    }

    std::lock_guard<std::mutex> lk(mtx_);
    entries_.clear();

    for (const auto& je : j) {
        ClipEntry e;
        const std::string kind = je.value("kind", "text");
        e.kind   = (kind == "image") ? EntryKind::Image : EntryKind::Text;
        e.mime   = je.value("mime", "text/plain");
        e.pinned = je.value("pinned", false);

        int64_t ts_sec = je.value("ts", int64_t(0));
        e.timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(ts_sec));

        if (e.kind == EntryKind::Text) {
            e.text = je.value("text", std::string{});
        } else {
            const std::string b64 = je.value("data_b64", std::string{});
            if (!b64.empty()) e.image_data = base64_decode(b64);
        }
        entries_.push_back(std::move(e));
    }

    return Result<void, SLError>::ok();
}

} // namespace straylight::clipboard
