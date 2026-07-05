// apps/editor/buffer.cpp
// StrayLight Editor — gap-buffer text storage implementation
#include "buffer.h"

#include <straylight/error.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace straylight::editor {

static constexpr size_t kInitialGapSize = 4096;
static constexpr size_t kGrowFactor     = 2;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Buffer::Buffer() {
    data_.resize(kInitialGapSize);
    gap_start_ = 0;
    gap_end_   = kInitialGapSize;
    rebuild_line_index();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Result<Buffer, SLError> Buffer::from_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return Result<Buffer, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Cannot open file: " + path.string()});
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();
    if (ifs.fail() && !ifs.eof()) {
        return Result<Buffer, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "Read error: " + path.string()});
    }

    Buffer buf;
    buf.path_     = path;
    buf.modified_ = false;

    const std::string content = oss.str();
    if (!content.empty()) {
        buf.insert({0, 0}, content);
        // Inserting counts as a modification only for user edits,
        // but loading from file resets both stacks.
        buf.undo_stack_.clear();
        buf.redo_stack_.clear();
        buf.modified_ = false;
    }

    return Result<Buffer, SLError>::ok(std::move(buf));
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

Result<void, SLError> Buffer::save(const std::filesystem::path& path) const {
    const std::filesystem::path tmp = path.string() + ".sltemp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError,
                        "Cannot create temp file: " + tmp.string()});
        }
        const std::string t = text();
        ofs.write(t.data(), static_cast<std::streamsize>(t.size()));
        if (!ofs) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError,
                        "Write failed: " + tmp.string()});
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "Rename failed: " + ec.message()});
    }

    // Cast away const: save is logically const but clears modified flag.
    const_cast<Buffer*>(this)->modified_ = false;
    const_cast<Buffer*>(this)->path_     = path;
    return Result<void, SLError>::ok();
}

Result<void, SLError> Buffer::save() const {
    if (path_.empty()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::InvalidArgument, "No file path set"});
    }
    return save(path_);
}

// ---------------------------------------------------------------------------
// Insert / erase
// ---------------------------------------------------------------------------

void Buffer::insert(Position pos, std::string_view text) {
    if (text.empty()) return;

    const size_t offset = pos_to_offset(pos);
    ensure_gap(text.size());
    move_gap(offset);

    std::memcpy(data_.data() + gap_start_, text.data(), text.size());
    gap_start_ += text.size();

    rebuild_line_index();

    push_undo(Edit{Edit::Kind::Erase, pos, std::string(text)});
    modified_ = true;
}

void Buffer::erase(Position start, Position end) {
    if (start == end) return;
    if (end < start) std::swap(start, end);

    const size_t s_off = pos_to_offset(start);
    const size_t e_off = pos_to_offset(end);
    if (s_off >= e_off) return;

    // Save erased text for undo
    std::string erased;
    erased.reserve(e_off - s_off);
    for (size_t i = s_off; i < e_off; ++i) {
        erased += char_at(i);
    }

    move_gap(s_off);
    // Extend gap to consume the erased region
    gap_end_ = gap_start_ + (e_off - s_off) +
               (gap_end_ - gap_start_);   // gap_end_ already accounts for gap

    // Recompute: after move_gap(s_off), gap_start_ == s_off.
    // The logical bytes [s_off, e_off) map to physical [gap_end_, gap_end_ + (e_off-s_off)).
    // Just advance gap_end_ by the count.
    gap_end_ = (gap_start_) + (gap_end_ - gap_start_) + (e_off - s_off);
    // Redo the arithmetic cleanly:
    move_gap(s_off);
    gap_end_ += (e_off - s_off);

    rebuild_line_index();

    push_undo(Edit{Edit::Kind::Insert, start, std::move(erased)});
    modified_ = true;
}

// ---------------------------------------------------------------------------
// Text access
// ---------------------------------------------------------------------------

std::string Buffer::text() const {
    std::string result;
    result.reserve(content_size());
    for (size_t i = 0; i < content_size(); ++i) {
        result += char_at(i);
    }
    return result;
}

std::string_view Buffer::line(int index) const {
    if (index < 0 || index >= static_cast<int>(line_starts_.size())) {
        return {};
    }
    // Build a temporary string — caller must use it before next mutation.
    // We return a view into an internal cache string.
    static thread_local std::string line_cache;
    line_cache.clear();

    const size_t start = line_starts_[static_cast<size_t>(index)];
    const size_t sz    = content_size();
    const size_t end   = (index + 1 < static_cast<int>(line_starts_.size()))
                             ? line_starts_[static_cast<size_t>(index + 1)]
                             : sz;

    for (size_t i = start; i < end && i < sz; ++i) {
        char c = char_at(i);
        if (c == '\n') break;
        line_cache += c;
    }
    return line_cache;
}

int Buffer::line_count() const {
    return static_cast<int>(line_starts_.size());
}

int Buffer::line_length(int idx) const {
    if (idx < 0 || idx >= line_count()) return 0;
    const size_t start = line_starts_[static_cast<size_t>(idx)];
    const size_t sz    = content_size();
    const size_t end   = (idx + 1 < line_count())
                             ? line_starts_[static_cast<size_t>(idx + 1)]
                             : sz;
    int len = 0;
    for (size_t i = start; i < end && i < sz; ++i) {
        if (char_at(i) == '\n') break;
        ++len;
    }
    return len;
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

void Buffer::undo() {
    if (undo_stack_.empty()) return;
    Edit e = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    apply_edit(e, false);
}

void Buffer::redo() {
    if (redo_stack_.empty()) return;
    Edit e = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    apply_edit(e, true);
}

void Buffer::push_undo(Edit edit) {
    undo_stack_.push_back(std::move(edit));
    redo_stack_.clear();
}

void Buffer::apply_edit(const Edit& e, bool for_redo) {
    if (e.kind == Edit::Kind::Insert) {
        const size_t offset = pos_to_offset(e.pos);
        ensure_gap(e.text.size());
        move_gap(offset);
        std::memcpy(data_.data() + gap_start_, e.text.data(), e.text.size());
        gap_start_ += e.text.size();
        rebuild_line_index();
        modified_ = true;

        Edit inv{Edit::Kind::Erase, e.pos, e.text};
        if (for_redo) undo_stack_.push_back(std::move(inv));
        else          redo_stack_.push_back(std::move(inv));
    } else {
        // Erase
        const size_t s_off = pos_to_offset(e.pos);
        const size_t e_off = s_off + e.text.size();
        move_gap(s_off);
        gap_end_ += (e_off - s_off);
        rebuild_line_index();
        modified_ = true;

        Edit inv{Edit::Kind::Insert, e.pos, e.text};
        if (for_redo) undo_stack_.push_back(std::move(inv));
        else          redo_stack_.push_back(std::move(inv));
    }
}

// ---------------------------------------------------------------------------
// Gap buffer internals
// ---------------------------------------------------------------------------

void Buffer::move_gap(size_t offset) {
    const size_t gap_size = gap_end_ - gap_start_;
    if (offset == gap_start_) return;

    if (offset < gap_start_) {
        const size_t count = gap_start_ - offset;
        std::memmove(data_.data() + offset + gap_size,
                     data_.data() + offset,
                     count);
        gap_start_ = offset;
        gap_end_   = offset + gap_size;
    } else {
        // offset > gap_start_ — offset is a logical offset
        // physical position of offset = offset + gap_size
        const size_t phys_offset = offset + gap_size;
        const size_t count       = phys_offset - gap_end_;
        std::memmove(data_.data() + gap_start_,
                     data_.data() + gap_end_,
                     count);
        gap_start_ = offset;
        gap_end_   = offset + gap_size;
    }
}

void Buffer::ensure_gap(size_t needed) {
    const size_t gap_size = gap_end_ - gap_start_;
    if (gap_size >= needed) return;

    const size_t content = content_size();
    const size_t new_gap = std::max(needed * kGrowFactor, kInitialGapSize);
    const size_t new_size = content + new_gap;

    std::vector<char> new_data(new_size);
    // Copy before gap
    std::memcpy(new_data.data(), data_.data(), gap_start_);
    // Copy after gap
    const size_t after_len = data_.size() - gap_end_;
    std::memcpy(new_data.data() + gap_start_ + new_gap,
                data_.data() + gap_end_,
                after_len);

    data_      = std::move(new_data);
    gap_end_   = gap_start_ + new_gap;
}

size_t Buffer::pos_to_offset(Position pos) const {
    if (pos.line < 0 || pos.line >= line_count()) return content_size();
    const size_t line_start = line_starts_[static_cast<size_t>(pos.line)];
    const int    line_len   = line_length(pos.line);
    const int    col        = std::min(pos.col, line_len);
    return line_start + static_cast<size_t>(std::max(0, col));
}

Position Buffer::offset_to_pos(size_t offset) const {
    if (line_starts_.empty()) return {0, 0};
    // Binary search for the line
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    if (it != line_starts_.begin()) --it;
    int line = static_cast<int>(std::distance(line_starts_.begin(), it));
    int col  = static_cast<int>(offset - *it);
    return {line, col};
}

char Buffer::char_at(size_t logical_idx) const {
    // Map logical index to physical index (skip gap)
    const size_t phys = (logical_idx < gap_start_)
                            ? logical_idx
                            : logical_idx + (gap_end_ - gap_start_);
    return data_[phys];
}

size_t Buffer::content_size() const {
    return data_.size() - (gap_end_ - gap_start_);
}

void Buffer::rebuild_line_index() {
    line_starts_.clear();
    line_starts_.push_back(0);
    const size_t sz = content_size();
    for (size_t i = 0; i < sz; ++i) {
        if (char_at(i) == '\n') {
            line_starts_.push_back(i + 1);
        }
    }
}

} // namespace straylight::editor
