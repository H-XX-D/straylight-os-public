// apps/settings/pages/security.cpp
#include "security.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace straylight::settings {

static constexpr ImVec4 kCyan    = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple  = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold    = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted   = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2  = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger  = {1.0f, 0.298f, 0.416f, 1.0f};

void SecurityPage::load() {
    enclave_ = {
        true, true, 3, 128, "2.22.100.3", "1.19.100.3", 99.2f
    };

    entropy_sources_ = {
        {"RDRAND",  "CPU DRNG",  true,  1200000.0f, true},
        {"RDSEED",  "CPU TRNG",  true,   800000.0f, true},
        {"TPM2",    "TPM 2.0",   true,    32000.0f, true},
        {"HWRNG",   "Char Dev",  false,       0.0f, false},
        {"Kernel",  "Kernel",    true,    64000.0f, true},
    };

    long long base_ts = 1700000000LL;
    audit_log_ = {
        {base_ts + 0,   "process_exec",   "straylight-core",   "allow", false},
        {base_ts + 5,   "file_open",      "user:operator",     "allow", false},
        {base_ts + 12,  "net_connect",    "straylight-agent",  "allow", false},
        {base_ts + 30,  "enclave_create", "straylight-enclave","allow", false},
        {base_ts + 45,  "privilege_esc",  "external:unknown",  "deny",  true},
        {base_ts + 60,  "file_write",     "user:operator",     "allow", false},
        {base_ts + 90,  "sudo",           "user:admin",        "allow", false},
        {base_ts + 120, "raw_socket",     "external:scanner",  "deny",  true},
        {base_ts + 135, "module_load",    "straylight-core",   "allow", false},
        {base_ts + 180, "bpf_prog_load",  "straylight-bus",    "allow", false},
    };
}

void SecurityPage::render_enclave_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("EnclaveChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "INTEL SGX ENCLAVE STATUS");
    ImGui::Separator();
    ImGui::Spacing();

    float card_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.028f, 0.045f, 0.090f, 1.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::BeginChild("sgx_card", ImVec2(card_w, 120.0f), true);

    ImGui::TextColored(kCyan, "SGX Support: ");
    ImGui::SameLine();
    ImGui::TextColored(enclave_.sgx_supported ? kSuccess : kDanger,
        enclave_.sgx_supported ? "Yes" : "No");
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextColored(kMuted, "SGX Enabled: ");
    ImGui::SameLine();
    ImGui::TextColored(enclave_.sgx_enabled ? kSuccess : kGold,
        enclave_.sgx_enabled ? "Enabled" : "Disabled");

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Active Enclaves: ");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%d", enclave_.enclave_count);
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextColored(kMuted, "EPC Memory: ");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%d MB", enclave_.epc_memory_mb);

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Platform SW: ");
    ImGui::SameLine();
    ImGui::TextColored(kMuted2, "%s", enclave_.platform_sw_version.c_str());
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextColored(kMuted, "DCAP: ");
    ImGui::SameLine();
    ImGui::TextColored(kMuted2, "%s", enclave_.dcap_version.c_str());

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Attestation Rate: ");
    ImGui::SameLine();
    ImGui::TextColored(enclave_.attestation_success_rate > 98.0f ? kSuccess : kGold,
        "%.1f%%", enclave_.attestation_success_rate);
    char att_label[24];
    std::snprintf(att_label, sizeof(att_label), "%.1f%%", enclave_.attestation_success_rate);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kCyan);
    ImGui::ProgressBar(enclave_.attestation_success_rate / 100.0f, ImVec2(-1.0f, 0.0f), att_label);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.12f});
    if (ImGui::Button("Re-Attest", ImVec2(100.0f, 28.0f))) {}
    ImGui::SameLine();
    if (ImGui::Button("Show TCB",  ImVec2(100.0f, 28.0f))) {}
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SecurityPage::render_entropy_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("EntropyChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "ENTROPY SOURCES");
    ImGui::Separator();
    ImGui::Spacing();

    float avail_w = ImGui::GetContentRegionAvail().x;

    if (ImGui::BeginTable("entropy_table", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Source",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Health",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Rate",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        float max_bps = 1500000.0f;
        for (const auto& src : entropy_sources_) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(src.active ? kCyan : kMuted2, "%s", src.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kMuted2, "%s", src.type.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(src.healthy ? kSuccess : kDanger,
                src.healthy ? "OK" : "FAIL");
            ImGui::TableSetColumnIndex(3);
            if (src.active && src.bits_per_sec > 0) {
                char rate_label[32];
                if (src.bits_per_sec >= 1000000.0f)
                    std::snprintf(rate_label, sizeof(rate_label), "%.1f Mb/s", src.bits_per_sec / 1e6f);
                else if (src.bits_per_sec >= 1000.0f)
                    std::snprintf(rate_label, sizeof(rate_label), "%.1f Kb/s", src.bits_per_sec / 1e3f);
                else
                    std::snprintf(rate_label, sizeof(rate_label), "%.0f b/s", src.bits_per_sec);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kPurple);
                ImGui::ProgressBar(src.bits_per_sec / max_bps,
                    ImVec2(avail_w * 0.5f, 0.0f), rate_label);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextColored(kMuted2, "inactive");
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SecurityPage::render_audit_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("AuditChild", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "AUDIT LOG");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##audit_filter", "Filter by event or actor…",
        audit_filter_, sizeof(audit_filter_));
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{1.0f, 0.298f, 0.416f, 0.10f});
    static bool show_flagged = false;
    if (ImGui::Button("Flagged Only", ImVec2(110.0f, 0.0f))) show_flagged = !show_flagged;
    ImGui::PopStyleColor();

    ImGui::Spacing();

    if (ImGui::BeginTable("audit_table", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("Time",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Event",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actor",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableHeadersRow();

        for (const auto& e : audit_log_) {
            if (show_flagged && !e.flagged) continue;
            if (audit_filter_[0] != '\0' &&
                e.event.find(audit_filter_) == std::string::npos &&
                e.actor.find(audit_filter_) == std::string::npos) continue;

            ImGui::TableNextRow();
            if (e.flagged) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    IM_COL32(100, 20, 30, 60));
            }
            ImGui::TableSetColumnIndex(0);
            char ts[16];
            std::snprintf(ts, sizeof(ts), "+%llds", e.timestamp - 1700000000LL);
            ImGui::TextColored(kMuted2, "%s", ts);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(e.flagged ? kDanger : kMuted, "%s", e.event.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(kMuted2, "%s", e.actor.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(e.result == "allow" ? kSuccess : kDanger,
                "%s", e.result.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SecurityPage::render() {
    if (ImGui::BeginTabBar("security_tabs")) {
        const char* tabs[] = {"Enclave", "Entropy", "Audit Log"};
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
        case 0: render_enclave_tab(); break;
        case 1: render_entropy_tab(); break;
        case 2: render_audit_tab();   break;
    }
}

} // namespace straylight::settings
