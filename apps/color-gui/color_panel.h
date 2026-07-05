// apps/color-gui/color_panel.h
// StrayLight Color Studio panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace straylight::color {

struct ColorEntry {
    float r, g, b, a;
    char  hex[16];
    char  name[32];
};

struct ColorState {
    float hsv[3] = {0.33f, 1.0f, 1.0f};
    float rgb[3] = {0.0f, 1.0f, 0.53f};
    float alpha = 1.0f;
    char  hex_input[16] = "#00FF88";

    std::vector<ColorEntry> palette;
    int  scheme_type = 0;
    bool show_export = false;

    static constexpr const char* scheme_types[] = {
        "Complementary", "Analogous", "Triadic", "Split-Complementary",
        "Tetradic", "Monochromatic"
    };
    static constexpr int num_schemes = 6;

    void init() {
        palette.push_back({0.0f, 1.0f, 0.53f, 1.0f, "#00FF88", "StrayLight Green"});
        palette.push_back({0.10f, 0.10f, 0.18f, 1.0f, "#1A1A2E", "Dark Background"});
        palette.push_back({0.08f, 0.08f, 0.13f, 1.0f, "#141421", "Deeper Dark"});
        palette.push_back({0.0f, 0.55f, 0.38f, 1.0f, "#008C61", "Accent Dark"});
        palette.push_back({0.0f, 0.8f, 0.55f, 1.0f, "#00CC8C", "Accent Bright"});
        palette.push_back({0.9f, 0.9f, 0.9f, 1.0f, "#E6E6E6", "Text Light"});
        palette.push_back({0.5f, 0.5f, 0.5f, 1.0f, "#808080", "Text Disabled"});
        palette.push_back({0.2f, 0.2f, 0.32f, 1.0f, "#333352", "Border"});
    }

    void sync_from_hsv() {
        ImGui::ColorConvertHSVtoRGB(hsv[0], hsv[1], hsv[2], rgb[0], rgb[1], rgb[2]);
        snprintf(hex_input, 16, "#%02X%02X%02X",
                 (int)(rgb[0]*255), (int)(rgb[1]*255), (int)(rgb[2]*255));
    }

    void sync_from_rgb() {
        ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], hsv[0], hsv[1], hsv[2]);
        snprintf(hex_input, 16, "#%02X%02X%02X",
                 (int)(rgb[0]*255), (int)(rgb[1]*255), (int)(rgb[2]*255));
    }

    void sync_from_hex() {
        unsigned int r = 0, g = 0, b = 0;
        sscanf(hex_input, "#%02x%02x%02x", &r, &g, &b);
        rgb[0] = static_cast<float>(r) / 255.0f; rgb[1] = static_cast<float>(g) / 255.0f; rgb[2] = static_cast<float>(b) / 255.0f;
        ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], hsv[0], hsv[1], hsv[2]);
    }
};

inline void render_color_panel(ColorState& st) {
    if (st.palette.empty()) st.init();

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("COLOR STUDIO");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.5f;

    // Left: Color picker
    ImGui::BeginChild("##picker", ImVec2(left_w, ImGui::GetContentRegionAvail().y * 0.6f), true);
    ImGui::TextColored(accent, "Color Picker");
    ImGui::Separator();
    ImGui::Spacing();

    // Color wheel / picker widget
    ImVec4 col(st.rgb[0], st.rgb[1], st.rgb[2], st.alpha);
    if (ImGui::ColorPicker4("##color_picker", (float*)&col,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_AlphaBar |
            ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV |
            ImGuiColorEditFlags_DisplayHex)) {
        st.rgb[0] = col.x; st.rgb[1] = col.y; st.rgb[2] = col.z; st.alpha = col.w;
        st.sync_from_rgb();
    }

    ImGui::Spacing();

    // Manual inputs
    ImGui::Text("RGB:");
    ImGui::SameLine(50);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat3("##rgb", st.rgb, 0.0f, 1.0f, "%.3f")) {
        st.sync_from_rgb();
    }

    ImGui::Text("HSV:");
    ImGui::SameLine(50);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat3("##hsv", st.hsv, 0.0f, 1.0f, "%.3f")) {
        st.sync_from_hsv();
    }

    ImGui::Text("Hex:");
    ImGui::SameLine(50);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputText("##hex", st.hex_input, sizeof(st.hex_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        st.sync_from_hex();
    }
    ImGui::SameLine();
    ImGui::Text("Alpha: %.2f", st.alpha);

    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Scheme generator
    ImGui::BeginChild("##scheme", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.6f), true);
    ImGui::TextColored(accent, "Scheme Generator");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Type:");
    ImGui::SameLine(80);
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##scheme_type", &st.scheme_type, ColorState::scheme_types, ColorState::num_schemes);
    ImGui::Spacing();

    // Generate scheme colors based on current color + scheme type
    float base_h = st.hsv[0], base_s = st.hsv[1], base_v = st.hsv[2];
    std::vector<float> hues;
    hues.push_back(base_h);

    switch (st.scheme_type) {
        case 0: // Complementary
            hues.push_back(fmodf(base_h + 0.5f, 1.0f));
            break;
        case 1: // Analogous
            hues.push_back(fmodf(base_h + 0.083f, 1.0f));
            hues.push_back(fmodf(base_h - 0.083f + 1.0f, 1.0f));
            break;
        case 2: // Triadic
            hues.push_back(fmodf(base_h + 0.333f, 1.0f));
            hues.push_back(fmodf(base_h + 0.667f, 1.0f));
            break;
        case 3: // Split-Complementary
            hues.push_back(fmodf(base_h + 0.417f, 1.0f));
            hues.push_back(fmodf(base_h + 0.583f, 1.0f));
            break;
        case 4: // Tetradic
            hues.push_back(fmodf(base_h + 0.25f, 1.0f));
            hues.push_back(fmodf(base_h + 0.5f, 1.0f));
            hues.push_back(fmodf(base_h + 0.75f, 1.0f));
            break;
        case 5: // Monochromatic
            hues.clear();
            for (int i = 0; i < 5; ++i) hues.push_back(base_h);
            break;
    }

    ImGui::Text("Generated Colors:");
    ImGui::Spacing();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 swatch_pos = ImGui::GetCursorScreenPos();
    float swatch_size = 50.0f;
    float spacing = 8.0f;

    for (int i = 0; i < (int)hues.size(); ++i) {
        float h = hues[i];
        float s = base_s;
        float v = (st.scheme_type == 5) ? base_v * (0.4f + 0.6f * (float)i / (float)(hues.size() - 1)) : base_v;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);

        ImVec2 tl(swatch_pos.x + i * (swatch_size + spacing), swatch_pos.y);
        ImVec2 br(tl.x + swatch_size, tl.y + swatch_size);
        draw->AddRectFilled(tl, br, IM_COL32((int)(r*255), (int)(g*255), (int)(b*255), 255), 4.0f);
        draw->AddRect(tl, br, IM_COL32(100, 100, 140, 255), 4.0f);

        char hex[16];
        snprintf(hex, 16, "#%02X%02X%02X", (int)(r*255), (int)(g*255), (int)(b*255));
        draw->AddText(ImVec2(tl.x, br.y + 4), IM_COL32(180, 180, 180, 255), hex);
    }
    ImGui::Dummy(ImVec2(0, swatch_size + 24));

    ImGui::Spacing();
    if (ImGui::Button("Add All to Palette")) {
        for (float h : hues) {
            float r, g, b;
            float v = base_v;
            ImGui::ColorConvertHSVtoRGB(h, base_s, v, r, g, b);
            ColorEntry ce;
            ce.r = r; ce.g = g; ce.b = b; ce.a = 1.0f;
            snprintf(ce.hex, 16, "#%02X%02X%02X", (int)(r*255), (int)(g*255), (int)(b*255));
            snprintf(ce.name, 32, "Color %d", (int)st.palette.size());
            st.palette.push_back(ce);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Export Palette")) st.show_export = true;
    ImGui::SameLine();
    if (ImGui::Button("Copy Current Hex")) {
        ImGui::SetClipboardText(st.hex_input);
    }

    ImGui::EndChild();

    // Bottom: Palette grid
    ImGui::BeginChild("##palette", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Palette (%zu colors)", st.palette.size());
    ImGui::Separator();
    ImGui::Spacing();

    float grid_size = 60.0f;
    float avail_w = ImGui::GetContentRegionAvail().x;
    int cols = (int)(avail_w / (grid_size + spacing));
    if (cols < 1) cols = 1;

    ImDrawList* pdraw = ImGui::GetWindowDrawList();
    ImVec2 grid_pos = ImGui::GetCursorScreenPos();

    int remove_idx = -1;
    for (int i = 0; i < (int)st.palette.size(); ++i) {
        auto& c = st.palette[i];
        int row = i / cols;
        int col = i % cols;
        ImVec2 tl(grid_pos.x + col * (grid_size + spacing), grid_pos.y + row * (grid_size + 20 + spacing));
        ImVec2 br(tl.x + grid_size, tl.y + grid_size);

        pdraw->AddRectFilled(tl, br, IM_COL32((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), 255), 4.0f);
        pdraw->AddRect(tl, br, IM_COL32(100, 100, 140, 255), 4.0f);
        pdraw->AddText(ImVec2(tl.x, br.y + 2), IM_COL32(160, 160, 160, 255), c.hex);

        // Check click to select
        ImGuiIO& io = ImGui::GetIO();
        if (io.MousePos.x >= tl.x && io.MousePos.x <= br.x &&
            io.MousePos.y >= tl.y && io.MousePos.y <= br.y) {
            if (ImGui::IsMouseClicked(0)) {
                st.rgb[0] = c.r; st.rgb[1] = c.g; st.rgb[2] = c.b;
                st.sync_from_rgb();
            }
            if (ImGui::IsMouseClicked(1)) {
                remove_idx = i;
            }
        }
    }
    if (remove_idx >= 0) st.palette.erase(st.palette.begin() + remove_idx);

    int total_rows = ((int)st.palette.size() + cols - 1) / cols;
    ImGui::Dummy(ImVec2(0, total_rows * (grid_size + 20 + spacing)));
    ImGui::TextDisabled("Click swatch to pick color. Right-click to remove.");
    ImGui::EndChild();

    // Export dialog
    if (st.show_export) {
        ImGui::OpenPopup("Export Palette");
        st.show_export = false;
    }
    if (ImGui::BeginPopupModal("Export Palette", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Export Format:");
        ImGui::Spacing();
        if (ImGui::Button("CSS Variables", ImVec2(200, 30))) ImGui::CloseCurrentPopup();
        if (ImGui::Button("JSON", ImVec2(200, 30))) ImGui::CloseCurrentPopup();
        if (ImGui::Button("SCSS Variables", ImVec2(200, 30))) ImGui::CloseCurrentPopup();
        if (ImGui::Button("GIMP Palette (.gpl)", ImVec2(200, 30))) ImGui::CloseCurrentPopup();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(200, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::color
