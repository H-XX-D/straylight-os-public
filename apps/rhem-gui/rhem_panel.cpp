// apps/rhem-gui/rhem_panel.cpp
#include "rhem_panel.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace straylight::rhem {

static constexpr ImVec4 kCyan    = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple  = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold    = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted   = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2  = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger  = {1.0f, 0.298f, 0.416f, 1.0f};

static ImVec4 kNVMe   = {0.55f, 0.15f, 0.15f, 1.0f};
static ImVec4 kGreen  = {0.18f, 0.70f, 0.40f, 1.0f};

static ImU32 ToU32(ImVec4 v) {
    return IM_COL32(static_cast<int>(v.x*255), static_cast<int>(v.y*255),
                    static_cast<int>(v.z*255), static_cast<int>(v.w*255));
}

static ImVec4 tier_color(TierType t) {
    switch (t) {
        case TierType::DRAM:  return kCyan;
        case TierType::HBM:   return kPurple;
        case TierType::PMEM:  return kGold;
        case TierType::CXL:   return kGreen;
        case TierType::NVMe:  return kNVMe;
    }
    return kMuted;
}

static const char* tier_name(TierType t) {
    switch (t) {
        case TierType::DRAM: return "DRAM (DDR5)";
        case TierType::HBM:  return "HBM3";
        case TierType::PMEM: return "Optane PMEM";
        case TierType::CXL:  return "CXL Expand";
        case TierType::NVMe: return "NVMe SSD";
    }
    return "Unknown";
}

void RhemState::init() {
    tiers = {
        {TierType::DRAM,  "DDR5-5600",   512,  182,  40000.0f, 1000.0f,  80.0f, 0.5f},
        {TierType::HBM,   "HBM3 Stack",   96,   71,  800000.0f, 480000.0f, 3.2f, 2.5f},
        {TierType::PMEM,  "Optane P5800", 256,   99,   13000.0f,   5000.0f, 305.0f, 3.0f},
        {TierType::CXL,   "CXL 2.0 Exp", 512,  120,   20000.0f,  12000.0f, 180.0f, 4.5f},
        {TierType::NVMe,  "PCIe Gen5 SSD",8192,3200, 7000.0f, 5000.0f,80000.0f, 12.0f},
    };

    policies = {
        {"Latency-First", "Route all hot allocations to HBM, demote idle to PMEM after 30s", 30, 90},
        {"Capacity-First","Maximize DRAM utilization; spill into CXL then NVMe",              120, 95},
        {"Energy-Aware",  "Minimize power: prefer PMEM for reads, reduce DRAM footprint",     60, 70},
    };
    active_policy = 0;

    allocations = {
        {"ML Model Weights",  3072, TierType::HBM,  true,  false, 0.0f,   0},
        {"Frame Buffers",      512, TierType::DRAM, true,  false, 0.0f,   1},
        {"Network RX Ring",     64, TierType::DRAM, true,  false, 0.0f,   0},
        {"Log Ring",            48, TierType::PMEM, false, false, 0.0f,   0},
        {"Cache Buffers",     1024, TierType::DRAM, true,  true,  0.38f,  0},
        {"Dataset Staging",   2048, TierType::CXL,  false, false, 0.0f,   0},
        {"Swap Space",        4096, TierType::NVMe, false, false, 0.0f,   0},
        {"Kernel Slab",        128, TierType::DRAM, false, false, 0.0f,   0},
    };
}

void RhemPanel::render_topology_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("TopoPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "MEMORY TIER TOPOLOGY");
    ImGui::Separator();
    ImGui::Spacing();

    float avail_w     = ImGui::GetContentRegionAvail().x;
    float lane_h      = 52.0f;
    float lane_gap    = 10.0f;
    float total_gb    = 0.0f;
    for (const auto& t : state_.tiers) total_gb += static_cast<float>(t.total_gb);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& tier : state_.tiers) {
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float frac    = static_cast<float>(tier.total_gb) / total_gb;
        float bar_w   = avail_w * frac * 0.85f;
        ImVec4 col    = tier_color(tier.type);

        // Background lane
        dl->AddRectFilled(origin,
            {origin.x + avail_w, origin.y + lane_h},
            IM_COL32(10, 14, 30, 200), 8.0f);

        // Tier bar proportional to capacity
        float used_f  = static_cast<float>(tier.used_gb) / static_cast<float>(tier.total_gb);
        dl->AddRectFilled(origin,
            {origin.x + bar_w, origin.y + lane_h},
            ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, 0.18f)), 8.0f);
        dl->AddRectFilled(origin,
            {origin.x + bar_w * used_f, origin.y + lane_h},
            ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, 0.38f)), 8.0f);
        dl->AddRect(origin,
            {origin.x + bar_w, origin.y + lane_h},
            ToU32(col), 8.0f, 0, 1.4f);

        // Label
        char label[128];
        std::snprintf(label, sizeof(label), "%s  %llu/%llu GB  |  R:%.0f MB/s  W:%.0f MB/s  Lat:%.0f ns",
            tier_name(tier.type),
            static_cast<unsigned long long>(tier.used_gb),
            static_cast<unsigned long long>(tier.total_gb),
            tier.read_bw_mbs, tier.write_bw_mbs, tier.latency_ns);
        dl->AddText(
            {origin.x + 8.0f, origin.y + lane_h * 0.30f},
            ToU32(col), label);

        ImGui::Dummy(ImVec2(avail_w, lane_h + lane_gap));
    }

    ImGui::Spacing();
    ImGui::TextColored(kMuted2, "Bar width ∝ tier capacity   |   Fill ∝ utilization");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RhemPanel::render_allocations_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("AllocsPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "ACTIVE ALLOCATIONS");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.545f, 0.361f, 0.965f, 0.15f});
    if (ImGui::Button("Migrate Hot→HBM",   ImVec2(160.0f, 28.0f))) {}
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.15f, 0.15f, 0.18f});
    if (ImGui::Button("Demote Cold→NVMe",  ImVec2(160.0f, 28.0f))) {}
    ImGui::PopStyleColor();

    ImGui::Spacing();

    if (ImGui::BeginTable("alloc_table", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 200.0f))) {
        ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size(MB)",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Tier",      ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Status",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& alloc : state_.allocations) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImVec4 name_col = alloc.hot ? kGold : kMuted;
            ImGui::TextColored(name_col, "%s", alloc.name.c_str());
            if (alloc.hot) {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextColored(kGold, "●");
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(alloc.size_mb));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(tier_color(alloc.current_tier), "%s",
                tier_name(alloc.current_tier));
            ImGui::TableSetColumnIndex(3);
            if (alloc.migrating) {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
                char prog_label[24];
                std::snprintf(prog_label, sizeof(prog_label), "%.0f%%", alloc.migration_pct * 100.0f);
                ImGui::ProgressBar(alloc.migration_pct, ImVec2(-1.0f, 0.0f), prog_label);
                ImGui::PopStyleColor();
            } else if (alloc.numa_node >= 0) {
                ImGui::TextColored(kMuted2, "NUMA %d", alloc.numa_node);
            } else {
                ImGui::TextColored(kSuccess, "Resident");
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RhemPanel::render_policy_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("PolicyPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "ALLOCATION POLICY");
    ImGui::Separator();
    ImGui::Spacing();

    for (int i = 0; i < static_cast<int>(state_.policies.size()); ++i) {
        const auto& p = state_.policies[i];
        bool selected = (state_.active_policy == i);
        if (ImGui::RadioButton(p.name.c_str(), selected)) {
            state_.active_policy = i;
        }
        ImGui::SameLine(0.0f, 20.0f);
        ImGui::TextColored(kMuted2, "%s", p.description.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (state_.active_policy >= 0 && state_.active_policy < static_cast<int>(state_.policies.size())) {
        auto& p = state_.policies[state_.active_policy];

        ImGui::TextColored(kMuted, "Demotion Timeout (s):");
        ImGui::SliderInt("##demo_timeout", &p.demotion_timeout_s, 5, 300);

        ImGui::TextColored(kMuted, "HBM Fill Target (%%):");
        ImGui::SliderInt("##hbm_target", &p.hbm_fill_target_pct, 50, 100);
    }

    ImGui::Spacing();
    static bool numa_bind = true;
    ImGui::Checkbox("NUMA Binding", &numa_bind);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::TextColored(kMuted2, "Pin allocations to NUMA-local memory nodes");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.15f});
    if (ImGui::Button("Apply Policy", ImVec2(140.0f, 32.0f))) {}
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RhemPanel::render_migration_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("MigPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "MIGRATION JOBS");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.957f, 0.722f, 0.271f, 0.12f});
    if (ImGui::Button("Pause All", ImVec2(110.0f, 28.0f))) {}
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{1.0f, 0.298f, 0.416f, 0.12f});
    if (ImGui::Button("Cancel",   ImVec2(90.0f, 28.0f))) {}
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool any = false;
    for (const auto& alloc : state_.allocations) {
        if (!alloc.migrating) continue;
        any = true;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.028f, 0.042f, 0.090f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::BeginChild(alloc.name.c_str(), ImVec2(-1.0f, 70.0f), true);

        ImGui::TextColored(kGold, "%s", alloc.name.c_str());
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextColored(kMuted, "%llu MB", static_cast<unsigned long long>(alloc.size_mb));
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextColored(kMuted2, "→");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(tier_color(alloc.current_tier), "%s", tier_name(alloc.current_tier));

        char pct_label[20];
        std::snprintf(pct_label, sizeof(pct_label), "%.1f%%", alloc.migration_pct * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
        ImGui::ProgressBar(alloc.migration_pct, ImVec2(-1.0f, 0.0f), pct_label);
        ImGui::PopStyleColor();

        int eta_s = static_cast<int>((1.0f - alloc.migration_pct) * 12.0f);
        ImGui::TextColored(kMuted2, "ETA: ~%ds", eta_s);

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (!any) {
        ImGui::TextColored(kMuted2, "No active migrations.");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RhemPanel::render() {
    ImGui::TextColored(kCyan, "STRAYLIGHT");
    ImGui::SameLine();
    ImGui::TextColored(kPurple, "RHEM");
    ImGui::SameLine(0.0f, 20.0f);
    uint64_t total_tb = 0;
    for (const auto& t : state_.tiers) total_tb += t.total_gb;
    ImGui::TextColored(kMuted, "Heterogeneous Memory Manager  |  %d Tiers  %llu GB",
        static_cast<int>(state_.tiers.size()),
        static_cast<unsigned long long>(total_tb));
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("rhem_tabs", ImGuiTabBarFlags_None)) {
        const char* tabs[] = {"Topology", "Allocations", "Policy", "Migration"};
        for (int i = 0; i < 4; ++i) {
            if (ImGui::BeginTabItem(tabs[i])) {
                state_.active_tab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    switch (state_.active_tab) {
        case 0: render_topology_tab();    break;
        case 1: render_allocations_tab(); break;
        case 2: render_policy_tab();      break;
        case 3: render_migration_tab();   break;
    }
}

} // namespace straylight::rhem
