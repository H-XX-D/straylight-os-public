// apps/player/playlist.cpp
// StrayLight Player — playlist loading and management
#include "playlist.h"

#include <straylight/log.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>

namespace straylight::player {

// ---------------------------------------------------------------------------
// Supported media extensions
// ---------------------------------------------------------------------------

static constexpr std::array<std::string_view, 24> kAudioExts = {
    "mp3","flac","ogg","opus","m4a","aac","wav","wma","ape","mka",
    "alac","aiff","aif","wv","tta","tak","m3u","pls","mp2","mpc","ra","rm","mid","mod",
};
static constexpr std::array<std::string_view, 14> kVideoExts = {
    "mp4","mkv","avi","mov","wmv","flv","webm","ts","m2ts","mts","ogv","3gp","hevc","m4v",
};

bool Playlist::is_media_file(const std::filesystem::path& path) {
    const auto ext_str = path.extension().string();
    if (ext_str.empty()) return false;
    // Strip leading dot, lowercase
    std::string ext = ext_str.substr(1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    for (auto& e : kAudioExts) if (e == ext) return true;
    for (auto& e : kVideoExts) if (e == ext) return true;
    return false;
}

std::string Playlist::uri_from_path(const std::filesystem::path& path) {
    return "file://" + path.string();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Playlist::Playlist()
    : rng_(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
{}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string Playlist::trim(std::string_view sv) {
    size_t s = sv.find_first_not_of(" \t\r\n");
    if (s == std::string_view::npos) return {};
    size_t e = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(s, e - s + 1));
}

bool Playlist::starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// M3U loading
// ---------------------------------------------------------------------------

Result<int, SLError> Playlist::load_m3u(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Result<int, SLError>::error(
            SLError{SLErrorCode::NotFound, "Cannot open M3U: " + path.string()});
    }

    const std::filesystem::path base_dir = path.parent_path();
    int added = 0;
    Track pending;
    std::string line;

    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (starts_with_ci(line, "#EXTM3U")) {
            continue; // header
        }

        if (starts_with_ci(line, "#EXTINF:")) {
            // #EXTINF:duration,title
            const auto comma = line.find(',');
            if (comma != std::string::npos) {
                pending.title = trim(line.substr(comma + 1));
                try {
                    pending.duration = std::stod(line.substr(8, comma - 8));
                } catch (...) {
                    pending.duration = -1.0;
                }
            }
            continue;
        }

        if (line.starts_with('#')) continue;  // other comment

        // Track URI
        Track t = pending;
        if (line.starts_with("file://") || line.starts_with("http://") ||
            line.starts_with("https://") || line.starts_with("rtsp://")) {
            t.uri = line;
        } else if (line.starts_with('/')) {
            t.uri = "file://" + line;
        } else {
            // Relative path — resolve against base directory
            t.uri = uri_from_path(base_dir / line);
        }

        if (t.title.empty()) {
            t.title = std::filesystem::path(line).filename().string();
        }

        tracks_.push_back(std::move(t));
        pending = {};
        ++added;
    }

    if (added > 0 && current_ < 0) current_ = 0;
    if (shuffle_) rebuild_shuffle_order();
    SL_INFO("Loaded {} tracks from M3U: {}", added, path.string());
    return Result<int, SLError>::ok(added);
}

// ---------------------------------------------------------------------------
// PLS loading
// ---------------------------------------------------------------------------

Result<int, SLError> Playlist::load_pls(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Result<int, SLError>::error(
            SLError{SLErrorCode::NotFound, "Cannot open PLS: " + path.string()});
    }

    std::map<int, Track> track_map;
    int num_entries = 0;
    std::string line;

    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '[' || line[0] == ';') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const auto key = line.substr(0, eq);
        const auto val = line.substr(eq + 1);

        if (starts_with_ci(key, "File")) {
            int idx = 0;
            try { idx = std::stoi(key.substr(4)); } catch (...) { continue; }
            auto& t = track_map[idx];
            const std::string v = trim(val);
            if (v.starts_with('/')) t.uri = "file://" + v;
            else if (v.find("://") != std::string::npos) t.uri = v;
            else t.uri = "file://" + v;
        } else if (starts_with_ci(key, "Title")) {
            int idx = 0;
            try { idx = std::stoi(key.substr(5)); } catch (...) { continue; }
            track_map[idx].title = trim(val);
        } else if (starts_with_ci(key, "Length")) {
            int idx = 0;
            try { idx = std::stoi(key.substr(6)); } catch (...) { continue; }
            try { track_map[idx].duration = std::stod(trim(val)); } catch (...) {}
        } else if (starts_with_ci(key, "NumberOfEntries")) {
            try { num_entries = std::stoi(trim(val)); } catch (...) {}
        }
    }

    int added = 0;
    for (auto& [idx, t] : track_map) {
        if (t.uri.empty()) continue;
        if (t.title.empty()) t.title = std::filesystem::path(t.uri).filename().string();
        tracks_.push_back(std::move(t));
        ++added;
    }

    (void)num_entries;
    if (added > 0 && current_ < 0) current_ = 0;
    if (shuffle_) rebuild_shuffle_order();
    SL_INFO("Loaded {} tracks from PLS: {}", added, path.string());
    return Result<int, SLError>::ok(added);
}

// ---------------------------------------------------------------------------
// Directory scan
// ---------------------------------------------------------------------------

Result<int, SLError> Playlist::load_directory(const std::filesystem::path& dir) {
    if (!std::filesystem::is_directory(dir)) {
        return Result<int, SLError>::error(
            SLError{SLErrorCode::NotFound, "Not a directory: " + dir.string()});
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && is_media_file(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        Track t;
        t.uri   = uri_from_path(f);
        t.title = f.filename().string();
        tracks_.push_back(std::move(t));
    }

    const int added = static_cast<int>(files.size());
    if (added > 0 && current_ < 0) current_ = 0;
    if (shuffle_) rebuild_shuffle_order();
    SL_INFO("Added {} tracks from directory: {}", added, dir.string());
    return Result<int, SLError>::ok(added);
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

void Playlist::add_track(Track t) {
    tracks_.push_back(std::move(t));
    if (current_ < 0 && !tracks_.empty()) current_ = 0;
    if (shuffle_) rebuild_shuffle_order();
}

void Playlist::remove_track(int index) {
    if (index < 0 || index >= count()) return;
    tracks_.erase(tracks_.begin() + index);
    if (current_ >= count()) current_ = count() - 1;
    if (shuffle_) rebuild_shuffle_order();
}

void Playlist::clear() {
    tracks_.clear();
    current_       = -1;
    shuffle_order_.clear();
    shuffle_pos_   = 0;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

const Track* Playlist::current_track() const {
    if (current_ < 0 || current_ >= count()) return nullptr;
    return &tracks_[static_cast<size_t>(current_)];
}

bool Playlist::next() {
    if (empty()) return false;

    if (repeat_ == RepeatMode::One) {
        // Stay on current track
        return true;
    }

    if (shuffle_) {
        if (shuffle_pos_ + 1 < static_cast<int>(shuffle_order_.size())) {
            ++shuffle_pos_;
            current_ = shuffle_order_[static_cast<size_t>(shuffle_pos_)];
            return true;
        }
        if (repeat_ == RepeatMode::All) {
            rebuild_shuffle_order();
            shuffle_pos_ = 0;
            current_ = shuffle_order_[0];
            return true;
        }
        return false;
    }

    if (current_ + 1 < count()) {
        ++current_;
        return true;
    }
    if (repeat_ == RepeatMode::All) {
        current_ = 0;
        return true;
    }
    return false;
}

bool Playlist::prev() {
    if (empty()) return false;

    if (repeat_ == RepeatMode::One) return true;

    if (shuffle_) {
        if (shuffle_pos_ > 0) {
            --shuffle_pos_;
            current_ = shuffle_order_[static_cast<size_t>(shuffle_pos_)];
            return true;
        }
        return false;
    }

    if (current_ - 1 >= 0) {
        --current_;
        return true;
    }
    if (repeat_ == RepeatMode::All) {
        current_ = count() - 1;
        return true;
    }
    return false;
}

void Playlist::jump_to(int index) {
    if (index < 0 || index >= count()) return;
    current_ = index;
    if (shuffle_) {
        // Update shuffle position to point to this track
        for (int i = 0; i < static_cast<int>(shuffle_order_.size()); ++i) {
            if (shuffle_order_[static_cast<size_t>(i)] == index) {
                shuffle_pos_ = i;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Shuffle
// ---------------------------------------------------------------------------

void Playlist::set_shuffle(bool s) {
    shuffle_ = s;
    if (shuffle_) rebuild_shuffle_order();
}

void Playlist::rebuild_shuffle_order() {
    shuffle_order_.resize(static_cast<size_t>(count()));
    std::iota(shuffle_order_.begin(), shuffle_order_.end(), 0);
    std::shuffle(shuffle_order_.begin(), shuffle_order_.end(), rng_);
    // Move current track to front
    if (current_ >= 0) {
        auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), current_);
        if (it != shuffle_order_.end()) {
            std::rotate(shuffle_order_.begin(), it, it + 1);
        }
    }
    shuffle_pos_ = 0;
}

} // namespace straylight::player
