// apps/pmem-gui/pmem_panel.cpp
#include "pmem_panel.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace straylight::pmem {

static constexpr ImVec4 kCyan     = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple   = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold     = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel  = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted    = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2   = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess  = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger   = {1.0f, 0.298f, 0.416f, 1.0f};

static ImU32 ToU32(ImVec4 v) {
    return IM_COL32(static_cast<int>(v.x*255), static_cast<int>(v.y*255),
                    static_cast<int>(v.z*255), static_cast<int>(v.w*255));
}

void PmemState::init() {
    dimms = {
        {0, "NMA1XXD128GPS", "NMP7DC4LXBHF-AA",  128, 45,  36.8f, 14.2f, 305.0f, 38.5f, 0, DimmHealth::Healthy, true, "App Direct"},
        {1, "NMA1XXD128GPS", "NMP7DC4LXBHF-AB",  128, 112, 35.1f, 13.8f, 312.0f, 41.2f, 2, DimmHealth::Warning, true, "App Direct"},
    };

    regions = {
        {0, "AppDirect", 256, 99,  2, true,  "/dev/pmem0"},
        {1, "Volatile",  128, 16,  1, false, "/dev/pmem1"},
    };

    namespaces = {
        {"/dev/pmem0", "pmem-data",   96, "fsdax",  true,  "/mnt/pmem0", 1250.0f},
        {"/dev/pmem1", "pmem-vol",    16, "devdax", false, "",            0.0f},
        {"/dev/pmem0.1","pmem-log",   12, "raw",    false, "",           420.0f},
    };

    // Bandwidth history with realistic sine-modulated pattern
    for (int i = 0; i < 128; ++i) {
        read_history[i]  = 28.0f + 6.0f * sinf(static_cast<float>(i) * 0.18f) +
            static_cast<float>(rand() % 40) / 20.0f;
        write_history[i] = 12.0f + 4.0f * sinf(static_cast<float>(i) * 0.22f + 1.0f) +
            static_cast<float>(rand() % 30) / 20.0f;
    }
}

void PmemPanel::render_devices_tab() {
    static int selected_dimm = -1;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("DevicesPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "PERSISTENT MEMORY DIMMS");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("dimms_table", 7,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 160.0f))) {
        ImGui::TableSetupColumn("Slot",    ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Serial",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Cap(GB)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Used",    ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Health",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Temp(°C)",ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Errors",  ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(state_.dimms.size()); ++i) {
            const auto& d = state_.dimms[i];
            ImGui::TableNextRow();
            bool sel = (selected_dimm == i);
            ImGui::TableSetColumnIndex(0);
            char row_id[16]; std::snprintf(row_id, sizeof(row_id), "%d##dsel", d.slot);
            if (ImGui::Selectable(row_id, sel,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                selected_dimm = (sel ? -1 : i);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kMuted, "%s", d.serial.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(d.capacity_gb));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(d.used_gb));
            ImGui::TableSetColumnIndex(4);
            ImVec4 h_col = d.health == DimmHealth::Healthy ? kSuccess
                         : (d.health == DimmHealth::Warning ? kGold : kDanger);
            const char* h_str = d.health == DimmHealth::Healthy ? "Healthy"
                              : (d.health == DimmHealth::Warning ? "Warning" : "Critical");
            ImGui::TextColored(h_col, "%s", h_str);
            ImGui::TableSetColumnIndex(5);
            ImVec4 t_col = d.temp_c < 40.0f ? kSuccess : (d.temp_c < 55.0f ? kGold : kDanger);
            ImGui::TextColored(t_col, "%.1f°", d.temp_c);
            ImGui::TableSetColumnIndex(6);
            ImGui::TextColored(d.media_errors > 0 ? kDanger : kSuccess, "%llu",
                static_cast<unsigned long long>(d.media_errors));
        }
        ImGui::EndTable();
    }

    // Expanded detail
    if (selected_dimm >= 0 && selected_dimm < static_cast<int>(state_.dimms.size())) {
        const auto& d = state_.dimms[selected_dimm];
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.030f, 0.048f, 0.095f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::BeginChild("dimm_detail", ImVec2(-1.0f, 120.0f), true);

        ImGui::TextColored(kCyan, "Slot %d — %s", d.slot, d.part_number.c_str());
        ImGui::Separator();
        ImGui::TextColored(kMuted, "Mode: %s", d.mode.c_str());
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::TextColored(kMuted, "DAX: ");
        ImGui::SameLine();
        ImGui::TextColored(d.dax_enabled ? kSuccess : kMuted2, d.dax_enabled ? "Enabled" : "Disabled");

        float used_f = static_cast<float>(d.used_gb) / static_cast<float>(d.capacity_gb);
        char used_label[32];
        std::snprintf(used_label, sizeof(used_label), "%lluGB / %lluGB",
            static_cast<unsigned long long>(d.used_gb),
            static_cast<unsigned long long>(d.capacity_gb));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
        ImGui::ProgressBar(used_f, ImVec2(-1.0f, 0.0f), used_label);
        ImGui::PopStyleColor();

        ImGui::TextColored(kMuted, "Read BW: ");
        ImGui::SameLine();
        ImGui::TextColored(kCyan, "%.1f GB/s", d.read_bw_gbs);
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::TextColored(kMuted, "Write BW: ");
        ImGui::SameLine();
        ImGui::TextColored(kPurple, "%.1f GB/s", d.write_bw_gbs);
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::TextColored(kMuted, "Latency: ");
        ImGui::SameLine();
        ImGui::TextColored(kGold, "%.0f ns", d.latency_ns);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.12f});
    if (ImGui::Button("Restart DIMM", ImVec2(130.0f, 28.0f))) {}
    ImGui::SameLine();
    if (ImGui::Button("Clear Errors", ImVec2(120.0f, 28.0f))) {}
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PmemPanel::render_regions_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("RegionsPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "MEMORY REGIONS");
    ImGui::Separator();
    ImGui::Spacing();

    float avail_w = ImGui::GetContentRegionAvail().x;
    float card_h  = 110.0f;

    for (const auto& region : state_.regions) {
        bool is_ad = (region.type == "AppDirect");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.030f, 0.048f, 0.095f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

        char card_id[32];
        std::snprintf(card_id, sizeof(card_id), "region_card_%d", region.id);
        ImGui::BeginChild(card_id, ImVec2(avail_w - 8.0f, card_h), true);

        ImGui::TextColored(is_ad ? kCyan : kPurple, "Region %d", region.id);
        ImGui::SameLine();
        ImGui::TextColored(is_ad ? kCyan : kPurple, "[%s]", region.type.c_str());
        ImGui::SameLine(0.0f, 12.0f);
        if (region.interleaved) {
            ImGui::TextColored(kGold, "Interleaved");
        }
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextColored(kMuted, "dev: %s", region.namespace_dev.c_str());

        ImGui::Spacing();
        float used_f = 1.0f - static_cast<float>(region.free_gb) / static_cast<float>(region.size_gb);
        char size_label[64];
        std::snprintf(size_label, sizeof(size_label), "%llu GB free / %llu GB",
            static_cast<unsigned long long>(region.free_gb),
            static_cast<unsigned long long>(region.size_gb));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, is_ad ? kCyan : kPurple);
        ImGui::ProgressBar(used_f, ImVec2(-1.0f, 0.0f), size_label);
        ImGui::PopStyleColor();

        ImGui::TextColored(kMuted2, "DIMMs: %d", region.dimm_count);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PmemPanel::render_namespaces_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("NSPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "NAMESPACES");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.133f, 0.773f, 0.447f, 0.15f});
    if (ImGui::Button("Create Namespace", ImVec2(160.0f, 28.0f))) {}
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{1.0f, 0.298f, 0.416f, 0.15f});
    if (ImGui::Button("Destroy", ImVec2(90.0f, 28.0f))) {}
    ImGui::PopStyleColor();

    ImGui::Spacing();

    if (ImGui::BeginTable("ns_table", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Device",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Mode",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("IOPS(K)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Mount",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& ns : state_.namespaces) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kCyan, "%s", ns.dev.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu GB", static_cast<unsigned long long>(ns.size_gb));
            ImGui::TableSetColumnIndex(2);
            ImVec4 mode_col = ns.mode == "fsdax" ? kSuccess : (ns.mode == "devdax" ? kCyan : kMuted2);
            ImGui::TextColored(mode_col, "%s", ns.mode.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(ns.iops_k > 0 ? kGold : kMuted2,
                ns.iops_k > 0 ? "%.0f K" : "—", ns.iops_k);
            ImGui::TableSetColumnIndex(4);
            if (ns.mounted) {
                ImGui::TextColored(kSuccess, "%s", ns.mount_point.c_str());
            } else {
                ImGui::TextColored(kMuted2, "unmounted");
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PmemPanel::render_perf_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("PerfPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "PERFORMANCE");
    ImGui::Separator();
    ImGui::Spacing();

    float avail_w = ImGui::GetContentRegionAvail().x;
    float chart_h = 80.0f;

    // Bandwidth chart using DrawList
    ImDrawList* dl       = ImGui::GetWindowDrawList();
    ImVec2 chart_origin  = ImGui::GetCursorScreenPos();
    float max_bw         = 40.0f;

    // Background
    dl->AddRectFilled(chart_origin,
        {chart_origin.x + avail_w, chart_origin.y + chart_h},
        IM_COL32(6, 10, 25, 220), 6.0f);

    int n_samples = 128;
    for (int i = 1; i < n_samples; ++i) {
        float t0 = static_cast<float>(i - 1) / static_cast<float>(n_samples - 1);
        float t1 = static_cast<float>(i)     / static_cast<float>(n_samples - 1);

        float r0 = state_.read_history[(state_.bw_offset + i - 1) % 128];
        float r1 = state_.read_history[(state_.bw_offset + i)     % 128];
        float w0 = state_.write_history[(state_.bw_offset + i - 1) % 128];
        float w1 = state_.write_history[(state_.bw_offset + i)     % 128];

        float x0 = chart_origin.x + t0 * avail_w;
        float x1 = chart_origin.x + t1 * avail_w;
        float ry0 = chart_origin.y + chart_h - (r0 / max_bw) * chart_h;
        float ry1 = chart_origin.y + chart_h - (r1 / max_bw) * chart_h;
        float wy0 = chart_origin.y + chart_h - (w0 / max_bw) * chart_h;
        float wy1 = chart_origin.y + chart_h - (w1 / max_bw) * chart_h;

        dl->AddLine({x0, ry0}, {x1, ry1}, ToU32(kCyan), 2.0f);
        dl->AddLine({x0, wy0}, {x1, wy1}, ToU32(kPurple), 2.0f);
    }

    ImGui::Dummy(ImVec2(avail_w, chart_h));

    ImGui::Spacing();
    ImGui::TextColored(kCyan, "■ Read");
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(kPurple, "■ Write");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Current BW gauges
    float cur_read  = state_.read_history[state_.bw_offset % 128];
    float cur_write = state_.write_history[state_.bw_offset % 128];

    ImGui::TextColored(kMuted, "Read Bandwidth:");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%.1f GB/s", cur_read);
    char r_label[16]; std::snprintf(r_label, sizeof(r_label), "%.1f GB/s", cur_read);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
    ImGui::ProgressBar(cur_read / max_bw, ImVec2(-1.0f, 0.0f), r_label);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Write Bandwidth:");
    ImGui::SameLine();
    ImGui::TextColored(kPurple, "%.1f GB/s", cur_write);
    char w_label[16]; std::snprintf(w_label, sizeof(w_label), "%.1f GB/s", cur_write);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kPurple);
    ImGui::ProgressBar(cur_write / max_bw, ImVec2(-1.0f, 0.0f), w_label);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Access Latency:");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "305 – 340 ns");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PmemPanel::render() {
    ImGui::TextColored(kCyan, "STRAYLIGHT");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "PMEM");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "Persistent Memory Manager  |  %d DIMMs  %d GB total",
        static_cast<int>(state_.dimms.size()),
        static_cast<int>(state_.dimms.size()) * 128);
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("pmem_tabs", ImGuiTabBarFlags_None)) {
        const char* tab_names[] = {"Devices", "Regions", "Namespaces", "Performance"};
        for (int t = 0; t < 4; ++t) {
            if (ImGui::BeginTabItem(tab_names[t])) {
                state_.active_tab = t;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    switch (state_.active_tab) {
        case 0: render_devices_tab();    break;
        case 1: render_regions_tab();    break;
        case 2: render_namespaces_tab(); break;
        case 3: render_perf_tab();       break;
    }
}

} // namespace straylight::pmem
