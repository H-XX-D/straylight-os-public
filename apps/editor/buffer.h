// apps/editor/buffer.h
// StrayLight Editor — gap-buffer backed text storage
#pragma once
#include <straylight/result.h>
#include <straylight/error.h>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace straylight::editor {

struct Position {
    int line = 0;
    int col  = 0;

    auto operator<=>(const Position&) const = default;
};

struct Selection {
    Position anchor;
    Position cursor;

    bool empty() const { return anchor == cursor; }
};

class Buffer {
public:
    Buffer();

    /// Load file into a new buffer.
    static Result<Buffer, SLError> from_file(const std::filesystem::path& path);

    /// Atomic save to a given path (write .tmp then rename).
    Result<void, SLError> save(const std::filesystem::path& path) const;

    /// Save to the buffer's original path.
    Result<void, SLError> save() const;

    /// Insert text at a logical position.
    void insert(Position pos, std::string_view text);

    /// Erase the range [start, end).
    void erase(Position start, Position end);

    /// Return the entire text content.
    std::string text() const;

    /// Return a single line by 0-based index (no trailing newline).
    std::string_view line(int index) const;

    int line_count()         const;
    int line_length(int idx) const;

    // Undo / redo
    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    void undo();
    void redo();

    bool modified()               const { return modified_; }
    const std::filesystem::path& file_path() const { return path_; }

private:
    // ---- Gap buffer ----
    // Layout: [content_before_gap | gap | content_after_gap]
    std::vector<char> data_;
    size_t            gap_start_ = 0;
    size_t            gap_end_   = 0;

    // ---- Line index ----
    // Stores logical byte offsets (0-based) of the start of each line.
    std::vector<size_t> line_starts_;
    void rebuild_line_index();

    // ---- Undo / redo ----
    struct Edit {
        enum class Kind { Insert, Erase };
        Kind      kind;
        Position  pos;
        std::string text;
    };
    std::vector<Edit> undo_stack_;
    std::vector<Edit> redo_stack_;
    void push_undo(Edit edit);
    void apply_edit(const Edit& e, bool for_redo);

    std::filesystem::path path_;
    bool                  modified_ = false;

    // ---- Internal helpers ----
    void   move_gap(size_t offset);
    void   ensure_gap(size_t needed);
    size_t pos_to_offset(Position pos) const;
    Position offset_to_pos(size_t offset) const;
    char   char_at(size_t logical_idx) const;
    size_t content_size() const;
};

} // namespace straylight::editor
