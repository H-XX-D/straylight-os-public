// apps/widgets/system/scheduler_view.cpp
#include "scheduler_view.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::SchedulerViewWidget, "scheduler_view", "Scheduler View", straylight::widgets::WidgetCategory::System);
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace straylight::widgets {

std::string SchedulerViewWidget::human_bytes(int64_t bytes) {
    char buf[64];
    if (bytes < 0) return "unlimited";
    if (bytes >= (1LL << 30)) std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1LL << 30));
    else if (bytes >= (1LL << 20)) std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1LL << 20));
    else if (bytes >= (1LL << 10)) std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / (1LL << 10));
    else std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    return buf;
}

void SchedulerViewWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/scheduler.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

static std::string read_text_file(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void SchedulerViewWidget::fetch_kernel_scheduler() {
    kernel_.online = false;
    kernel_.status_text.clear();
    kernel_.policy_text.clear();
    kernel_.task_lines.clear();

    kernel_.status_text = read_text_file("/proc/straylight/sched/status");
    if (kernel_.status_text.empty()) return;

    kernel_.online = true;
    kernel_.policy_text = read_text_file("/proc/straylight/sched/policy");

    std::ifstream tasks("/proc/straylight/sched/tasks");
    std::string line;
    while (std::getline(tasks, line) && kernel_.task_lines.size() < 200) {
        if (!line.empty()) kernel_.task_lines.push_back(line);
    }
}

void SchedulerViewWidget::fetch_cgroups() {
    if (!connected_) return;

    auto res = ipc_.command("scheduler.cgroups");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("cgroups") || !j["cgroups"].is_array()) return;

    cgroups_.clear();
    for (auto& cj : j["cgroups"]) {
        CgroupInfo ci;
        ci.path = cj.value("path", "");
        ci.name = cj.value("name", "");
        ci.depth = cj.value("depth", 0);
        ci.nr_tasks = cj.value("nr_tasks", 0);
        ci.cpu_usage_pct = cj.value("cpu_usage_pct", 0.0f);
        ci.memory_current = cj.value("memory_current", int64_t(0));
        ci.memory_max = cj.value("memory_max", int64_t(-1));
        ci.cpu_set = cj.value("cpu_set", "");
        ci.io_read_bytes = cj.value("io_read_bytes", int64_t(0));
        ci.io_write_bytes = cj.value("io_write_bytes", int64_t(0));
        cgroups_.push_back(std::move(ci));
    }
}

void SchedulerViewWidget::fetch_tasks() {
    if (!connected_) return;

    auto res = ipc_.command("scheduler.tasks");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("tasks") || !j["tasks"].is_array()) return;

    tasks_.clear();
    for (auto& tj : j["tasks"]) {
        TaskPlacement tp;
        tp.pid = tj.value("pid", 0);
        tp.comm = tj.value("comm", "");
        tp.cpu = tj.value("cpu", -1);
        tp.numa_node = tj.value("numa_node", -1);
        tp.cgroup = tj.value("cgroup", "");
        tp.sched_policy = tj.value("sched_policy", "normal");
        tp.priority = tj.value("priority", 0);
        tp.cpu_pct = tj.value("cpu_pct", 0.0f);
        tasks_.push_back(std::move(tp));
    }
}

void SchedulerViewWidget::update() {
    if (!should_update()) return;
    fetch_kernel_scheduler();
    if (!connected_) try_connect();
    if (connected_) {
        fetch_cgroups();
        fetch_tasks();
    }
}

void SchedulerViewWidget::render(bool* p_open) {
    if (!ImGui::Begin("Scheduler View", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_ && !kernel_.online) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-scheduler");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##sched_tabs")) {
        if (kernel_.online && ImGui::BeginTabItem("Kernel Module")) {
            ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "/proc/straylight/sched online");
            ImGui::Separator();
            ImGui::TextUnformatted(kernel_.status_text.c_str());
            if (!kernel_.policy_text.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(kernel_.policy_text.c_str());
            }
            ImGui::EndTabItem();
        }

        if (kernel_.online && ImGui::BeginTabItem("Kernel Tasks")) {
            ImGui::Text("%zu classified tasks", kernel_.task_lines.size());
            if (ImGui::BeginChild("##kernel_tasks", ImVec2(0, 0), true)) {
                for (const auto& line : kernel_.task_lines) {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // Cgroup hierarchy tab
        if (connected_ && ImGui::BeginTabItem("Cgroup Hierarchy")) {
            view_tab_ = 0;

            if (ImGui::BeginTable("##cgroup_table", 7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                    ImVec2(0, ImGui::GetContentRegionAvail().y))) {

                ImGui::TableSetupColumn("Cgroup");
                ImGui::TableSetupColumn("Tasks");
                ImGui::TableSetupColumn("CPU %");
                ImGui::TableSetupColumn("Memory");
                ImGui::TableSetupColumn("Mem Limit");
                ImGui::TableSetupColumn("CPUSet");
                ImGui::TableSetupColumn("I/O (R/W)");
                ImGui::TableHeadersRow();

                for (auto& cg : cgroups_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // Indent by depth
                    std::string indent(cg.depth * 2, ' ');
                    ImGui::Text("%s%s", indent.c_str(), cg.name.c_str());
                    if (ImGui::IsItemHovered() && !cg.path.empty()) {
                        ImGui::SetTooltip("%s", cg.path.c_str());
                    }
                    ImGui::TableNextColumn(); ImGui::Text("%d", cg.nr_tasks);
                    ImGui::TableNextColumn();
                    {
                        ImVec4 col = (cg.cpu_usage_pct > 80) ? ImVec4(1, 0.3f, 0.3f, 1) :
                                     (cg.cpu_usage_pct > 50) ? ImVec4(1, 0.8f, 0, 1) : ImVec4(1, 1, 1, 1);
                        ImGui::TextColored(col, "%.1f%%", cg.cpu_usage_pct);
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(human_bytes(cg.memory_current).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(human_bytes(cg.memory_max).c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cg.cpu_set.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s / %s",
                                human_bytes(cg.io_read_bytes).c_str(),
                                human_bytes(cg.io_write_bytes).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // Task placement tab
        if (connected_ && ImGui::BeginTabItem("Task Placement")) {
            view_tab_ = 1;

            ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##tfilter", "Filter by comm...", task_filter_, sizeof(task_filter_));
            ImGui::SameLine();
            ImGui::Text("%zu tasks", tasks_.size());

            std::string filter(task_filter_);

            if (ImGui::BeginTable("##task_table", 7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable,
                    ImVec2(0, ImGui::GetContentRegionAvail().y))) {

                ImGui::TableSetupColumn("PID");
                ImGui::TableSetupColumn("Comm");
                ImGui::TableSetupColumn("CPU");
                ImGui::TableSetupColumn("NUMA");
                ImGui::TableSetupColumn("Policy");
                ImGui::TableSetupColumn("Prio");
                ImGui::TableSetupColumn("CPU %");
                ImGui::TableHeadersRow();

                for (auto& t : tasks_) {
                    if (!filter.empty() && t.comm.find(filter) == std::string::npos) continue;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", t.pid);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(t.comm.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", t.cpu);
                    ImGui::TableNextColumn(); ImGui::Text("%d", t.numa_node);
                    ImGui::TableNextColumn();
                    {
                        ImVec4 col = (t.sched_policy == "fifo" || t.sched_policy == "rr")
                            ? ImVec4(1, 0.8f, 0.3f, 1) : ImVec4(1, 1, 1, 1);
                        ImGui::TextColored(col, "%s", t.sched_policy.c_str());
                    }
                    ImGui::TableNextColumn(); ImGui::Text("%d", t.priority);
                    ImGui::TableNextColumn();
                    {
                        float f = t.cpu_pct / 100.0f;
                        char ov[16]; std::snprintf(ov, sizeof(ov), "%.1f%%", t.cpu_pct);
                        ImGui::ProgressBar(f, ImVec2(60, 0), ov);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace straylight::widgets
