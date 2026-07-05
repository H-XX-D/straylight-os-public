// apps/predict-gui/predict_panel.h
// StrayLight Prediction Dashboard panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace straylight::predict {

struct PredictedApp {
    char name[64];
    float probability;
    bool preloaded;
};

struct PreloadedResource {
    char name[64];
    char type[32];   // "binary", "library", "data"
    float size_mb;
};

struct PredictState {
    std::vector<PredictedApp> predictions;
    std::vector<PreloadedResource> preloaded;

    float ram_budget = 0.35f;     // fraction used
    float ram_max_gb = 2.0f;
    float vram_budget = 0.20f;
    float vram_max_gb = 1.0f;

    bool  model_training = false;
    float train_progress = 0.0f;
    int   model_accuracy = 87;
    int   total_predictions = 1248;
    int   correct_predictions = 1086;
    float anim_timer = 0;

    void init() {
        predictions.push_back({"Firefox", 0.92f, true});
        predictions.push_back({"Terminal", 0.88f, true});
        predictions.push_back({"File Manager", 0.74f, true});
        predictions.push_back({"Code Editor", 0.65f, true});
        predictions.push_back({"System Monitor", 0.52f, false});
        predictions.push_back({"Image Viewer", 0.38f, false});
        predictions.push_back({"Media Player", 0.25f, false});
        predictions.push_back({"Settings", 0.18f, false});

        preloaded.push_back({"firefox", "binary", 128.5f});
        preloaded.push_back({"libGLESv2.so", "library", 12.4f});
        preloaded.push_back({"libvulkan.so", "library", 8.2f});
        preloaded.push_back({"straylight-terminal", "binary", 4.8f});
        preloaded.push_back({"nautilus", "binary", 22.1f});
        preloaded.push_back({"font-cache", "data", 45.0f});
        preloaded.push_back({"icon-cache", "data", 18.3f});
    }
};

inline void render_predict_panel(PredictState& st) {
    if (st.predictions.empty()) st.init();

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);
    st.anim_timer += ImGui::GetIO().DeltaTime;

    // Simulate small fluctuations
    for (auto& p : st.predictions) {
        p.probability += 0.001f * sinf(st.anim_timer * 2.0f + (float)(p.name[0]));
        if (p.probability > 1.0f) p.probability = 1.0f;
        if (p.probability < 0.0f) p.probability = 0.0f;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("PREDICTION DASHBOARD");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.55f;

    // Top predicted apps
    ImGui::BeginChild("##predictions", ImVec2(left_w, ImGui::GetContentRegionAvail().y * 0.55f), true);
    ImGui::TextColored(accent, "App Launch Predictions");
    ImGui::Separator();
    ImGui::Spacing();

    for (int i = 0; i < (int)st.predictions.size(); ++i) {
        auto& p = st.predictions[i];
        ImGui::PushID(i);

        ImGui::Text("%-16s", p.name);
        ImGui::SameLine(140);

        // Probability bar
        ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        float bar_w = ImGui::GetContentRegionAvail().x - 100;
        float bar_h = 18;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                            IM_COL32(30, 30, 50, 255), 3.0f);

        // Gradient fill based on probability
        float fill = p.probability * bar_w;
        ImU32 col = (p.probability > 0.7f) ? IM_COL32(0, 255, 136, 255) :
                    (p.probability > 0.4f) ? IM_COL32(0, 180, 100, 255) :
                    IM_COL32(80, 80, 120, 255);
        draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + fill, bar_pos.y + bar_h),
                            col, 3.0f);

        // Percentage label
        char pct[16]; snprintf(pct, 16, "%.0f%%", p.probability * 100);
        draw->AddText(ImVec2(bar_pos.x + fill + 4, bar_pos.y + 1),
                      IM_COL32(200, 200, 200, 255), pct);

        ImGui::Dummy(ImVec2(bar_w, bar_h));
        ImGui::SameLine();

        if (p.preloaded) {
            ImGui::TextColored(accent, "LOADED");
        } else {
            ImGui::TextDisabled("idle");
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Resource budget gauges
    ImGui::BeginChild("##gauges", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f), true);
    ImGui::TextColored(accent, "Resource Budget");
    ImGui::Separator();
    ImGui::Spacing();

    // RAM gauge
    ImGui::Text("RAM Preload Budget");
    {
        ImVec2 gp = ImGui::GetCursorScreenPos();
        float gw = ImGui::GetContentRegionAvail().x - 10;
        float gh = 28;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(gp, ImVec2(gp.x + gw, gp.y + gh), IM_COL32(30, 30, 50, 255), 4.0f);

        ImU32 ram_col = (st.ram_budget > 0.8f) ? IM_COL32(255, 60, 60, 255) :
                        (st.ram_budget > 0.5f) ? IM_COL32(255, 200, 0, 255) :
                        IM_COL32(0, 200, 100, 255);
        draw->AddRectFilled(gp, ImVec2(gp.x + gw * st.ram_budget, gp.y + gh), ram_col, 4.0f);

        char ram_str[64];
        snprintf(ram_str, 64, "%.1f / %.1f GB (%.0f%%)",
                 st.ram_budget * st.ram_max_gb, st.ram_max_gb, st.ram_budget * 100);
        draw->AddText(ImVec2(gp.x + 8, gp.y + 6), IM_COL32(255, 255, 255, 255), ram_str);
        ImGui::Dummy(ImVec2(0, gh + 4));
    }

    ImGui::Spacing();
    ImGui::Text("VRAM Preload Budget");
    {
        ImVec2 gp = ImGui::GetCursorScreenPos();
        float gw = ImGui::GetContentRegionAvail().x - 10;
        float gh = 28;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(gp, ImVec2(gp.x + gw, gp.y + gh), IM_COL32(30, 30, 50, 255), 4.0f);

        ImU32 vram_col = (st.vram_budget > 0.8f) ? IM_COL32(255, 60, 60, 255) :
                         (st.vram_budget > 0.5f) ? IM_COL32(255, 200, 0, 255) :
                         IM_COL32(0, 200, 100, 255);
        draw->AddRectFilled(gp, ImVec2(gp.x + gw * st.vram_budget, gp.y + gh), vram_col, 4.0f);

        char vram_str[64];
        snprintf(vram_str, 64, "%.1f / %.1f GB (%.0f%%)",
                 st.vram_budget * st.vram_max_gb, st.vram_max_gb, st.vram_budget * 100);
        draw->AddText(ImVec2(gp.x + 8, gp.y + 6), IM_COL32(255, 255, 255, 255), vram_str);
        ImGui::Dummy(ImVec2(0, gh + 4));
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("RAM Limit (GB)", &st.ram_max_gb, 0.5f, 8.0f, "%.1f GB");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("VRAM Limit (GB)", &st.vram_max_gb, 0.25f, 4.0f, "%.2f GB");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(accent, "Model Status");
    ImGui::Spacing();
    ImGui::Text("Accuracy: %d%%", st.model_accuracy);
    ImGui::Text("Predictions: %d / %d correct", st.correct_predictions, st.total_predictions);

    if (!st.model_training) {
        if (ImGui::Button("Retrain Model", ImVec2(-1, 28))) {
            st.model_training = true;
            st.train_progress = 0;
        }
    } else {
        st.train_progress += ImGui::GetIO().DeltaTime * 0.08f;
        if (st.train_progress >= 1.0f) {
            st.model_training = false;
            st.model_accuracy = 89;
        }
        ImGui::ProgressBar(st.train_progress, ImVec2(-1, 20), "Training...");
    }

    ImGui::EndChild();

    // Preloaded resources list
    ImGui::BeginChild("##preloaded", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Currently Preloaded Resources");
    ImGui::Separator();

    if (ImGui::BeginTable("##res_table", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Resource");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)st.preloaded.size(); ++i) {
            auto& r = st.preloaded[i];
            ImGui::TableNextRow();
            ImGui::PushID(500 + i);
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", r.name);
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", r.type);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f MB", r.size_mb);
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Evict")) {
                st.preloaded.erase(st.preloaded.begin() + i);
                --i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    float total_mb = 0;
    for (auto& r : st.preloaded) total_mb += r.size_mb;
    ImGui::Text("Total preloaded: %.1f MB", total_mb);

    ImGui::EndChild();
}

} // namespace straylight::predict
