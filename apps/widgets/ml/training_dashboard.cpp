// apps/widgets/ml/training_dashboard.cpp
#include "training_dashboard.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::TrainingDashboardWidget, "training_dashboard", "Training Dashboard", straylight::widgets::WidgetCategory::ML);
#include <algorithm>
#include <cstdio>

namespace straylight::widgets {

void TrainingDashboardWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/agent.sock");
    connected_ = res.has_value();
    if (!connected_) {
        error_msg_ = res.error();
    }
}

void TrainingDashboardWidget::fetch_runs() {
    if (!connected_) return;

    auto res = ipc_.command("training.list_runs");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("runs") || !j["runs"].is_array()) return;

    runs_.clear();
    for (auto& rj : j["runs"]) {
        TrainingRun r;
        r.run_id = rj.value("run_id", "");
        r.model_name = rj.value("model_name", "unknown");
        r.current_epoch = rj.value("current_epoch", 0);
        r.total_epochs = rj.value("total_epochs", 0);
        r.current_step = rj.value("current_step", 0);
        r.total_steps = rj.value("total_steps", 0);
        r.learning_rate = rj.value("learning_rate", 0.0f);
        r.eta_seconds = rj.value("eta_seconds", 0.0f);
        r.best_val_loss = rj.value("best_val_loss", 1e9f);
        r.best_epoch = rj.value("best_epoch", 0);

        if (rj.contains("train_loss") && rj["train_loss"].is_array()) {
            for (auto& v : rj["train_loss"]) {
                r.train_loss.push_back(v.get<float>());
            }
        }
        if (rj.contains("val_loss") && rj["val_loss"].is_array()) {
            for (auto& v : rj["val_loss"]) {
                r.val_loss.push_back(v.get<float>());
            }
        }
        runs_.push_back(std::move(r));
    }
}

void TrainingDashboardWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_runs();
}

void TrainingDashboardWidget::render(bool* p_open) {
    if (!ImGui::Begin("Training Dashboard", p_open)) {
        ImGui::End();
        return;
    }

    // Connection status
    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-agent");
        if (!error_msg_.empty()) {
            ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        }
        if (ImGui::Button("Retry Connection")) {
            try_connect();
        }
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Connected to straylight-agent");
    ImGui::SameLine();
    ImGui::Text("| %zu active run(s)", runs_.size());

    if (runs_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("No active training runs. Start a run via straylight-agent to see live metrics.");
        ImGui::End();
        return;
    }

    // Run selector
    if (ImGui::BeginTabBar("##training_tabs")) {
        for (int i = 0; i < static_cast<int>(runs_.size()); ++i) {
            char label[128];
            std::snprintf(label, sizeof(label), "%s###run_%d",
                          runs_[i].model_name.c_str(), i);
            if (ImGui::BeginTabItem(label)) {
                selected_run_ = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    auto& run = runs_[selected_run_];

    // Progress section
    ImGui::Separator();
    ImGui::Text("Run ID: %s", run.run_id.c_str());

    // Epoch progress
    ImGui::Text("Epoch");
    ImGui::SameLine(100);
    char epoch_overlay[64];
    std::snprintf(epoch_overlay, sizeof(epoch_overlay), "%d / %d",
                  run.current_epoch, run.total_epochs);
    float epoch_frac = (run.total_epochs > 0)
        ? static_cast<float>(run.current_epoch) / static_cast<float>(run.total_epochs) : 0.0f;
    ImGui::ProgressBar(epoch_frac, ImVec2(-1, 0), epoch_overlay);

    // Step progress
    ImGui::Text("Step");
    ImGui::SameLine(100);
    char step_overlay[64];
    std::snprintf(step_overlay, sizeof(step_overlay), "%d / %d",
                  run.current_step, run.total_steps);
    float step_frac = (run.total_steps > 0)
        ? static_cast<float>(run.current_step) / static_cast<float>(run.total_steps) : 0.0f;
    ImGui::ProgressBar(step_frac, ImVec2(-1, 0), step_overlay);

    // ETA
    int eta_h = static_cast<int>(run.eta_seconds) / 3600;
    int eta_m = (static_cast<int>(run.eta_seconds) % 3600) / 60;
    int eta_s = static_cast<int>(run.eta_seconds) % 60;
    ImGui::Text("ETA");
    ImGui::SameLine(100);
    ImGui::Text("%02d:%02d:%02d", eta_h, eta_m, eta_s);

    ImGui::Text("Learning Rate");
    ImGui::SameLine(100);
    ImGui::Text("%.2e", run.learning_rate);

    ImGui::Text("Best Val Loss");
    ImGui::SameLine(100);
    ImGui::Text("%.6f (epoch %d)", run.best_val_loss, run.best_epoch);

    // Loss curves
    ImGui::Separator();
    ImGui::Text("Loss Curves");

    if (!run.train_loss.empty()) {
        float min_loss = *std::min_element(run.train_loss.begin(), run.train_loss.end());
        float max_loss = *std::max_element(run.train_loss.begin(), run.train_loss.end());
        if (!run.val_loss.empty()) {
            min_loss = std::min(min_loss, *std::min_element(run.val_loss.begin(), run.val_loss.end()));
            max_loss = std::max(max_loss, *std::max_element(run.val_loss.begin(), run.val_loss.end()));
        }
        float padding = (max_loss - min_loss) * 0.1f;
        if (padding < 0.001f) padding = 0.001f;

        ImGui::Text("Train Loss");
        ImGui::PlotLines("##train_loss", run.train_loss.data(),
                         static_cast<int>(run.train_loss.size()),
                         0, nullptr, min_loss - padding, max_loss + padding,
                         ImVec2(-1, 80));

        if (!run.val_loss.empty()) {
            ImGui::Text("Val Loss");
            ImGui::PlotLines("##val_loss", run.val_loss.data(),
                             static_cast<int>(run.val_loss.size()),
                             0, nullptr, min_loss - padding, max_loss + padding,
                             ImVec2(-1, 80));
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
