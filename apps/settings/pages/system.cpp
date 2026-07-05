// apps/settings/pages/system.cpp
#include "system.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>

namespace straylight::settings {

static constexpr ImVec4 kCyan    = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple  = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold    = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted   = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2  = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger  = {1.0f, 0.298f, 0.416f, 1.0f};

void SystemPage::load() {
    daemons_ = {
        {"straylight-core",      1001, 86400, true,  1.2f,  48.5f},
        {"straylight-bus",       1002, 86400, true,  0.3f,  22.1f},
        {"straylight-registry",  1003, 86399, true,  0.1f,  18.4f},
        {"straylight-scheduler", 1004, 86395, true,  2.8f,  35.6f},
        {"straylight-agent",     1050, 82000, true,  5.4f,  110.2f},
        {"straylight-enclave",   1055,  3600, false, 0.0f,   0.0f},
    };

    sched_.policy_idx    = 0;
    sched_.tick_hz       = 1000;
    sched_.cgroup_v2     = true;
    sched_.cpu_isolation = false;
    std::strncpy(sched_.isolated_cpus, "0-3", sizeof(sched_.isolated_cpus) - 1);

    registry_ = {
        {"compositor.refresh_hz",       "240",       "int",    true},
        {"compositor.gpu_vendor",       "nvidia",    "string", false},
        {"shell.animation_speed",       "0.85",      "float",  true},
        {"shell.blur_radius",           "18",        "int",    true},
        {"net.hostname",                "straylight","string", true},
        {"security.enclave_enabled",    "true",      "bool",   true},
        {"scheduler.cfs_period_us",     "100000",    "int",    true},
        {"audio.sample_rate",           "48000",     "int",    true},
        {"pmem.dax_enabled",            "true",      "bool",   true},
        {"rhem.active_policy",          "latency",   "string", true},
    };
}

void SystemPage::render_daemons_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("DaemonsChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "SYSTEM DAEMONS");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("daemon_table", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("PID",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Uptime",  ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("CPU%",    ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Mem(MB)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        for (auto& d : daemons_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(d.running ? kSuccess : kDanger, d.running ? "●" : "●");
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextColored(kMuted, "%s", d.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", d.pid);

            ImGui::TableSetColumnIndex(2);
            int h = d.uptime_s / 3600;
            int m = (d.uptime_s % 3600) / 60;
            char up_str[16];
            std::snprintf(up_str, sizeof(up_str), "%dh%02dm", h, m);
            ImGui::TextColored(kMuted2, "%s", up_str);

            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(d.cpu_pct > 5.0f ? kGold : kCyan, "%.1f", d.cpu_pct);

            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(kMuted, "%.1f", d.mem_mb);

            ImGui::TableSetColumnIndex(5);
            char restart_id[64], stop_id[64];
            std::snprintf(restart_id, sizeof(restart_id), "Restart##%s", d.name.c_str());
            std::snprintf(stop_id,    sizeof(stop_id),    "Stop##%s",    d.name.c_str());

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.10f});
            if (ImGui::SmallButton(restart_id)) {}
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{1.0f, 0.298f, 0.416f, 0.12f});
            if (ImGui::SmallButton(stop_id)) { if (d.running) d.running = false; }
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SystemPage::render_scheduler_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("SchedChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "SCHEDULER CONFIGURATION");
    ImGui::Separator();
    ImGui::Spacing();

    const char* policies[] = {"CFS (Completely Fair)", "FIFO (Real-time)", "RR (Round Robin)"};
    ImGui::TextColored(kMuted, "Scheduling Policy:");
    ImGui::Combo("##sched_policy", &sched_.policy_idx, policies, 3);

    ImGui::Spacing();
    const char* ticks[] = {"100 Hz", "250 Hz", "1000 Hz"};
    int tick_idx = sched_.tick_hz == 100 ? 0 : (sched_.tick_hz == 250 ? 1 : 2);
    ImGui::TextColored(kMuted, "Timer Frequency:");
    if (ImGui::Combo("##tick_hz", &tick_idx, ticks, 3)) {
        sched_.tick_hz = tick_idx == 0 ? 100 : (tick_idx == 1 ? 250 : 1000);
    }

    ImGui::Spacing();
    ImGui::Checkbox("cgroup v2 Hierarchy", &sched_.cgroup_v2);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted2, "Unified resource control hierarchy");

    ImGui::Spacing();
    ImGui::Checkbox("CPU Isolation (isolcpus)", &sched_.cpu_isolation);
    if (sched_.cpu_isolation) {
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##iso_cpus", sched_.isolated_cpus, sizeof(sched_.isolated_cpus));
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.15f});
    if (ImGui::Button("Apply Scheduler Config", ImVec2(200.0f, 32.0f))) {}
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SystemPage::render_registry_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("RegistryChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "SYSTEM REGISTRY");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##reg_search", "Search keys…", reg_search_, sizeof(reg_search_));
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.133f, 0.773f, 0.447f, 0.12f});
    if (ImGui::Button("+ Add", ImVec2(70.0f, 0.0f))) {}
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.10f});
    if (ImGui::Button("Export", ImVec2(70.0f, 0.0f))) {}
    ImGui::PopStyleColor();

    ImGui::Spacing();

    if (ImGui::BeginTable("reg_table", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("Key",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto& entry : registry_) {
            if (reg_search_[0] != '\0' &&
                entry.key.find(reg_search_) == std::string::npos) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kMuted, "%s", entry.key.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kMuted2, "%s", entry.type.c_str());
            ImGui::TableSetColumnIndex(2);
            if (entry.editable) {
                char buf[256];
                std::strncpy(buf, entry.value.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char edit_id[128];
                std::snprintf(edit_id, sizeof(edit_id), "##regv_%s", entry.key.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText(edit_id, buf, sizeof(buf),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    entry.value = buf;
                }
            } else {
                ImGui::TextColored(kMuted2, "%s", entry.value.c_str());
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SystemPage::render() {
    if (ImGui::BeginTabBar("system_tabs")) {
        const char* tabs[] = {"Daemons", "Scheduler", "Registry"};
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
        case 0: render_daemons_tab();   break;
        case 1: render_scheduler_tab(); break;
        case 2: render_registry_tab();  break;
    }
}

} // namespace straylight::settings
