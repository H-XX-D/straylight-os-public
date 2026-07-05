// apps/widgets/research/dataset_browser.cpp
#include "dataset_browser.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::DatasetBrowserWidget, "dataset_browser", "Dataset Browser", straylight::widgets::WidgetCategory::Research);
#include <cstdio>
#include <algorithm>

namespace straylight::widgets {

std::string DatasetBrowserWidget::human_bytes(size_t bytes) {
    char buf[64];
    if (bytes >= 1ULL << 30) std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1ULL << 30));
    else if (bytes >= 1ULL << 20) std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1ULL << 20));
    else if (bytes >= 1ULL << 10) std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / (1ULL << 10));
    else std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    return buf;
}

void DatasetBrowserWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/agent.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void DatasetBrowserWidget::fetch_datasets() {
    if (!connected_) return;

    auto res = ipc_.command("dataset.list");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("datasets") || !j["datasets"].is_array()) return;

    datasets_.clear();
    for (auto& dj : j["datasets"]) {
        DatasetInfo ds;
        ds.name = dj.value("name", "");
        ds.path = dj.value("path", "");
        ds.format = dj.value("format", "unknown");
        ds.num_rows = dj.value("num_rows", int64_t(0));
        ds.num_cols = dj.value("num_cols", int64_t(0));
        ds.size_bytes = dj.value("size_bytes", size_t(0));

        if (dj.contains("columns") && dj["columns"].is_array()) {
            for (auto& cj : dj["columns"]) {
                ColumnStats cs;
                cs.name = cj.value("name", "");
                cs.dtype = cj.value("dtype", "");
                cs.mean = cj.value("mean", 0.0f);
                cs.std_dev = cj.value("std_dev", 0.0f);
                cs.min_val = cj.value("min_val", 0.0f);
                cs.max_val = cj.value("max_val", 0.0f);
                cs.null_count = cj.value("null_count", 0);
                cs.unique_count = cj.value("unique_count", 0);
                if (cj.contains("histogram") && cj["histogram"].is_array()) {
                    for (auto& v : cj["histogram"]) cs.histogram.push_back(v.get<float>());
                }
                ds.columns.push_back(std::move(cs));
            }
        }

        if (dj.contains("sample_rows") && dj["sample_rows"].is_array()) {
            for (auto& rj : dj["sample_rows"]) {
                std::vector<std::string> row;
                if (rj.is_array()) {
                    for (auto& v : rj) row.push_back(v.is_string() ? v.get<std::string>() : v.dump());
                }
                ds.sample_rows.push_back(std::move(row));
            }
        }

        datasets_.push_back(std::move(ds));
    }
}

void DatasetBrowserWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_datasets();
}

void DatasetBrowserWidget::render(bool* p_open) {
    if (!ImGui::Begin("Dataset Browser", p_open)) {
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

    ImGui::Text("Datasets: %zu", datasets_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##dfilter", "Filter...", filter_buf_, sizeof(filter_buf_));

    ImGui::Separator();
    std::string filter(filter_buf_);

    // Left: dataset list
    float list_w = 220.0f;
    ImGui::BeginChild("##ds_list", ImVec2(list_w, 0), true);
    for (int i = 0; i < static_cast<int>(datasets_.size()); ++i) {
        auto& ds = datasets_[i];
        if (!filter.empty() && ds.name.find(filter) == std::string::npos) continue;

        char lbl[256];
        std::snprintf(lbl, sizeof(lbl), "%s (%s)###ds%d", ds.name.c_str(), ds.format.c_str(), i);
        if (ImGui::Selectable(lbl, selected_ds_ == i)) {
            selected_ds_ = i;
            selected_col_ = -1;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: detail
    ImGui::BeginChild("##ds_detail", ImVec2(0, 0), true);
    if (selected_ds_ >= 0 && selected_ds_ < static_cast<int>(datasets_.size())) {
        auto& ds = datasets_[selected_ds_];

        ImGui::Text("Dataset: %s", ds.name.c_str());
        ImGui::Text("Path: %s", ds.path.c_str());
        ImGui::Text("Format: %s | Size: %s", ds.format.c_str(), human_bytes(ds.size_bytes).c_str());
        ImGui::Text("Rows: %lld | Columns: %lld",
                    static_cast<long long>(ds.num_rows), static_cast<long long>(ds.num_cols));

        if (ImGui::BeginTabBar("##ds_tabs")) {
            // Column stats tab
            if (ImGui::BeginTabItem("Statistics")) {
                view_tab_ = 1;

                if (ImGui::BeginTable("##col_table", 7,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                        ImVec2(0, 200))) {

                    ImGui::TableSetupColumn("Column");
                    ImGui::TableSetupColumn("Type");
                    ImGui::TableSetupColumn("Mean");
                    ImGui::TableSetupColumn("Std");
                    ImGui::TableSetupColumn("Min");
                    ImGui::TableSetupColumn("Max");
                    ImGui::TableSetupColumn("Nulls");
                    ImGui::TableHeadersRow();

                    for (int ci = 0; ci < static_cast<int>(ds.columns.size()); ++ci) {
                        auto& cs = ds.columns[ci];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Selectable(cs.name.c_str(), selected_col_ == ci,
                                ImGuiSelectableFlags_SpanAllColumns)) {
                            selected_col_ = ci;
                        }
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(cs.dtype.c_str());
                        ImGui::TableNextColumn(); ImGui::Text("%.4f", cs.mean);
                        ImGui::TableNextColumn(); ImGui::Text("%.4f", cs.std_dev);
                        ImGui::TableNextColumn(); ImGui::Text("%.4f", cs.min_val);
                        ImGui::TableNextColumn(); ImGui::Text("%.4f", cs.max_val);
                        ImGui::TableNextColumn(); ImGui::Text("%d", cs.null_count);
                    }
                    ImGui::EndTable();
                }

                // Histogram for selected column
                if (selected_col_ >= 0 && selected_col_ < static_cast<int>(ds.columns.size())) {
                    auto& cs = ds.columns[selected_col_];
                    if (!cs.histogram.empty()) {
                        ImGui::Separator();
                        ImGui::Text("Histogram: %s", cs.name.c_str());
                        float max_h = *std::max_element(cs.histogram.begin(), cs.histogram.end());
                        ImGui::PlotHistogram("##col_hist", cs.histogram.data(),
                                             static_cast<int>(cs.histogram.size()),
                                             0, nullptr, 0.0f, max_h * 1.1f, ImVec2(-1, 80));
                    }
                }
                ImGui::EndTabItem();
            }

            // Sample preview tab
            if (ImGui::BeginTabItem("Sample Preview")) {
                view_tab_ = 2;

                if (!ds.sample_rows.empty() && !ds.columns.empty()) {
                    int ncols = static_cast<int>(ds.columns.size());
                    if (ImGui::BeginTable("##sample_table", ncols,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                            ImVec2(0, ImGui::GetContentRegionAvail().y))) {

                        for (auto& cs : ds.columns) {
                            ImGui::TableSetupColumn(cs.name.c_str());
                        }
                        ImGui::TableHeadersRow();

                        for (auto& row : ds.sample_rows) {
                            ImGui::TableNextRow();
                            for (int ci = 0; ci < ncols && ci < static_cast<int>(row.size()); ++ci) {
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted(row[ci].c_str());
                            }
                        }
                        ImGui::EndTable();
                    }
                } else {
                    ImGui::TextWrapped("No sample data available.");
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    } else {
        ImGui::TextWrapped("Select a dataset from the list.");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace straylight::widgets
