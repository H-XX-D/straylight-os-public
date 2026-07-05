// apps/widgets/hpc/job_queue.cpp
#include "job_queue.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::JobQueueWidget, "job_queue", "Job Queue", straylight::widgets::WidgetCategory::HPC);
#include <cstdio>

namespace straylight::widgets {

ImVec4 JobQueueWidget::state_color(JobState s) {
    switch (s) {
        case JobState::Pending:   return ImVec4(0.7f, 0.7f, 0.7f, 1);
        case JobState::Running:   return ImVec4(0.3f, 0.9f, 0.3f, 1);
        case JobState::Completed: return ImVec4(0.3f, 0.6f, 1, 1);
        case JobState::Failed:    return ImVec4(1, 0.3f, 0.3f, 1);
        case JobState::Cancelled: return ImVec4(0.6f, 0.4f, 0.1f, 1);
    }
    return ImVec4(1, 1, 1, 1);
}

const char* JobQueueWidget::state_str(JobState s) {
    switch (s) {
        case JobState::Pending:   return "Pending";
        case JobState::Running:   return "Running";
        case JobState::Completed: return "Completed";
        case JobState::Failed:    return "Failed";
        case JobState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

std::string JobQueueWidget::format_duration(float seconds) {
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    int s = static_cast<int>(seconds) % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

void JobQueueWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/agent.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void JobQueueWidget::fetch_jobs() {
    if (!connected_) return;

    auto res = ipc_.command("job.list");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("jobs") || !j["jobs"].is_array()) return;

    jobs_.clear();
    for (auto& jj : j["jobs"]) {
        JobEntry je;
        je.job_id = jj.value("job_id", "");
        je.name = jj.value("name", "");
        je.user = jj.value("user", "");
        je.state = static_cast<JobState>(jj.value("state", 0));
        je.gpus_requested = jj.value("gpus", 0);
        je.cpus_requested = jj.value("cpus", 0);
        je.mem_gb_requested = jj.value("mem_gb", 0.0f);
        je.node = jj.value("node", "");
        je.progress_pct = jj.value("progress", 0.0f);
        je.elapsed_sec = jj.value("elapsed_sec", 0.0f);
        je.eta_sec = jj.value("eta_sec", 0.0f);
        je.submit_time = jj.value("submit_time", "");
        je.start_time = jj.value("start_time", "");
        je.priority = jj.value("priority", 0);
        jobs_.push_back(std::move(je));
    }
}

void JobQueueWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_jobs();
}

void JobQueueWidget::render(bool* p_open) {
    if (!ImGui::Begin("Job Queue", p_open)) {
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

    // Summary counts
    int pending = 0, running = 0, completed = 0, failed = 0;
    for (auto& j : jobs_) {
        switch (j.state) {
            case JobState::Pending:   pending++;   break;
            case JobState::Running:   running++;   break;
            case JobState::Completed: completed++; break;
            case JobState::Failed:    failed++;    break;
            default: break;
        }
    }
    ImGui::Text("P:%d R:%d C:%d F:%d | Total:%zu", pending, running, completed, failed, jobs_.size());

    // Filter controls
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    const char* filter_labels[] = { "All", "Pending", "Running", "Completed", "Failed", "Cancelled" };
    ImGui::Combo("##state_filter", &filter_state_, filter_labels, 6);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##jfilter", "Filter name...", filter_buf_, sizeof(filter_buf_));

    ImGui::Separator();

    std::string name_filter(filter_buf_);

    // Job table
    if (ImGui::BeginTable("##job_table", 9,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
            ImVec2(0, ImGui::GetContentRegionAvail().y - 100))) {

        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("User");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("GPUs");
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Progress");
        ImGui::TableSetupColumn("Elapsed");
        ImGui::TableSetupColumn("Priority");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(jobs_.size()); ++i) {
            auto& je = jobs_[i];
            // Apply filters
            if (filter_state_ > 0 && static_cast<int>(je.state) != (filter_state_ - 1)) continue;
            if (!name_filter.empty() && je.name.find(name_filter) == std::string::npos) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(je.job_id.c_str(), selected_job_ == i,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                selected_job_ = i;
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(je.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(je.user.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(state_color(je.state), "%s", state_str(je.state));
            ImGui::TableNextColumn(); ImGui::Text("%d", je.gpus_requested);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(je.node.c_str());
            ImGui::TableNextColumn();
            if (je.state == JobState::Running) {
                char ov[16]; std::snprintf(ov, sizeof(ov), "%.0f%%", je.progress_pct);
                ImGui::ProgressBar(je.progress_pct / 100.0f, ImVec2(-1, 0), ov);
            } else {
                ImGui::Text("%.0f%%", je.progress_pct);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_duration(je.elapsed_sec).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", je.priority);
        }
        ImGui::EndTable();
    }

    // Detail for selected job
    if (selected_job_ >= 0 && selected_job_ < static_cast<int>(jobs_.size())) {
        ImGui::Separator();
        auto& je = jobs_[selected_job_];
        ImGui::Text("Job: %s | %s", je.job_id.c_str(), je.name.c_str());
        ImGui::Text("Resources: %d GPUs, %d CPUs, %.1f GiB", je.gpus_requested, je.cpus_requested, je.mem_gb_requested);
        ImGui::Text("Submitted: %s | Started: %s", je.submit_time.c_str(), je.start_time.c_str());
        if (je.state == JobState::Running && je.eta_sec > 0) {
            ImGui::Text("ETA: %s", format_duration(je.eta_sec).c_str());
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
