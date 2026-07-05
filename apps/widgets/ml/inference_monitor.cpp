// apps/widgets/ml/inference_monitor.cpp
#include "inference_monitor.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::InferenceMonitorWidget, "inference_monitor", "Inference Monitor", straylight::widgets::WidgetCategory::ML);
#include <algorithm>
#include <cstdio>

namespace straylight::widgets {

void InferenceMonitorWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/agent.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void InferenceMonitorWidget::fetch_metrics() {
    if (!connected_) return;

    auto res = ipc_.command("inference.metrics");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("endpoints") || !j["endpoints"].is_array()) return;

    // Merge into existing (preserve history)
    for (auto& ej : j["endpoints"]) {
        std::string ep_name = ej.value("endpoint", "");
        InferenceEndpoint* found = nullptr;
        for (auto& e : endpoints_) {
            if (e.endpoint == ep_name) { found = &e; break; }
        }
        if (!found) {
            endpoints_.emplace_back();
            found = &endpoints_.back();
            found->endpoint = ep_name;
        }
        found->model_name = ej.value("model_name", "");
        found->rps = ej.value("rps", 0.0f);
        found->latency_p50_ms = ej.value("latency_p50_ms", 0.0f);
        found->latency_p95_ms = ej.value("latency_p95_ms", 0.0f);
        found->latency_p99_ms = ej.value("latency_p99_ms", 0.0f);
        found->batch_size = ej.value("batch_size", 1);
        found->queue_depth = ej.value("queue_depth", 0);
        found->total_requests = ej.value("total_requests", uint64_t(0));
        found->total_errors = ej.value("total_errors", uint64_t(0));
        push_history(*found);
    }
}

void InferenceMonitorWidget::push_history(InferenceEndpoint& ep) {
    int idx = ep.hist_offset % InferenceEndpoint::kHistLen;
    ep.rps_history[idx] = ep.rps;
    ep.latency_history[idx] = ep.latency_p50_ms;
    ep.hist_offset++;
}

void InferenceMonitorWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_metrics();
}

void InferenceMonitorWidget::render(bool* p_open) {
    if (!ImGui::Begin("Inference Monitor", p_open)) {
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

    ImGui::Text("Endpoints: %zu", endpoints_.size());

    if (endpoints_.empty()) {
        ImGui::TextWrapped("No inference endpoints active.");
        ImGui::End();
        return;
    }

    // Summary table
    if (ImGui::BeginTable("##inf_table", 8,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, 200))) {

        ImGui::TableSetupColumn("Endpoint");
        ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("RPS");
        ImGui::TableSetupColumn("p50 ms");
        ImGui::TableSetupColumn("p95 ms");
        ImGui::TableSetupColumn("p99 ms");
        ImGui::TableSetupColumn("Batch");
        ImGui::TableSetupColumn("Queue");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(endpoints_.size()); ++i) {
            auto& ep = endpoints_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(ep.endpoint.c_str(), selected_ == i,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                selected_ = i;
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ep.model_name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", ep.rps);
            ImGui::TableNextColumn();
            {
                ImVec4 col = (ep.latency_p50_ms > 100.0f) ? ImVec4(1, 0.3f, 0.3f, 1) :
                             (ep.latency_p50_ms > 50.0f)  ? ImVec4(1, 0.8f, 0, 1) :
                                                              ImVec4(0.4f, 1, 0.4f, 1);
                ImGui::TextColored(col, "%.1f", ep.latency_p50_ms);
            }
            ImGui::TableNextColumn(); ImGui::Text("%.1f", ep.latency_p95_ms);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", ep.latency_p99_ms);
            ImGui::TableNextColumn(); ImGui::Text("%d", ep.batch_size);
            ImGui::TableNextColumn();
            {
                ImVec4 col = (ep.queue_depth > 50) ? ImVec4(1, 0.3f, 0.3f, 1) :
                             (ep.queue_depth > 10) ? ImVec4(1, 0.8f, 0, 1) :
                                                      ImVec4(1, 1, 1, 1);
                ImGui::TextColored(col, "%d", ep.queue_depth);
            }
        }
        ImGui::EndTable();
    }

    // Detail charts for selected endpoint
    if (selected_ >= 0 && selected_ < static_cast<int>(endpoints_.size())) {
        auto& ep = endpoints_[selected_];
        ImGui::Separator();
        ImGui::Text("Detail: %s (%s)", ep.endpoint.c_str(), ep.model_name.c_str());
        ImGui::Text("Total Requests: %llu | Errors: %llu (%.2f%%)",
                    static_cast<unsigned long long>(ep.total_requests),
                    static_cast<unsigned long long>(ep.total_errors),
                    (ep.total_requests > 0) ?
                        100.0 * static_cast<double>(ep.total_errors) / static_cast<double>(ep.total_requests) : 0.0);

        int count = std::min(ep.hist_offset, InferenceEndpoint::kHistLen);
        if (count > 0) {
            // RPS history
            std::array<float, InferenceEndpoint::kHistLen> rps_plot{};
            std::array<float, InferenceEndpoint::kHistLen> lat_plot{};
            for (int j = 0; j < count; ++j) {
                int src = (ep.hist_offset - count + j) % InferenceEndpoint::kHistLen;
                rps_plot[j] = ep.rps_history[src];
                lat_plot[j] = ep.latency_history[src];
            }

            ImGui::Text("RPS History");
            float max_rps = *std::max_element(rps_plot.begin(), rps_plot.begin() + count);
            ImGui::PlotLines("##rps_hist", rps_plot.data(), count,
                             0, nullptr, 0.0f, max_rps * 1.2f, ImVec2(-1, 60));

            ImGui::Text("Latency (p50) History");
            float max_lat = *std::max_element(lat_plot.begin(), lat_plot.begin() + count);
            ImGui::PlotLines("##lat_hist", lat_plot.data(), count,
                             0, nullptr, 0.0f, max_lat * 1.2f, ImVec2(-1, 60));
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
