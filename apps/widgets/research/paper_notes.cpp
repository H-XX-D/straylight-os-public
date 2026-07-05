// apps/widgets/research/paper_notes.cpp
#include "paper_notes.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::PaperNotesWidget, "paper_notes", "Paper Notes", straylight::widgets::WidgetCategory::Research);
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace straylight::widgets {

static const std::string kNotesDir = "/var/lib/straylight/notes";

bool PaperNotesWidget::is_heading(const std::string& line, int& level) {
    level = 0;
    for (size_t i = 0; i < line.size() && line[i] == '#'; ++i) level++;
    return level > 0 && level <= 6 && level < static_cast<int>(line.size()) && line[level] == ' ';
}

bool PaperNotesWidget::is_bullet(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') i++;
    return i < line.size() && (line[i] == '-' || line[i] == '*') && i + 1 < line.size() && line[i + 1] == ' ';
}

bool PaperNotesWidget::is_code_fence(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') i++;
    return line.size() - i >= 3 && line.substr(i, 3) == "```";
}

bool PaperNotesWidget::is_math_block(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') i++;
    return line.size() - i >= 2 && line.substr(i, 2) == "$$";
}

std::string PaperNotesWidget::strip_md_prefix(const std::string& line, int heading_level) {
    if (heading_level > 0 && heading_level + 1 < static_cast<int>(line.size())) {
        return line.substr(heading_level + 1);
    }
    return line;
}

void PaperNotesWidget::load_notes_from_disk() {
    namespace fs = std::filesystem;
    notes_.clear();

    if (!fs::exists(kNotesDir)) {
        // Create default note
        NoteEntry n;
        n.title = "Welcome";
        n.content = "# Welcome to Paper Notes\n\n"
                     "Use this panel for research notes with **Markdown** formatting.\n\n"
                     "## Features\n"
                     "- Markdown rendering with headings, bullets, bold, italic\n"
                     "- Code blocks with syntax highlighting hints\n"
                     "- Math notation: $E = mc^2$ and display math:\n\n"
                     "$$\n"
                     "\\nabla \\cdot \\mathbf{E} = \\frac{\\rho}{\\epsilon_0}\n"
                     "$$\n\n"
                     "## Example Code\n"
                     "```python\n"
                     "import torch\n"
                     "model = torch.nn.Linear(768, 10)\n"
                     "```\n";
        n.tags = "welcome, tutorial";
        notes_.push_back(std::move(n));
        return;
    }

    for (auto& entry : fs::directory_iterator(kNotesDir)) {
        if (entry.path().extension() != ".md") continue;

        NoteEntry n;
        n.title = entry.path().stem().string();
        std::ifstream f(entry.path());
        if (f) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            n.content = std::move(content);
        }

        // Try to read metadata sidecar
        std::string meta_path = entry.path().string() + ".meta";
        if (fs::exists(meta_path)) {
            std::ifstream mf(meta_path);
            std::string line;
            while (std::getline(mf, line)) {
                if (line.find("tags:") == 0) n.tags = line.substr(5);
                else if (line.find("created:") == 0) n.created_at = line.substr(8);
                else if (line.find("modified:") == 0) n.modified_at = line.substr(9);
            }
        }

        notes_.push_back(std::move(n));
    }

    std::sort(notes_.begin(), notes_.end(),
              [](const NoteEntry& a, const NoteEntry& b) { return a.title < b.title; });
}

void PaperNotesWidget::save_note(int index) {
    namespace fs = std::filesystem;
    if (index < 0 || index >= static_cast<int>(notes_.size())) return;

    fs::create_directories(kNotesDir);

    auto& n = notes_[index];
    std::string filepath = kNotesDir + "/" + n.title + ".md";
    std::ofstream f(filepath);
    if (f) {
        f << n.content;
        n.modified = false;
    }

    // Save metadata
    std::ofstream mf(filepath + ".meta");
    if (mf) {
        mf << "tags:" << n.tags << "\n";
        mf << "created:" << n.created_at << "\n";
        mf << "modified:" << n.modified_at << "\n";
    }
}

void PaperNotesWidget::add_new_note() {
    NoteEntry n;
    n.title = "Untitled Note";
    n.content = "# New Note\n\nStart writing here...\n";
    n.modified = true;
    notes_.push_back(std::move(n));
    selected_note_ = static_cast<int>(notes_.size()) - 1;
    mode_ = RenderMode::Edit;
}

void PaperNotesWidget::render_text_line(const std::string& line) {
    // Parse inline formatting: **bold**, *italic*, `code`, $math$
    size_t i = 0;
    while (i < line.size()) {
        // Bold
        if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
            size_t end = line.find("**", i + 2);
            if (end != std::string::npos) {
                std::string bold_text = line.substr(i + 2, end - i - 2);
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                ImGui::TextUnformatted(bold_text.c_str());
                ImGui::PopStyleColor();
                i = end + 2;
                continue;
            }
        }
        // Italic
        if (line[i] == '*' && (i + 1 >= line.size() || line[i + 1] != '*')) {
            size_t end = line.find('*', i + 1);
            if (end != std::string::npos) {
                std::string italic_text = line.substr(i + 1, end - i - 1);
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.9f, 1.0f, 0.9f));
                ImGui::TextUnformatted(italic_text.c_str());
                ImGui::PopStyleColor();
                i = end + 1;
                continue;
            }
        }
        // Inline code
        if (line[i] == '`') {
            size_t end = line.find('`', i + 1);
            if (end != std::string::npos) {
                std::string code_text = line.substr(i + 1, end - i - 1);
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.4f, 1));
                ImGui::TextUnformatted(code_text.c_str());
                ImGui::PopStyleColor();
                i = end + 1;
                continue;
            }
        }
        // Inline math $...$
        if (line[i] == '$' && (i + 1 >= line.size() || line[i + 1] != '$')) {
            size_t end = line.find('$', i + 1);
            if (end != std::string::npos) {
                std::string math_text = line.substr(i + 1, end - i - 1);
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.6f, 1));
                ImGui::Text("[%s]", math_text.c_str());
                ImGui::PopStyleColor();
                i = end + 1;
                continue;
            }
        }

        // Regular character — collect until next special char
        size_t next = line.find_first_of("*`$", i);
        std::string segment = (next != std::string::npos) ? line.substr(i, next - i) : line.substr(i);
        if (!segment.empty()) {
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(segment.c_str());
        }
        i = (next != std::string::npos) ? next : line.size();
    }
}

void PaperNotesWidget::render_markdown(const std::string& md) {
    std::istringstream ss(md);
    std::string line;
    bool in_code_block = false;
    bool in_math_block = false;

    while (std::getline(ss, line)) {
        // Code block toggle
        if (is_code_fence(line)) {
            in_code_block = !in_code_block;
            if (in_code_block) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.15f, 1));
                ImGui::BeginChild(("##code" + std::to_string(ImGui::GetCursorPosY())).c_str(),
                                  ImVec2(-1, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
            } else {
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            continue;
        }

        if (in_code_block) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.4f, 1));
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
            continue;
        }

        // Math block toggle
        if (is_math_block(line)) {
            in_math_block = !in_math_block;
            if (!in_math_block) {
                // End of math block — just continue
            }
            continue;
        }

        if (in_math_block) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 1.0f, 0.6f, 1));
            ImGui::Text("  [math] %s", line.c_str());
            ImGui::PopStyleColor();
            continue;
        }

        // Heading
        int level = 0;
        if (is_heading(line, level)) {
            std::string text = strip_md_prefix(line, level);
            float sizes[] = {0, 24.0f, 20.0f, 17.0f, 15.0f, 13.0f, 12.0f};
            float size = sizes[level];
            ImGui::SetWindowFontScale(size / 13.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 1.0f, 1));
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
            ImGui::SetWindowFontScale(1.0f);
            if (level <= 2) {
                ImGui::Separator();
            }
            continue;
        }

        // Bullet
        if (is_bullet(line)) {
            size_t i = 0;
            int indent = 0;
            while (i < line.size() && line[i] == ' ') { i++; indent++; }
            std::string bullet_text = line.substr(i + 2); // skip "- " or "* "
            ImGui::Indent(static_cast<float>(indent) * 8.0f + 8.0f);
            ImGui::Bullet();
            render_text_line(bullet_text);
            ImGui::Unindent(static_cast<float>(indent) * 8.0f + 8.0f);
            continue;
        }

        // Empty line = paragraph break
        if (line.empty()) {
            ImGui::Spacing();
            continue;
        }

        // Normal text with inline formatting
        ImGui::Dummy(ImVec2(0, 0)); // Anchor for SameLine(0,0) in render_text_line
        render_text_line(line);
    }
}

void PaperNotesWidget::update() {
    if (notes_.empty()) load_notes_from_disk();
}

void PaperNotesWidget::render(bool* p_open) {
    if (!ImGui::Begin("Paper Notes", p_open, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem("New Note")) add_new_note();
        if (ImGui::MenuItem("Save", nullptr, false, selected_note_ >= 0)) {
            save_note(selected_note_);
        }
        if (selected_note_ >= 0) {
            if (mode_ == RenderMode::Edit) {
                if (ImGui::MenuItem("Preview")) mode_ = RenderMode::Preview;
            } else {
                if (ImGui::MenuItem("Edit")) mode_ = RenderMode::Edit;
            }
        }
        ImGui::EndMenuBar();
    }

    // Search
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##nsearch", "Search notes...", search_buf_, sizeof(search_buf_));

    ImGui::Separator();

    std::string search(search_buf_);

    // Left: note list
    float list_w = 200.0f;
    ImGui::BeginChild("##note_list", ImVec2(list_w, 0), true);
    for (int i = 0; i < static_cast<int>(notes_.size()); ++i) {
        auto& n = notes_[i];
        if (!search.empty() && n.title.find(search) == std::string::npos &&
            n.content.find(search) == std::string::npos) continue;

        char lbl[256];
        std::snprintf(lbl, sizeof(lbl), "%s%s###n%d",
                      n.modified ? "* " : "", n.title.c_str(), i);
        if (ImGui::Selectable(lbl, selected_note_ == i)) {
            selected_note_ = i;
            std::strncpy(title_buf_, n.title.c_str(), sizeof(title_buf_) - 1);
            std::strncpy(content_buf_, n.content.c_str(), sizeof(content_buf_) - 1);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: note content
    ImGui::BeginChild("##note_content", ImVec2(0, 0), true);
    if (selected_note_ >= 0 && selected_note_ < static_cast<int>(notes_.size())) {
        auto& n = notes_[selected_note_];

        if (mode_ == RenderMode::Edit) {
            // Title
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##title", title_buf_, sizeof(title_buf_))) {
                n.title = title_buf_;
                n.modified = true;
            }

            ImGui::Separator();

            // Content editor
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (ImGui::InputTextMultiline("##content", content_buf_, sizeof(content_buf_),
                    ImVec2(-1, avail.y - 30),
                    ImGuiInputTextFlags_AllowTabInput)) {
                n.content = content_buf_;
                n.modified = true;
            }

            // Tags
            char tags_buf[256]{};
            std::strncpy(tags_buf, n.tags.c_str(), sizeof(tags_buf) - 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##tags", "Tags (comma-separated)", tags_buf, sizeof(tags_buf))) {
                n.tags = tags_buf;
                n.modified = true;
            }
        } else {
            // Preview mode
            ImGui::Text("Tags: %s", n.tags.c_str());
            ImGui::Separator();
            render_markdown(n.content);
        }
    } else {
        ImGui::TextWrapped("Select or create a note.");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace straylight::widgets
