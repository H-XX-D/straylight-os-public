// apps/widgets/research/experiment_tracker.cpp
#include "experiment_tracker.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::ExperimentTrackerWidget, "experiment_tracker", "Experiment Tracker", straylight::widgets::WidgetCategory::Research);
#include <cstdio>
#include <algorithm>

namespace straylight::widgets {

ImVec4 ExperimentTrackerWidget::status_color(const std::string& s) {
    if (s == "running")   return ImVec4(0.3f, 0.9f, 0.3f, 1);
    if (s == "completed") return ImVec4(0.3f, 0.6f, 1, 1);
    if (s == "failed")    return ImVec4(1, 0.3f, 0.3f, 1);
    return ImVec4(0.6f, 0.6f, 0.6f, 1);
}

void ExperimentTrackerWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/agent.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void ExperimentTrackerWidget::fetch_experiments() {
    if (!connected_) return;

    auto res = ipc_.command("experiment.list");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("experiments") || !j["experiments"].is_array()) return;

    experiments_.clear();
    for (auto& ej : j["experiments"]) {
        Experiment e;
        e.id = ej.value("id", "");
        e.name = ej.value("name", "");
        e.status = ej.value("status", "unknown");
        e.start_time = ej.value("start_time", "");
        e.end_time = ej.value("end_time", "");
        e.duration_sec = ej.value("duration_sec", 0.0f);
        e.notes = ej.value("notes", "");
        e.tags = ej.value("tags", "");

        if (ej.contains("hyperparams") && ej["hyperparams"].is_object()) {
            for (auto& [k, v] : ej["hyperparams"].items()) {
                e.hyperparams.push_back({k, v.dump()});
            }
        }

        if (ej.contains("metrics") && ej["metrics"].is_object()) {
            for (auto& [k, v] : ej["metrics"].items()) {
                MetricSeries ms;
                ms.name = k;
                if (v.is_array()) {
                    for (auto& val : v) ms.values.push_back(val.get<float>());
                }
                e.metrics.push_back(std::move(ms));
            }
        }

        experiments_.push_back(std::move(e));
    }
}

void ExperimentTrackerWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_experiments();
}

void ExperimentTrackerWidget::render(bool* p_open) {
    if (!ImGui::Begin("Experiment Tracker", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-agent");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    ImGui::Text("Experiments: %zu", experiments_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##efilter", "Filter...", filter_buf_, sizeof(filter_buf_));
    ImGui::SameLine();
    ImGui::Checkbox("Compare Mode", &show_comparison_);

    ImGui::Separator();
    std::string filter(filter_buf_);

    // Left: experiment list
    float list_w = 280.0f;
    ImGui::BeginChild("##exp_list", ImVec2(list_w, 0), true);
    for (int i = 0; i < static_cast<int>(experiments_.size()); ++i) {
        auto& e = experiments_[i];
        if (!filter.empty() && e.name.find(filter) == std::string::npos) continue;

        char lbl[256];
        std::snprintf(lbl, sizeof(lbl), "%s [%s]###exp%d", e.name.c_str(), e.status.c_str(), i);

        bool selected = (selected_exp_ == i);
        if (show_comparison_ && compare_exp_ == i) selected = true;

        ImGui::PushStyleColor(ImGuiCol_Text, status_color(e.status));
        if (ImGui::Selectable(lbl, selected)) {
            if (show_comparison_ && selected_exp_ >= 0 && selected_exp_ != i) {
                compare_exp_ = i;
            } else {
                selected_exp_ = i;
                compare_exp_ = -1;
            }
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: detail
    ImGui::BeginChild("##exp_detail", ImVec2(0, 0), true);
    if (selected_exp_ >= 0 && selected_exp_ < static_cast<int>(experiments_.size())) {
        auto& e = experiments_[selected_exp_];

        ImGui::Text("Experiment: %s", e.name.c_str());
        ImGui::Text("ID: %s", e.id.c_str());
        ImGui::TextColored(status_color(e.status), "Status: %s", e.status.c_str());
        ImGui::Text("Duration: %.1f s", e.duration_sec);
        ImGui::Text("Tags: %s", e.tags.c_str());

        // Hyperparameters
        if (!e.hyperparams.empty()) {
            ImGui::Separator();
            ImGui::Text("Hyperparameters:");
            if (ImGui::BeginTable("##hp_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 0))) {
                ImGui::TableSetupColumn("Key");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();
                for (auto& hp : e.hyperparams) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(hp.key.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(hp.value.c_str());
                }
                ImGui::EndTable();
            }

            // Comparison table
            if (show_comparison_ && compare_exp_ >= 0 && compare_exp_ < static_cast<int>(experiments_.size())) {
                auto& ce = experiments_[compare_exp_];
                ImGui::Separator();
                ImGui::Text("Comparison with: %s", ce.name.c_str());

                if (ImGui::BeginTable("##hp_cmp", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Param");
                    ImGui::TableSetupColumn(e.name.c_str());
                    ImGui::TableSetupColumn(ce.name.c_str());
                    ImGui::TableHeadersRow();

                    // Collect all keys
                    std::map<std::string, std::pair<std::string, std::string>> merged;
                    for (auto& hp : e.hyperparams) merged[hp.key].first = hp.value;
                    for (auto& hp : ce.hyperparams) merged[hp.key].second = hp.value;

                    for (auto& [key, vals] : merged) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(key.c_str());
                        ImGui::TableNextColumn();
                        ImVec4 col = (vals.first != vals.second) ? ImVec4(1, 0.8f, 0, 1) : ImVec4(1, 1, 1, 1);
                        ImGui::TextColored(col, "%s", vals.first.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextColored(col, "%s", vals.second.c_str());
                    }
                    ImGui::EndTable();
                }
            }
        }

        // Metrics plots
        if (!e.metrics.empty()) {
            ImGui::Separator();
            ImGui::Text("Metrics:");
            for (auto& ms : e.metrics) {
                if (ms.values.empty()) continue;
                float min_v = *std::min_element(ms.values.begin(), ms.values.end());
                float max_v = *std::max_element(ms.values.begin(), ms.values.end());
                float pad = (max_v - min_v) * 0.1f;
                if (pad < 0.001f) pad = 0.001f;

                char label[128];
                std::snprintf(label, sizeof(label), "%s (last: %.4f)###%s",
                              ms.name.c_str(), ms.values.back(), ms.name.c_str());
                ImGui::PlotLines(label, ms.values.data(), static_cast<int>(ms.values.size()),
                                 0, nullptr, min_v - pad, max_v + pad, ImVec2(-1, 60));
            }
        }

        // Notes
        if (!e.notes.empty()) {
            ImGui::Separator();
            ImGui::Text("Notes:");
            ImGui::TextWrapped("%s", e.notes.c_str());
        }
    } else {
        ImGui::TextWrapped("Select an experiment to view details.");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace straylight::widgets
