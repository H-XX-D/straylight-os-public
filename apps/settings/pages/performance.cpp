// apps/settings/pages/performance.cpp
#include "performance.h"
#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace straylight::settings {

static constexpr ImVec4 kCyan    = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple  = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold    = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted   = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2  = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger  = {1.0f, 0.298f, 0.416f, 1.0f};

static ImVec4 temp_color(float t) {
    if (t <= 50.0f) return kCyan;
    if (t <= 70.0f) return kGold;
    return kDanger;
}

static ImU32 ToU32(ImVec4 v) {
    return IM_COL32(static_cast<int>(v.x*255), static_cast<int>(v.y*255),
                    static_cast<int>(v.z*255), static_cast<int>(v.w*255));
}

void PerformancePage::load() {
    // 16 logical cores across 2 sockets
    for (int i = 0; i < 16; ++i) {
        CpuCore c;
        c.core_id    = i;
        c.socket     = i / 8;
        c.cluster    = (i % 8) / 4;
        c.freq_mhz   = 3600 + (i % 3) * 200;
        c.freq_max_mhz = 5200;
        c.util_pct   = 10.0f + static_cast<float>(rand() % 80);
        c.temp_c     = 38.0f + static_cast<float>(rand() % 45);
        c.online     = true;
        c.governor   = i < 4 ? "performance" : "schedutil";
        cores_.push_back(c);

        for (int s = 0; s < 32; ++s) {
            core_util_history_[i][s] = 5.0f + static_cast<float>(rand() % 85);
        }
    }

    freq_cfg_.governor_idx  = 2;  // schedutil
    freq_cfg_.min_freq_idx  = 0;
    freq_cfg_.max_freq_idx  = 5;
    freq_cfg_.boost_enabled = true;
    freq_cfg_.hwp_enabled   = true;

    hp_cfg_.transparent_hp  = true;
    hp_cfg_.thp_mode_idx    = 1;  // madvise
    hp_cfg_.huge_2mb_count  = 512;
    hp_cfg_.huge_1gb_count  = 4;
}

void PerformancePage::render_topology_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("TopologyChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "CPU TOPOLOGY");
    ImGui::Separator();
    ImGui::Spacing();

    float avail_w  = ImGui::GetContentRegionAvail().x;
    float tile_sz  = 64.0f;
    float tile_gap = 8.0f;
    int   cols     = static_cast<int>(avail_w / (tile_sz + tile_gap));
    if (cols < 1) cols = 1;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < static_cast<int>(cores_.size()); ++i) {
        const auto& c = cores_[i];

        if (i % cols != 0) ImGui::SameLine(0.0f, tile_gap);

        ImVec2 tile_origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(tile_sz, tile_sz));

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Core %d  Socket %d\n%d/%d MHz\nUtil: %.0f%%  Temp: %.1f°C\nGov: %s",
                c.core_id, c.socket, c.freq_mhz, c.freq_max_mhz,
                c.util_pct, c.temp_c, c.governor.c_str());
        }

        ImVec4 t_col    = temp_color(c.temp_c);
        ImU32  bg_col   = IM_COL32(
            static_cast<int>(t_col.x * c.util_pct / 100.0f * 80 + 8),
            static_cast<int>(t_col.y * c.util_pct / 100.0f * 80 + 8),
            static_cast<int>(t_col.z * c.util_pct / 100.0f * 80 + 8),
            220);

        dl->AddRectFilled(tile_origin,
            {tile_origin.x + tile_sz, tile_origin.y + tile_sz},
            bg_col, 8.0f);
        dl->AddRect(tile_origin,
            {tile_origin.x + tile_sz, tile_origin.y + tile_sz},
            ToU32(t_col), 8.0f, 0, 1.2f);

        char core_label[8];
        std::snprintf(core_label, sizeof(core_label), "C%d", c.core_id);
        dl->AddText({tile_origin.x + 4.0f, tile_origin.y + 4.0f}, ToU32(kMuted), core_label);

        char util_label[8];
        std::snprintf(util_label, sizeof(util_label), "%.0f%%", c.util_pct);
        dl->AddText({tile_origin.x + 4.0f, tile_origin.y + 22.0f}, ToU32(t_col), util_label);

        char temp_label[10];
        std::snprintf(temp_label, sizeof(temp_label), "%.0f°", c.temp_c);
        dl->AddText({tile_origin.x + 4.0f, tile_origin.y + 40.0f}, ToU32(kMuted2), temp_label);
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextColored(kCyan,  "■ ≤50°C");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kGold,  "■ 51–70°C");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kDanger,"■ >70°C");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted2, " Fill intensity ∝ utilization");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PerformancePage::render_freq_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("FreqChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "FREQUENCY SCALING");
    ImGui::Separator();
    ImGui::Spacing();

    const char* governors[] = {"performance", "powersave", "schedutil", "ondemand"};
    ImGui::TextColored(kMuted, "CPU Governor:");
    ImGui::Combo("##governor", &freq_cfg_.governor_idx, governors, 4);

    ImGui::Spacing();

    const char* freq_steps[] = {"800 MHz","1.2 GHz","2.0 GHz","3.0 GHz","4.0 GHz","5.2 GHz"};
    ImGui::TextColored(kMuted, "Min Frequency:");
    ImGui::Combo("##min_freq", &freq_cfg_.min_freq_idx, freq_steps, 6);

    ImGui::TextColored(kMuted, "Max Frequency:");
    ImGui::Combo("##max_freq", &freq_cfg_.max_freq_idx, freq_steps, 6);

    ImGui::Spacing();
    ImGui::Checkbox("Turbo Boost", &freq_cfg_.boost_enabled);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextColored(kMuted2, "Intel Turbo / AMD Precision Boost");

    ImGui::Spacing();
    ImGui::Checkbox("HWP (Hardware P-States)", &freq_cfg_.hwp_enabled);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextColored(kMuted2, "Hardware-controlled frequency scaling");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Per-core freq display
    if (ImGui::BeginTable("core_freq_table", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_ScrollY, ImVec2(0.0f, 140.0f))) {
        ImGui::TableSetupColumn("Core", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Freq",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Util%", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Gov",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableHeadersRow();
        for (const auto& c : cores_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kMuted, "C%d", c.core_id);
            ImGui::TableSetColumnIndex(1);
            char freq_str[32];
            std::snprintf(freq_str, sizeof(freq_str), "%d MHz", c.freq_mhz);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
            ImGui::ProgressBar(
                static_cast<float>(c.freq_mhz) / static_cast<float>(c.freq_max_mhz),
                ImVec2(-1.0f, 0.0f), freq_str);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(c.util_pct > 80.0f ? kDanger : kMuted, "%.0f%%", c.util_pct);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(kMuted2, "%s", c.governor.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.545f, 0.361f, 0.965f, 0.15f});
    if (ImGui::Button("Apply Scaling Config", ImVec2(190.0f, 32.0f))) {}
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PerformancePage::render_hugepages_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("HPChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "HUGEPAGE CONFIGURATION");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Transparent Hugepages (THP)", &hp_cfg_.transparent_hp);
    ImGui::Spacing();

    if (hp_cfg_.transparent_hp) {
        const char* thp_modes[] = {"always", "madvise", "never"};
        ImGui::TextColored(kMuted, "THP Mode:");
        ImGui::Combo("##thp_mode", &hp_cfg_.thp_mode_idx, thp_modes, 3);
        ImGui::Spacing();
    }

    ImGui::TextColored(kMuted, "2 MB Hugepages:");
    ImGui::SliderInt("##hp_2mb", &hp_cfg_.huge_2mb_count, 0, 4096);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kMuted2, "= %d MB", hp_cfg_.huge_2mb_count * 2);

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "1 GB Hugepages:");
    ImGui::SliderInt("##hp_1gb", &hp_cfg_.huge_1gb_count, 0, 64);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextColored(kMuted2, "= %d GB", hp_cfg_.huge_1gb_count);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    int total_hp_mb = hp_cfg_.huge_2mb_count * 2 + hp_cfg_.huge_1gb_count * 1024;
    ImGui::TextColored(kCyan, "Reserved for Hugepages: %d MB (%.1f GB)",
        total_hp_mb, static_cast<float>(total_hp_mb) / 1024.0f);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.957f, 0.722f, 0.271f, 0.15f});
    if (ImGui::Button("Apply Hugepage Config", ImVec2(190.0f, 32.0f))) {}
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PerformancePage::render() {
    if (ImGui::BeginTabBar("perf_tabs")) {
        const char* tabs[] = {"Topology", "Freq Scaling", "Hugepages"};
        for (int i = 0; i < 3; ++i) {
            if (ImGui::BeginTabItem(tabs[i])) {
                active_tab_ = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    switch (active_tab_) {
        case 0: render_topology_tab();  break;
        case 1: render_freq_tab();      break;
        case 2: render_hugepages_tab(); break;
    }
}

} // namespace straylight::settings
