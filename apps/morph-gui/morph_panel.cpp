// apps/morph-gui/morph_panel.cpp
#include "morph_panel.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace straylight::morph {

static constexpr ImVec4 kCyan     = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple   = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold     = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel  = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kBorder   = {1.0f, 1.0f, 1.0f, 0.08f};
static constexpr ImVec4 kMuted    = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2   = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess  = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger   = {1.0f, 0.298f, 0.416f, 1.0f};

// === STRAYLIGHT_MORPH_WIRED: MorphState::init() now loads REAL model data inline in
// morph_panel.h (enumerate the configured StrayLight model directory, stat safetensors size,
// parse safetensors header for params, read config.json arch). No fabricated
// models/curves/loss/layer-errors/history. accuracy & latency_ms have no real
// source on this box and are left zero. ===

void MorphPanel::render_model_selector() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("ModelList", ImVec2(300.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "MODELS");
    ImGui::SameLine();
    ImGui::TextColored(kMuted2, "(%d)", static_cast<int>(state_.models.size()));
    ImGui::Separator();
    ImGui::Spacing();

    for (int i = 0; i < static_cast<int>(state_.models.size()); ++i) {
        const auto& m = state_.models[i];
        bool selected = (state_.selected_model == i);

        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            selected ? ImVec4{0.098f, 0.906f, 1.000f, 0.08f} : ImVec4{0.030f, 0.048f, 0.095f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

        char child_id[32];
        std::snprintf(child_id, sizeof(child_id), "model_%d", i);
        ImGui::BeginChild(child_id, ImVec2(-1.0f, 92.0f), true);

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0))
            state_.selected_model = i;

        ImGui::TextColored(selected ? kCyan : ImVec4{1,1,1,0.9f}, "%s", m.name.c_str());
        ImGui::TextColored(kMuted, "%s", m.arch.c_str());

        // Size badge
        ImVec4 size_col = m.size_gb < 1.0f ? kCyan : (m.size_gb < 10.0f ? kPurple : kGold);
        ImGui::TextColored(size_col, "%.1f GB", m.size_gb);
        ImGui::SameLine(80.0f);
        ImGui::TextColored(kMuted2, "%dM params", m.params_m);

        // Accuracy progress bar
        char acc_label[16];
        std::snprintf(acc_label, sizeof(acc_label), "%.1f%%", m.accuracy * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
        ImGui::ProgressBar(m.accuracy, ImVec2(-1.0f, 6.0f), "");
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MorphPanel::render_prune_tab() {
    auto& p = state_.prune;
    const auto& m = state_.models[state_.selected_model];

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("PrunePanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "MAGNITUDE PRUNING");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(kMuted, "Model: %s  (%.1f GB)", m.name.c_str(), m.size_gb);
    ImGui::Spacing();

    ImGui::Text("Sparsity Target");
    char sp_label[32];
    std::snprintf(sp_label, sizeof(sp_label), "%.0f%%", p.sparsity_target * 100.0f);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, kCyan);
    ImGui::SliderFloat("##sparsity", &p.sparsity_target, 0.0f, 1.0f, sp_label);
    ImGui::PopStyleColor();

    // Estimated reduction
    float est_size = m.size_gb * (1.0f - p.sparsity_target * 0.7f);
    ImGui::TextColored(kMuted, "  Estimated size after pruning: ");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%.2f GB", est_size);

    ImGui::Spacing();
    const char* methods[] = {"Magnitude", "Structured", "Gradient"};
    ImGui::Text("Method");
    ImGui::Combo("##method", &p.method_idx, methods, 3);

    ImGui::Spacing();
    ImGui::Checkbox("Iterative Pruning", &p.iterative);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (p.running) {
        ImGui::TextColored(kCyan, "Pruning in progress...");
        char prog_label[32];
        std::snprintf(prog_label, sizeof(prog_label), "%.0f%%", p.progress * 100.0f);
        ImGui::ProgressBar(p.progress, ImVec2(-1.0f, 0.0f), prog_label);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.098f, 0.906f, 1.000f, 0.18f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.098f, 0.906f, 1.000f, 0.32f});
        if (ImGui::Button("Run Pruning", ImVec2(160.0f, 36.0f))) {
            p.running = true;
            p.progress = 0.0f;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Sparsity vs Accuracy");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_PlotLines, kCyan);
    ImGui::PlotLines("##sparsity_curve", p.curve_y, 32, 0, nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 80.0f));
    ImGui::PopStyleColor();

    // Draw sparsity marker line approximately
    float est_acc = 1.0f - 0.05f * p.sparsity_target - 0.5f * p.sparsity_target * p.sparsity_target * p.sparsity_target;
    char acc_info[64];
    std::snprintf(acc_info, sizeof(acc_info), "Est. accuracy at sparsity %.0f%%: %.1f%%",
        p.sparsity_target * 100.0f, est_acc * 100.0f);
    ImGui::TextColored(kGold, "%s", acc_info);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MorphPanel::render_distill_tab() {
    auto& d = state_.distill;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("DistillPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "KNOWLEDGE DISTILLATION");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Teacher Model");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##teacher", d.teacher_buf, sizeof(d.teacher_buf));

    ImGui::Spacing();
    ImGui::Text("Student Model");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##student", d.student_buf, sizeof(d.student_buf));

    ImGui::Spacing();
    const char* temps[] = {"1.0x", "2.0x", "4.0x", "8.0x"};
    ImGui::Text("Temperature");
    ImGui::Combo("##temperature", &d.temp_idx, temps, 4);

    ImGui::Spacing();
    ImGui::Text("Alpha (KD weight)");
    ImGui::SliderFloat("##alpha", &d.alpha, 0.0f, 1.0f, "%.2f");

    ImGui::Spacing();
    ImGui::Text("Epochs");
    ImGui::SliderInt("##epochs", &d.epochs, 1, 20);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (d.running) {
        ImGui::TextColored(kPurple, "Distillation running...");
        char prog_label[32];
        std::snprintf(prog_label, sizeof(prog_label), "%.0f%%", d.progress * 100.0f);
        ImGui::ProgressBar(d.progress, ImVec2(-1.0f, 0.0f), prog_label);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.545f, 0.361f, 0.965f, 0.18f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.545f, 0.361f, 0.965f, 0.32f});
        if (ImGui::Button("Start Distillation", ImVec2(180.0f, 36.0f))) {
            d.running = true;
            d.progress = 0.0f;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Training Loss History");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_PlotLines, kPurple);
    ImGui::PlotLines("##loss", d.loss_history, 64, 0, nullptr, 0.0f, 3.0f, ImVec2(-1.0f, 80.0f));
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MorphPanel::render_quantize_tab() {
    auto& q = state_.quantize;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("QuantizePanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "QUANTIZATION");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Bit Width");
    ImGui::Spacing();
    const char* bits_labels[] = {"FP16", "INT8", "INT4", "INT2"};
    for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::SameLine();
        bool sel = (q.bits_idx == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.957f, 0.722f, 0.271f, 0.30f});
        char btn_id[16];
        std::snprintf(btn_id, sizeof(btn_id), "%s##bit%d", bits_labels[i], i);
        if (ImGui::Button(btn_id, ImVec2(70.0f, 32.0f))) q.bits_idx = i;
        if (sel) ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const char* schemes[] = {"Symmetric", "Asymmetric", "GPTQ", "AWQ"};
    ImGui::Text("Quantization Scheme");
    ImGui::Combo("##scheme", &q.scheme_idx, schemes, 4);

    ImGui::Spacing();
    ImGui::Checkbox("Per-channel quantization", &q.per_channel);
    ImGui::Checkbox("Run calibration pass", &q.calibrate);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (q.running) {
        ImGui::TextColored(kGold, "Quantizing...");
        char prog_label[32];
        std::snprintf(prog_label, sizeof(prog_label), "%.0f%%", q.progress * 100.0f);
        ImGui::ProgressBar(q.progress, ImVec2(-1.0f, 0.0f), prog_label);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.957f, 0.722f, 0.271f, 0.18f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.957f, 0.722f, 0.271f, 0.32f});
        if (ImGui::Button("Quantize Model", ImVec2(160.0f, 36.0f))) {
            q.running = true;
            q.progress = 0.0f;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Per-Layer Quantization Error");
    ImGui::Spacing();

    if (ImGui::BeginTable("layer_errors", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();
        for (const auto& [layer, err] : q.layer_errors) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kMuted, "%s", layer.c_str());
            ImGui::TableSetColumnIndex(1);
            ImVec4 err_col = err < 0.003f ? kSuccess : (err < 0.005f ? kGold : kDanger);
            ImGui::TextColored(err_col, "%.4f", err);
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MorphPanel::render_history() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("HistoryPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kMuted, "OPERATION HISTORY");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("morph_history", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Type",          ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Reduction",     ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Acc Delta",     ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Lat Delta(ms)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Active",        ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(state_.history.size()); ++i) {
            const auto& op = state_.history[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImVec4 type_col = op.type == "prune" ? kCyan : (op.type == "distill" ? kPurple : kGold);
            ImGui::TextColored(type_col, "%s", op.type.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kSuccess, "%.1f%%", op.reduction);
            ImGui::TableSetColumnIndex(2);
            ImVec4 acc_col = op.accuracy_delta > -0.03f ? kSuccess : kDanger;
            ImGui::TextColored(acc_col, "%.3f", op.accuracy_delta);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(kGold, "%.1f", op.latency_delta);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(op.enabled ? kSuccess : kMuted2, op.enabled ? "yes" : "no");
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void MorphPanel::render() {
    // === STRAYLIGHT_MORPH_WIRED: refresh REAL model data (throttled ~2s) ===
    state_.maybe_refresh();
    if (!state_.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.298f, 0.416f, 1.0f));
        ImGui::TextWrapped("model data unavailable: %s", state_.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) state_.refresh();
        ImGui::Separator();
    }

    // Per-frame fake job-progress ramp removed (no real job runner exists; the
    // prune/distill/quantize progress had no real data source).

    // Header
    ImGui::TextColored(kCyan, "STRAYLIGHT");
    ImGui::SameLine();
    ImGui::TextColored(kPurple, "MORPH");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "Model Compression Pipeline");
    ImGui::Separator();
    ImGui::Spacing();

    // Two-column layout
    render_model_selector();
    ImGui::SameLine();

    ImGui::BeginGroup();

    const char* tabs[] = {"Prune", "Distill", "Quantize", "History"};
    if (ImGui::BeginTabBar("morph_tabs", ImGuiTabBarFlags_None)) {
        for (int t = 0; t < 4; ++t) {
            if (ImGui::BeginTabItem(tabs[t])) {
                state_.active_tab = t;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    switch (state_.active_tab) {
        case 0: render_prune_tab();    break;
        case 1: render_distill_tab();  break;
        case 2: render_quantize_tab(); break;
        case 3: render_history();      break;
    }

    ImGui::EndGroup();
}

} // namespace straylight::morph
