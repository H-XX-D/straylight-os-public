// apps/health-gui/health_panel.h
// StrayLight Health GUI — System Health Dashboard panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <straylight/ipc_client.h>

namespace straylight::health {

struct HealthCheck {
    std::string name;
    std::string icon;   // text icon
    float       score;  // 0-100
    int         status; // 0=ok, 1=warn, 2=critical
    std::string detail;
    std::string category;
};

struct Alert {
    std::string message;
    std::string timestamp;
    int         severity; // 0=info, 1=warn, 2=critical
};

struct HealthState {
    float overall_score = 0.0f;
    std::vector<HealthCheck> checks;
    std::vector<Alert> alerts;
    float score_history[60] = {};
    int   history_offset = 0;
    bool  generating_report = false;
    float report_progress = 0.0f;

    // Live daemon link (no fabricated data)
    IpcJsonClient ipc_;
    bool          connected_ = false;
    std::string   conn_error_;
    double        last_refresh_ = -1.0e9;
    bool          history_primed_ = false;

    static int status_to_int(const std::string& s) {
        if (s == "ok") return 0;
        if (s == "warn" || s == "warning") return 1;
        return 2;
    }

    static std::string categorize(const std::string& name) {
        if (name.find("CPU") != std::string::npos || name.find("Memory") != std::string::npos ||
            name.find("GPU") != std::string::npos || name.find("Battery") != std::string::npos) return "Hardware";
        if (name.find("Disk") != std::string::npos || name.find("Filesystem") != std::string::npos ||
            name.find("SMART") != std::string::npos) return "Storage";
        if (name.find("Network") != std::string::npos || name.find("DNS") != std::string::npos) return "Network";
        if (name.find("Service") != std::string::npos) return "Services";
        return "System";
    }

    void ensure_connected() {
        if (connected_) return;
        auto r = ipc_.connect("/run/straylight/health.sock");
        connected_ = r.has_value();
        conn_error_ = connected_ ? std::string() : r.error();
    }

    void refresh() {
        ensure_connected();
        if (!connected_) return;
        auto r = ipc_.call("status");
        if (!r.has_value()) { connected_ = false; conn_error_ = r.error(); return; }
        const nlohmann::json& j = r.value();

        checks.clear();
        alerts.clear();
        if (j.contains("checks") && j["checks"].is_array()) {
            for (const auto& cj : j["checks"]) {
                HealthCheck c;
                c.name     = cj.value("name", std::string());
                c.detail   = cj.value("detail", std::string());
                c.score    = cj.value("score", 0.0f);
                c.status   = status_to_int(cj.value("status", std::string("ok")));
                c.icon     = "[*]";
                c.category = categorize(c.name);
                if (c.status != 0) {
                    Alert a;
                    a.message   = c.name + ": " + c.detail;
                    a.timestamp = j.value("timestamp", std::string());
                    a.severity  = c.status;
                    alerts.push_back(std::move(a));
                }
                checks.push_back(std::move(c));
            }
        }
        overall_score = j.value("overall_score", 0.0f);

        if (!history_primed_) {
            for (int i = 0; i < 60; ++i) score_history[i] = overall_score;
            history_primed_ = true;
        }
        score_history[history_offset] = overall_score;
        history_offset = (history_offset + 1) % 60;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() {
        refresh();
    }
};

inline void render_health_panel(HealthState& st) {
    st.maybe_refresh();
    if (!st.connected_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("health daemon disconnected: %s", st.conn_error_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT HEALTH");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Top section: Overall gauge + alerts
    float gauge_section_h = 220.0f;
    if (ImGui::BeginChild("##gauge_section", ImVec2(0, gauge_section_h), false)) {
        float gauge_w = 250.0f;

        // Circular gauge (drawn with ImDrawList)
        if (ImGui::BeginChild("##gauge", ImVec2(gauge_w, -1), false)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 center = ImGui::GetCursorScreenPos();
            center.x += gauge_w * 0.5f;
            center.y += 100.0f;
            float radius = 80.0f;

            // Background arc
            for (float a = 3.14159f; a < 3.14159f * 2.0f; a += 0.02f) {
                float x1 = center.x + cosf(a) * radius;
                float y1 = center.y + sinf(a) * radius;
                float x2 = center.x + cosf(a) * (radius - 12.0f);
                float y2 = center.y + sinf(a) * (radius - 12.0f);
                draw->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(40, 40, 60, 255), 2.0f);
            }

            // Score arc
            float score_frac = st.overall_score / 100.0f;
            float score_angle = 3.14159f + 3.14159f * score_frac;
            ImU32 score_col = st.overall_score >= 80 ? IM_COL32(0, 200, 130, 255)
                            : st.overall_score >= 60 ? IM_COL32(200, 180, 40, 255)
                            : IM_COL32(200, 60, 60, 255);
            for (float a = 3.14159f; a < score_angle; a += 0.02f) {
                float x1 = center.x + cosf(a) * radius;
                float y1 = center.y + sinf(a) * radius;
                float x2 = center.x + cosf(a) * (radius - 12.0f);
                float y2 = center.y + sinf(a) * (radius - 12.0f);
                draw->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), score_col, 3.0f);
            }

            // Score text
            char score_text[16];
            snprintf(score_text, sizeof(score_text), "%.0f", st.overall_score);
            ImVec2 text_size = ImGui::CalcTextSize(score_text);
            draw->AddText(nullptr, 36.0f, ImVec2(center.x - text_size.x * 1.5f, center.y - 25.0f),
                          score_col, score_text);
            draw->AddText(ImVec2(center.x - 30, center.y + 15.0f), IM_COL32(160, 160, 160, 255), "Health Score");

            ImGui::Dummy(ImVec2(0, 200));
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Score history sparkline
        if (ImGui::BeginChild("##history_section", ImVec2(300, -1), false)) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Health Over Time");
            ImGui::Spacing();
            ImGui::PlotLines("##history", st.score_history, 60, st.history_offset,
                             nullptr, 0.0f, 100.0f, ImVec2(-1, 120));
            ImGui::TextDisabled("Last 60 readings");

            // History is updated from real daemon data in HealthState::refresh()
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Alert list
        if (ImGui::BeginChild("##alerts", ImVec2(0, -1), true)) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Alerts");
            ImGui::Separator();
            for (auto& a : st.alerts) {
                ImVec4 col = a.severity == 2 ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                           : a.severity == 1 ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f)
                           : ImVec4(0.4f, 0.7f, 0.4f, 1.0f);
                const char* sev_str = a.severity == 2 ? "[CRIT]" : a.severity == 1 ? "[WARN]" : "[INFO]";
                ImGui::TextColored(col, "%s", sev_str);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", a.message.c_str());
                ImGui::TextDisabled("  %s", a.timestamp.c_str());
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // Generate Report button
    ImGui::Spacing();
    if (st.generating_report) {
        ImGui::BeginDisabled();
        ImGui::Button("Generating Report...", ImVec2(180, 30));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::ProgressBar(st.report_progress, ImVec2(200, 30));
        st.report_progress += ImGui::GetIO().DeltaTime * 0.2f;
        if (st.report_progress >= 1.0f) { st.generating_report = false; st.report_progress = 0.0f; }
    } else {
        if (ImGui::Button("Generate Report", ImVec2(160, 30))) {
            st.generating_report = true;
            st.report_progress = 0.0f;
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Per-check cards
    if (ImGui::BeginChild("##check_cards", ImVec2(0, -1), false)) {
        std::string current_cat;
        for (auto& c : st.checks) {
            if (c.category != current_cat) {
                current_cat = c.category;
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", current_cat.c_str());
                ImGui::Separator();
            }

            ImVec4 status_col = c.status == 0 ? ImVec4(0.2f, 1.0f, 0.5f, 1.0f)
                              : c.status == 1 ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f)
                              : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            const char* status_str = c.status == 0 ? "OK" : c.status == 1 ? "WARN" : "CRIT";

            // Icon
            ImGui::TextColored(status_col, "%s", c.icon.c_str());
            ImGui::SameLine();

            // Name
            ImGui::Text("%s", c.name.c_str());
            ImGui::SameLine(220);

            // Score bar
            float frac = c.score / 100.0f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, status_col);
            char score_label[32];
            snprintf(score_label, sizeof(score_label), "%.0f/100", c.score);
            ImGui::ProgressBar(frac, ImVec2(200, 18), score_label);
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::TextColored(status_col, "[%s]", status_str);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", c.detail.c_str());
        }
    }
    ImGui::EndChild();
}

} // namespace straylight::health
