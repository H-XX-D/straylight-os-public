// apps/player/playlist.h
// StrayLight Player — M3U/PLS playlist management
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace straylight::player {

struct Track {
    std::string uri;      ///< file:// or network URI
    std::string title;    ///< Display name (extracted from tags or filename)
    double      duration = -1.0; ///< Seconds (-1 = unknown)
};

enum class RepeatMode {
    None,       ///< Stop at end
    One,        ///< Repeat current track
    All,        ///< Repeat entire playlist
};

class Playlist {
public:
    Playlist();

    /// Load an M3U or M3U8 file. Returns number of tracks added.
    Result<int, SLError> load_m3u(const std::filesystem::path& path);

    /// Load a PLS file.
    Result<int, SLError> load_pls(const std::filesystem::path& path);

    /// Scan a directory and add all supported audio/video files.
    Result<int, SLError> load_directory(const std::filesystem::path& dir);

    /// Add a single URI/track.
    void add_track(Track t);

    /// Remove track at index.
    void remove_track(int index);

    /// Clear all tracks.
    void clear();

    const std::vector<Track>& tracks() const { return tracks_; }
    int count() const { return static_cast<int>(tracks_.size()); }
    bool empty() const { return tracks_.empty(); }

    // Navigation
    int  current_index() const { return current_; }
    const Track* current_track() const;

    /// Move to the next track. Returns false if at end and no repeat.
    bool next();

    /// Move to the previous track.
    bool prev();

    /// Jump to a specific index.
    void jump_to(int index);

    // Shuffle / repeat
    bool shuffle() const { return shuffle_; }
    void set_shuffle(bool s);

    RepeatMode repeat() const { return repeat_; }
    void set_repeat(RepeatMode r) { repeat_ = r; }

private:
    std::vector<Track> tracks_;
    int                current_  = -1;
    bool               shuffle_  = false;
    RepeatMode         repeat_   = RepeatMode::None;

    // Shuffle order — indices into tracks_
    std::vector<int>         shuffle_order_;
    int                      shuffle_pos_  = 0;
    std::mt19937             rng_;

    void rebuild_shuffle_order();
    int  next_shuffle_index(bool forward) const;

    static std::string uri_from_path(const std::filesystem::path& path);
    static bool is_media_file(const std::filesystem::path& path);

    // M3U helpers
    static std::string trim(std::string_view sv);
    static bool starts_with_ci(std::string_view s, std::string_view prefix);
};

} // namespace straylight::player
