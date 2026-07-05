// apps/widgets/ml/model_browser.cpp
#include "model_browser.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::ModelBrowserWidget, "model_browser", "Model Browser", straylight::widgets::WidgetCategory::ML);
#include <cstdio>

namespace straylight::widgets {

std::string ModelBrowserWidget::human_params(int64_t count) {
    char buf[64];
    if (count >= 1'000'000'000LL) {
        std::snprintf(buf, sizeof(buf), "%.2fB", static_cast<double>(count) / 1e9);
    } else if (count >= 1'000'000LL) {
        std::snprintf(buf, sizeof(buf), "%.2fM", static_cast<double>(count) / 1e6);
    } else if (count >= 1'000LL) {
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(count) / 1e3);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(count));
    }
    return buf;
}

std::string ModelBrowserWidget::human_bytes(size_t bytes) {
    char buf[64];
    if (bytes >= 1ULL << 30) {
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1ULL << 30));
    } else if (bytes >= 1ULL << 20) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1ULL << 20));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / (1ULL << 10));
    }
    return buf;
}

void ModelBrowserWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/bus.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void ModelBrowserWidget::fetch_models() {
    if (!connected_) return;

    auto res = ipc_.command("model.list");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("models") || !j["models"].is_array()) return;

    models_.clear();
    for (auto& mj : j["models"]) {
        ModelInfo m;
        m.name = mj.value("name", "");
        m.path = mj.value("path", "");
        m.framework = mj.value("framework", "unknown");
        m.total_params = mj.value("total_params", int64_t(0));
        m.trainable_params = mj.value("trainable_params", int64_t(0));
        m.size_bytes = mj.value("size_bytes", size_t(0));
        m.loaded = mj.value("loaded", false);

        if (mj.contains("layers") && mj["layers"].is_array()) {
            for (auto& lj : mj["layers"]) {
                LayerInfo l;
                l.name = lj.value("name", "");
                l.type = lj.value("type", "");
                l.param_count = lj.value("param_count", int64_t(0));
                l.shape_desc = lj.value("shape", "");
                m.layers.push_back(std::move(l));
            }
        }
        models_.push_back(std::move(m));
    }
}

void ModelBrowserWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_models();
}

void ModelBrowserWidget::render(bool* p_open) {
    if (!ImGui::Begin("Model Browser", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-bus");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    ImGui::Text("Models: %zu", models_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##mfilter", "Filter...", filter_buf_, sizeof(filter_buf_));

    ImGui::Separator();

    std::string filter(filter_buf_);

    // Left pane: model list
    float list_width = 250.0f;
    ImGui::BeginChild("##model_list", ImVec2(list_width, 0), true);
    for (int i = 0; i < static_cast<int>(models_.size()); ++i) {
        auto& m = models_[i];
        if (!filter.empty() && m.name.find(filter) == std::string::npos) continue;

        char label[256];
        std::snprintf(label, sizeof(label), "%s %s (%s)###mdl%d",
                      m.loaded ? "[*]" : "[ ]",
                      m.name.c_str(),
                      human_params(m.total_params).c_str(), i);
        if (ImGui::Selectable(label, selected_model_ == i)) {
            selected_model_ = i;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right pane: model detail
    ImGui::BeginChild("##model_detail", ImVec2(0, 0), true);
    if (selected_model_ >= 0 && selected_model_ < static_cast<int>(models_.size())) {
        auto& m = models_[selected_model_];

        ImGui::Text("Model: %s", m.name.c_str());
        ImGui::TextColored(m.loaded ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                           m.loaded ? "LOADED" : "NOT LOADED");
        ImGui::Separator();

        ImGui::Text("Framework:  %s", m.framework.c_str());
        ImGui::Text("Path:       %s", m.path.c_str());
        ImGui::Text("Total Params:     %s", human_params(m.total_params).c_str());
        ImGui::Text("Trainable Params: %s", human_params(m.trainable_params).c_str());
        ImGui::Text("Disk Size:  %s", human_bytes(m.size_bytes).c_str());

        ImGui::Separator();
        ImGui::Text("Layers (%zu):", m.layers.size());

        if (ImGui::BeginTable("##layer_table", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, ImGui::GetContentRegionAvail().y))) {

            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Params");
            ImGui::TableSetupColumn("Shape");
            ImGui::TableHeadersRow();

            for (auto& l : m.layers) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.type.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(human_params(l.param_count).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.shape_desc.c_str());
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextWrapped("Select a model from the list to view details.");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace straylight::widgets
