// apps/widgets/research/paper_notes.h
#pragma once

#include <straylight/widget.h>
#include <string>
#include <vector>

namespace straylight::widgets {

struct NoteEntry {
    std::string title;
    std::string content;     // Markdown text
    std::string tags;
    std::string created_at;
    std::string modified_at;
    bool modified = false;
};

class PaperNotesWidget : public WidgetBase {
public:
    const char* name() const override { return "Paper Notes"; }
    float poll_interval() const override { return 0.0f; } // User-driven
    void update() override;
    void render(bool* p_open) override;

private:
    std::vector<NoteEntry> notes_;
    int selected_note_ = -1;
    bool editing_ = false;
    char title_buf_[256]{};
    char content_buf_[16384]{};
    char search_buf_[128]{};

    // Rendering state
    enum class RenderMode { Edit, Preview };
    RenderMode mode_ = RenderMode::Preview;

    void load_notes_from_disk();
    void save_note(int index);
    void add_new_note();
    void render_markdown(const std::string& md);
    void render_text_line(const std::string& line);

    static bool is_heading(const std::string& line, int& level);
    static bool is_bullet(const std::string& line);
    static bool is_code_fence(const std::string& line);
    static bool is_math_block(const std::string& line);
    static std::string strip_md_prefix(const std::string& line, int heading_level);
};

} // namespace straylight::widgets
