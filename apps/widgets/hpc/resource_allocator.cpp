// apps/widgets/hpc/resource_allocator.cpp
#include "resource_allocator.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::ResourceAllocatorWidget, "resource_allocator", "Resource Allocator", straylight::widgets::WidgetCategory::HPC);
#include <cstdio>

namespace straylight::widgets {

void ResourceAllocatorWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/rhem.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void ResourceAllocatorWidget::fetch_allocations() {
    if (!connected_) return;

    auto res = ipc_.command("resource.allocations");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();

    // GPU allocations
    gpu_allocs_.clear();
    if (j.contains("gpus") && j["gpus"].is_array()) {
        for (auto& gj : j["gpus"]) {
            GpuAllocation ga;
            ga.gpu_id = gj.value("gpu_id", 0);
            ga.device_name = gj.value("device_name", "");
            ga.assigned_job = gj.value("assigned_job", "");
            ga.assigned_user = gj.value("assigned_user", "");
            ga.util_pct = gj.value("util_pct", 0.0f);
            ga.vram_used_mb = gj.value("vram_used_mb", 0.0f);
            ga.vram_total_mb = gj.value("vram_total_mb", 0.0f);
            ga.reserved = gj.value("reserved", false);
            gpu_allocs_.push_back(std::move(ga));
        }
    }

    // CPU allocations
    cpu_allocs_.clear();
    if (j.contains("cpus") && j["cpus"].is_array()) {
        for (auto& cj : j["cpus"]) {
            CpuAllocation ca;
            ca.cpu_id = cj.value("cpu_id", 0);
            ca.numa_node = cj.value("numa_node", 0);
            ca.assigned_job = cj.value("assigned_job", "");
            ca.util_pct = cj.value("util_pct", 0.0f);
            ca.reserved = cj.value("reserved", false);
            cpu_allocs_.push_back(std::move(ca));
        }
    }

    // Memory allocations
    mem_allocs_.clear();
    if (j.contains("memory") && j["memory"].is_array()) {
        for (auto& mj : j["memory"]) {
            MemAllocation ma;
            ma.numa_node = mj.value("numa_node", 0);
            ma.used_gb = mj.value("used_gb", 0.0f);
            ma.total_gb = mj.value("total_gb", 0.0f);
            ma.assigned_job = mj.value("assigned_job", "");
            mem_allocs_.push_back(std::move(ma));
        }
    }
}

void ResourceAllocatorWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_allocations();
}

void ResourceAllocatorWidget::render(bool* p_open) {
    if (!ImGui::Begin("Resource Allocator", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-rhem");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##res_tabs")) {
        // GPU tab
        if (ImGui::BeginTabItem("GPUs")) {
            view_tab_ = 0;

            int alloc_count = 0;
            for (auto& g : gpu_allocs_) { if (!g.assigned_job.empty()) alloc_count++; }
            ImGui::Text("Allocated: %d / %zu", alloc_count, gpu_allocs_.size());

            if (ImGui::BeginTable("##gpu_alloc_table", 6,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("GPU");
                ImGui::TableSetupColumn("Device");
                ImGui::TableSetupColumn("Job");
                ImGui::TableSetupColumn("User");
                ImGui::TableSetupColumn("Util");
                ImGui::TableSetupColumn("VRAM");
                ImGui::TableHeadersRow();

                for (auto& g : gpu_allocs_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", g.gpu_id);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(g.device_name.c_str());
                    ImGui::TableNextColumn();
                    if (g.assigned_job.empty()) {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "(free)");
                    } else {
                        ImGui::TextUnformatted(g.assigned_job.c_str());
                    }
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(g.assigned_user.c_str());
                    ImGui::TableNextColumn();
                    char ov[16]; std::snprintf(ov, sizeof(ov), "%.0f%%", g.util_pct);
                    ImGui::ProgressBar(g.util_pct / 100.0f, ImVec2(80, 0), ov);
                    ImGui::TableNextColumn();
                    std::snprintf(ov, sizeof(ov), "%.0f/%.0f", g.vram_used_mb, g.vram_total_mb);
                    float frac = (g.vram_total_mb > 0) ? g.vram_used_mb / g.vram_total_mb : 0.0f;
                    ImGui::ProgressBar(frac, ImVec2(100, 0), ov);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // CPU tab
        if (ImGui::BeginTabItem("CPUs")) {
            view_tab_ = 1;

            int alloc_count = 0;
            for (auto& c : cpu_allocs_) { if (!c.assigned_job.empty()) alloc_count++; }
            ImGui::Text("Allocated: %d / %zu cores", alloc_count, cpu_allocs_.size());

            // Show as grid colored by utilization
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 origin = ImGui::GetCursorScreenPos();
            float cell = 16.0f;
            int cols = 32;
            for (int i = 0; i < static_cast<int>(cpu_allocs_.size()); ++i) {
                auto& c = cpu_allocs_[i];
                int col = i % cols;
                int row = i / cols;
                ImVec2 tl(origin.x + col * (cell + 2), origin.y + row * (cell + 2));
                ImVec2 br(tl.x + cell, tl.y + cell);

                float t = c.util_pct / 100.0f;
                ImVec4 color(t, 1.0f - t, 0.1f, 0.9f);
                if (!c.assigned_job.empty()) {
                    color.w = 1.0f;
                }
                dl->AddRectFilled(tl, br, ImGui::GetColorU32(color), 2.0f);

                // Tooltip on hover
                ImGui::SetCursorScreenPos(tl);
                char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##cpu%d", i);
                ImGui::InvisibleButton(btn_id, ImVec2(cell, cell));
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("CPU %d (NUMA %d)", c.cpu_id, c.numa_node);
                    ImGui::Text("Util: %.0f%%", c.util_pct);
                    ImGui::Text("Job: %s", c.assigned_job.empty() ? "(none)" : c.assigned_job.c_str());
                    ImGui::EndTooltip();
                }
            }
            int total_rows = (static_cast<int>(cpu_allocs_.size()) + cols - 1) / cols;
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + total_rows * (cell + 2) + 4));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::EndTabItem();
        }

        // Memory tab
        if (ImGui::BeginTabItem("Memory")) {
            view_tab_ = 2;

            if (ImGui::BeginTable("##mem_alloc_table", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("NUMA Node");
                ImGui::TableSetupColumn("Used / Total");
                ImGui::TableSetupColumn("Usage");
                ImGui::TableSetupColumn("Job");
                ImGui::TableHeadersRow();

                for (auto& m : mem_allocs_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%d", m.numa_node);
                    ImGui::TableNextColumn(); ImGui::Text("%.1f / %.1f GiB", m.used_gb, m.total_gb);
                    ImGui::TableNextColumn();
                    float frac = (m.total_gb > 0) ? m.used_gb / m.total_gb : 0.0f;
                    char ov[16]; std::snprintf(ov, sizeof(ov), "%.0f%%", frac * 100.0f);
                    ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(m.assigned_job.c_str());
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
