// apps/font-gui/font_panel.h
// StrayLight Font Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
// straylight-wire: os includes
#include <unistd.h>
#include <cstdlib>
#include <cctype>
#include <set>
#include <array>

namespace straylight::fontmgr {

struct FontFamily {
    char name[64];
    char category[32];   // Serif, Sans-Serif, Monospace, Display
    char styles[128];    // "Regular, Bold, Italic, Bold Italic"
    int  num_styles;
    int  num_glyphs;
    bool installed;
};

struct FontState {
    std::vector<FontFamily> fonts;
    int  selected_font = 0;
    char preview_text[256] = "The quick brown fox jumps over the lazy dog. 0123456789";
    char search_filter[128] = {};
    char install_path[256] = {};
    char gfonts_search[128] = {};
    int  filter_category = 0;
    bool show_install_dialog = false;

    static constexpr const char* categories[] = {
        "All", "Sans-Serif", "Serif", "Monospace", "Display"
    };
    static constexpr int num_categories = 5;

    // straylight-wire: real fc-list datapath
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // Run a command, capture stdout. Returns false on spawn failure.
    static bool run_cmd(const std::string& cmd, std::string& out) {
        out.clear();
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) return false;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        int rc = ::pclose(p);
        return rc == 0;
    }

    // Shell-quote a single argument for safe interpolation.
    static std::string shq(const std::string& s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
        r += "'";
        return r;
    }

    // Split a comma-aliased family entry on its leading canonical name.
    static std::string lead_name(const std::string& fam) {
        size_t c = fam.find(',');
        std::string n = (c == std::string::npos) ? fam : fam.substr(0, c);
        size_t a = n.find_first_not_of(" \t");
        size_t b = n.find_last_not_of(" \t");
        if (a == std::string::npos) return std::string();
        return n.substr(a, b - a + 1);
    }

    // Category: spacing==100 => Monospace, else family-name keyword heuristic.
    static std::string categorize(const std::string& name, bool mono) {
        if (mono) return "Monospace";
        std::string l;
        for (char c : name) l += (char)std::tolower((unsigned char)c);
        if (l.find("mono") != std::string::npos) return "Monospace";
        if (l.find("serif") != std::string::npos &&
            l.find("sans") == std::string::npos) return "Serif";
        if (l.find("display") != std::string::npos ||
            l.find("emoji") != std::string::npos ||
            l.find("awesome") != std::string::npos) return "Display";
        if (l.find("sans") != std::string::npos) return "Sans-Serif";
        return "Sans-Serif";
    }

    static void set_cstr(char* dst, size_t cap, const std::string& s) {
        size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        memcpy(dst, s.data(), n);
        dst[n] = '\0';
    }

    void ensure_connected() {
        if (ok_) return;
        // fontconfig CLI is the data source; require fc-list present.
        if (::access("/usr/bin/fc-list", X_OK) != 0) {
            ok_ = false;
            err_ = "/usr/bin/fc-list not found or not executable";
            return;
        }
        ok_ = true;
        err_.clear();
    }

    void refresh() {
        ensure_connected();
        if (!ok_) return;

        std::string out;
        if (!run_cmd("fc-list : family | sort -u", out)) {
            ok_ = false;
            err_ = "fc-list : family failed";
            return;
        }

        // Collect unique canonical family names (leading name of aliases).
        std::vector<std::string> names;
        std::set<std::string> seen;
        size_t pos = 0;
        while (pos <= out.size()) {
            size_t nl = out.find('\n', pos);
            std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (nl == std::string::npos) pos = out.size() + 1; else pos = nl + 1;
            std::string nm = lead_name(line);
            if (nm.empty()) continue;
            if (seen.insert(nm).second) names.push_back(nm);
        }

        std::vector<FontFamily> next;
        next.reserve(names.size());
        for (const std::string& nm : names) {
            FontFamily f{};
            set_cstr(f.name, sizeof(f.name), nm);

            // spacing: 100 => monospace.
            std::string sp;
            bool mono = false;
            if (run_cmd("fc-list " + shq(nm) + " : spacing | sort -u", sp)) {
                mono = sp.find("100") != std::string::npos;
            }
            set_cstr(f.category, sizeof(f.category), categorize(nm, mono));

            // styles: strip ':style=', dedupe, join ', '.
            std::string so;
            std::vector<std::string> styles;
            std::set<std::string> sseen;
            if (run_cmd("fc-list " + shq(nm) + " : style | sort -u", so)) {
                size_t sp2 = 0;
                while (sp2 <= so.size()) {
                    size_t nl = so.find('\n', sp2);
                    std::string line = so.substr(sp2, nl == std::string::npos ? std::string::npos : nl - sp2);
                    if (nl == std::string::npos) sp2 = so.size() + 1; else sp2 = nl + 1;
                    size_t eq = line.find("style=");
                    std::string sv = (eq == std::string::npos) ? std::string() : line.substr(eq + 6);
                    // take leading style before any comma-alias
                    size_t cm = sv.find(',');
                    if (cm != std::string::npos) sv = sv.substr(0, cm);
                    size_t a = sv.find_first_not_of(" \t");
                    size_t b = sv.find_last_not_of(" \t");
                    if (a == std::string::npos) continue;
                    sv = sv.substr(a, b - a + 1);
                    if (sv.empty()) continue;
                    if (sseen.insert(sv).second) styles.push_back(sv);
                }
            }
            std::string joined;
            for (size_t i = 0; i < styles.size(); ++i) {
                if (i) joined += ", ";
                joined += styles[i];
            }
            set_cstr(f.styles, sizeof(f.styles), joined);
            f.num_styles = (int)styles.size();

            // num_glyphs: no cheap per-family real source; leave 0 (never fabricate).
            f.num_glyphs = 0;

            // Every family fc-list returns is installed.
            f.installed = true;

            next.push_back(f);
        }

        // Preserve user-driven installed flips across refresh by family name.
        for (auto& nf : next) {
            for (auto& of : fonts) {
                if (strcmp(nf.name, of.name) == 0) { nf.installed = of.installed; break; }
            }
        }
        fonts.swap(next);
        if (selected_font >= (int)fonts.size()) selected_font = fonts.empty() ? 0 : (int)fonts.size() - 1;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() {
        refresh();
    }
};

inline void render_font_panel(FontState& st) {
    // straylight-wire: refresh + unavailable banner
    if (st.fonts.empty()) st.init();
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("fontconfig unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("FONT MANAGER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##search", "Search fonts...", st.search_filter, sizeof(st.search_filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##cat", &st.filter_category, FontState::categories, FontState::num_categories);
    ImGui::SameLine();
    if (ImGui::Button("Install from File")) {
        st.show_install_dialog = true;
    }
    ImGui::Spacing();

    // Preview text input
    ImGui::Text("Preview:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##preview_text", st.preview_text, sizeof(st.preview_text));
    ImGui::Spacing();

    float list_w = ImGui::GetContentRegionAvail().x * 0.4f;

    // Font list with inline preview
    ImGui::BeginChild("##font_list", ImVec2(list_w, 0), true);
    ImGui::TextColored(accent, "Fonts");
    ImGui::Separator();

    for (int i = 0; i < (int)st.fonts.size(); ++i) {
        auto& f = st.fonts[i];

        // Apply filters
        if (st.filter_category > 0 &&
            strcmp(f.category, FontState::categories[st.filter_category]) != 0) continue;
        if (strlen(st.search_filter) > 0 &&
            strstr(f.name, st.search_filter) == nullptr) continue;

        ImGui::PushID(i);
        bool selected = (i == st.selected_font);

        // Font name with category badge
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##font_sel", selected, 0, ImVec2(0, 50))) {
            st.selected_font = i;
        }

        // Draw font info overlay
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(ImVec2(pos.x + 4, pos.y + 2), IM_COL32(220, 220, 220, 255), f.name);

        // Category badge
        ImU32 badge_col = IM_COL32(60, 60, 100, 255);
        if (strcmp(f.category, "Monospace") == 0) badge_col = IM_COL32(80, 40, 120, 255);
        else if (strcmp(f.category, "Serif") == 0) badge_col = IM_COL32(120, 60, 40, 255);
        else if (strcmp(f.category, "Display") == 0) badge_col = IM_COL32(40, 100, 80, 255);

        ImVec2 badge_pos(pos.x + ImGui::CalcTextSize(f.name).x + 12, pos.y + 3);
        ImVec2 badge_end(badge_pos.x + ImGui::CalcTextSize(f.category).x + 10, badge_pos.y + 16);
        draw->AddRectFilled(badge_pos, badge_end, badge_col, 3.0f);
        draw->AddText(ImVec2(badge_pos.x + 5, badge_pos.y + 1),
                      IM_COL32(200, 200, 200, 255), f.category);

        // Preview line (simulated - using default font since we can't load arbitrary fonts)
        draw->AddText(ImVec2(pos.x + 4, pos.y + 22),
                      IM_COL32(160, 160, 160, 255), st.preview_text);

        // Installed indicator
        if (f.installed) {
            draw->AddText(ImVec2(pos.x + 4, pos.y + 38),
                          IM_COL32(0, 200, 100, 200), "Installed");
        } else {
            draw->AddText(ImVec2(pos.x + 4, pos.y + 38),
                          IM_COL32(150, 150, 150, 200), "Not installed");
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Font detail panel
    ImGui::BeginChild("##font_detail", ImVec2(0, 0), true);
    if (st.selected_font >= 0 && st.selected_font < (int)st.fonts.size()) {
        auto& f = st.fonts[st.selected_font];

        ImGui::TextColored(accent, "%s", f.name);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Category:");  ImGui::SameLine(120); ImGui::Text("%s", f.category);
        ImGui::Text("Styles:");    ImGui::SameLine(120); ImGui::Text("%d", f.num_styles);
        ImGui::Text("Glyphs:");    ImGui::SameLine(120); ImGui::Text("%d", f.num_glyphs);
        ImGui::Text("Status:");    ImGui::SameLine(120);
        if (f.installed) ImGui::TextColored(accent, "Installed");
        else ImGui::TextDisabled("Not installed");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(accent, "Available Styles");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", f.styles);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(accent, "Preview");
        ImGui::Spacing();

        // Preview at different sizes
        float sizes[] = {12, 16, 20, 28, 36};
        const char* size_labels[] = {"12px", "16px", "20px", "28px", "36px"};
        for (int s = 0; s < 5; ++s) {
            ImGui::TextDisabled("%s:", size_labels[s]);
            ImGui::SameLine(60);
            // Simulated size preview using scale
            float scale = sizes[s] / 16.0f;
            ImGui::SetWindowFontScale(scale);
            ImGui::Text("%s", st.preview_text);
            ImGui::SetWindowFontScale(1.0f);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!f.installed) {
            if (ImGui::Button("Install Font", ImVec2(160, 32))) {
                f.installed = true;
            }
        } else {
            if (ImGui::Button("Uninstall Font", ImVec2(160, 32))) {
                f.installed = false;
            }
        }
    }
    ImGui::EndChild();

    // Install dialog
    if (st.show_install_dialog) {
        ImGui::OpenPopup("Install Font");
        st.show_install_dialog = false;
    }
    if (ImGui::BeginPopupModal("Install Font", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Install from file:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##path", "/path/to/font.ttf", st.install_path, sizeof(st.install_path));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Or search Google Fonts:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##gfonts", "Search Google Fonts...", st.gfonts_search, sizeof(st.gfonts_search));
        ImGui::Spacing();
        if (ImGui::Button("Install", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::fontmgr
