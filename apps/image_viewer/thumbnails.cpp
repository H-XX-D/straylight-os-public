// apps/image_viewer/thumbnails.cpp
// Directory scan, thumbnail generation, LRU GL texture cache.
#include "thumbnails.h"

#include <straylight/log.h>

#include <stb_image.h>

#include <algorithm>
#include <cmath>

namespace straylight::viewer {

// ---------------------------------------------------------------------------
// LruThumbCache
// ---------------------------------------------------------------------------

LruThumbCache::LruThumbCache(size_t max_entries)
    : max_entries_(max_entries) {}

LruThumbCache::~LruThumbCache() { evict_all(); }

GLuint LruThumbCache::get(const std::filesystem::path& path) {
    auto it = map_.find(path.string());
    if (it == map_.end()) return 0;
    // Move to front (most recently used)
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    return it->second->texture_id;
}

void LruThumbCache::put(const std::filesystem::path& path, GLuint texture_id,
                        uint32_t w, uint32_t h) {
    (void)w; (void)h;
    const std::string key = path.string();
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Update existing — delete old texture
        glDeleteTextures(1, &it->second->texture_id);
        it->second->texture_id = texture_id;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }
    if (map_.size() >= max_entries_) evict_one();
    lru_list_.push_front(CacheEntry{path, texture_id});
    map_[key] = lru_list_.begin();
}

void LruThumbCache::evict_one() {
    if (lru_list_.empty()) return;
    auto last = std::prev(lru_list_.end());
    glDeleteTextures(1, &last->texture_id);
    map_.erase(last->path.string());
    lru_list_.pop_back();
}

void LruThumbCache::evict_all() {
    for (auto& e : lru_list_) glDeleteTextures(1, &e.texture_id);
    lru_list_.clear();
    map_.clear();
}

// ---------------------------------------------------------------------------
// ThumbnailGrid
// ---------------------------------------------------------------------------

ThumbnailGrid::ThumbnailGrid(size_t cache_capacity) : cache_(cache_capacity) {}

Result<size_t, SLError> ThumbnailGrid::scan(const std::filesystem::path& dir) {
    clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return Result<size_t, SLError>::error(
            SLError{SLErrorCode::NotFound, "Not a directory: " + dir.string()});
    }

    std::vector<std::filesystem::path> paths;
    for (auto& entry : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (ImageLoader::is_supported(entry.path())) {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());

    entries_.reserve(paths.size());
    for (auto& p : paths) {
        ThumbEntry e;
        e.path = p;
        entries_.push_back(std::move(e));
    }
    return Result<size_t, SLError>::ok(entries_.size());
}

void ThumbnailGrid::generate_pending(int budget) {
    int generated = 0;
    for (size_t i = 0; i < entries_.size() && generated < budget; ++i) {
        if (!entries_[i].loaded && !entries_[i].failed) {
            generate(i);
            ++generated;
        }
    }
}

void ThumbnailGrid::generate(size_t index) {
    ThumbEntry& e = entries_[index];

    // Check cache first
    GLuint cached = cache_.get(e.path);
    if (cached != 0) {
        e.texture_id = cached;
        e.loaded     = true;
        return;
    }

    int w = 0, h = 0, comp = 0;
    uint8_t* data = stbi_load(e.path.c_str(), &w, &h, &comp, STBI_rgb_alpha);
    if (!data) {
        e.failed = true;
        return;
    }

    // Compute thumbnail dimensions preserving aspect ratio
    uint32_t tw, th;
    if (w >= h) {
        tw = kThumbSize;
        th = std::max(1u, static_cast<uint32_t>(kThumbSize * h / w));
    } else {
        th = kThumbSize;
        tw = std::max(1u, static_cast<uint32_t>(kThumbSize * w / h));
    }

    std::vector<uint8_t> thumb(tw * th * 4);
    // Box-filter downsample
    const float sx = static_cast<float>(w)  / static_cast<float>(tw);
    const float sy = static_cast<float>(h)  / static_cast<float>(th);
    for (uint32_t dy = 0; dy < th; ++dy) {
        for (uint32_t dx = 0; dx < tw; ++dx) {
            const int x0 = static_cast<int>(static_cast<float>(dx) * sx);
            const int y0 = static_cast<int>(static_cast<float>(dy) * sy);
            const int x1 = std::min(w - 1, static_cast<int>((static_cast<float>(dx) + 1.f) * sx));
            const int y1 = std::min(h - 1, static_cast<int>((static_cast<float>(dy) + 1.f) * sy));
            uint32_t sum[4] = {};
            int cnt = 0;
            for (int sy2 = y0; sy2 <= y1; ++sy2) {
                for (int sx2 = x0; sx2 <= x1; ++sx2) {
                    const uint8_t* px = data + (sy2 * w + sx2) * 4;
                    sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2]; sum[3] += px[3];
                    ++cnt;
                }
            }
            if (cnt > 0) {
                uint8_t* out = thumb.data() + (dy * tw + dx) * 4;
                for (int c = 0; c < 4; ++c)
                    out[c] = static_cast<uint8_t>(sum[c] / static_cast<uint32_t>(cnt));
            }
        }
    }
    stbi_image_free(data);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(tw), static_cast<GLsizei>(th),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, thumb.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    cache_.put(e.path, tex, tw, th);
    e.texture_id = tex;
    e.width      = tw;
    e.height     = th;
    e.loaded     = true;
}

int ThumbnailGrid::draw_grid(float panel_width, float thumb_display_size) const {
    int clicked = -1;
    const float cell_size = thumb_display_size + 8.0f;
    const int cols = std::max(1, static_cast<int>(panel_width / cell_size));

    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const ThumbEntry& e = entries_[i];
        const int col = i % cols;

        if (col > 0) ImGui::SameLine(0, 4);

        ImGui::PushID(i);
        ImGui::BeginGroup();

        if (e.loaded && e.texture_id != 0) {
            // Aspect-correct display
            const float asp = (e.height > 0)
                ? static_cast<float>(e.width) / static_cast<float>(e.height)
                : 1.0f;
            const float disp_w = (asp >= 1.0f) ? thumb_display_size : thumb_display_size * asp;
            const float disp_h = (asp >= 1.0f) ? thumb_display_size / asp : thumb_display_size;
            const float pad_x  = (thumb_display_size - disp_w) * 0.5f;
            const float pad_y  = (thumb_display_size - disp_h) * 0.5f;

            ImGui::Dummy(ImVec2(thumb_display_size, pad_y));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);
            if (ImGui::ImageButton(
                    reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(e.texture_id)),
                    ImVec2(disp_w, disp_h))) {
                clicked = i;
            }
        } else if (e.failed) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.05f, 0.05f, 1.0f));
            ImGui::Button("?##thumb_fail", ImVec2(thumb_display_size, thumb_display_size));
            ImGui::PopStyleColor();
        } else {
            ImGui::Button("...##loading", ImVec2(thumb_display_size, thumb_display_size));
        }

        // Filename label (truncated)
        const std::string name = e.path.filename().string();
        const std::string label = name.size() > 14 ? name.substr(0, 12) + ".." : name;
        ImGui::TextUnformatted(label.c_str());

        ImGui::EndGroup();
        ImGui::PopID();
    }

    return clicked;
}

void ThumbnailGrid::clear() {
    cache_.evict_all();
    entries_.clear();
}

} // namespace straylight::viewer
