// apps/intent-gui/intent_panel.h
// StrayLight Intent Commander panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

namespace straylight::intent {

struct PlannedAction {
    char description[256];
    char command[256];
    int  risk;  // 0=safe, 1=moderate, 2=dangerous
};

struct HistoryEntry {
    char input[256];
    char result[256];
    bool success;
    char timestamp[32];
};

struct IntentState {
    char input_text[512] = {};
    std::vector<PlannedAction> preview;
    std::vector<HistoryEntry> history;
    char output_text[2048] = {};
    bool executing = false;
    float exec_progress = 0;
    bool show_output = false;

    static constexpr const char* risk_labels[] = { "Safe", "Moderate", "Dangerous" };

    static PlannedAction make_action(const std::string& description,
                                     const std::string& command,
                                     int risk) {
        PlannedAction action{};
        std::snprintf(action.description, sizeof(action.description), "%s", description.c_str());
        std::snprintf(action.command, sizeof(action.command), "%s", command.c_str());
        action.risk = risk;
        return action;
    }

    void add_preview(const std::string& description, const std::string& command, int risk) {
        preview.push_back(make_action(description, command, risk));
    }

    void init() {
        history.push_back({"restart the network service", "NetworkManager restarted successfully", true, "10:12"});
        history.push_back({"show disk usage for /home", "du -sh /home/* completed", true, "10:05"});
        history.push_back({"update all packages", "28 packages upgraded", true, "09:30"});
        history.push_back({"kill process using port 8080", "Process 4521 terminated", true, "09:15"});
        history.push_back({"set GPU to performance mode", "Failed: permission denied", false, "08:50"});
    }

    void resolve(const char* text) {
        preview.clear();
        // Simulate resolution based on keywords
        std::string s(text);
        if (s.find("restart") != std::string::npos || s.find("service") != std::string::npos) {
            add_preview("Stop the target service", "systemctl stop <service>", 1);
            add_preview("Start the target service", "systemctl start <service>", 0);
            add_preview("Verify service status", "systemctl status <service>", 0);
        } else if (s.find("update") != std::string::npos || s.find("upgrade") != std::string::npos) {
            add_preview("Sync package database", "pacman -Sy", 0);
            add_preview("Create system snapshot", "snapper create --description pre-update", 0);
            add_preview("Upgrade all packages", "pacman -Su --noconfirm", 1);
        } else if (s.find("kill") != std::string::npos || s.find("stop") != std::string::npos) {
            add_preview("Identify target process", "pgrep -f <pattern>", 0);
            add_preview("Send SIGTERM to process", "kill <pid>", 1);
        } else if (s.find("disk") != std::string::npos || s.find("storage") != std::string::npos) {
            add_preview("Scan disk usage", "du -sh /* 2>/dev/null | sort -rh", 0);
            add_preview("Check filesystem health", "btrfs scrub start /", 0);
        } else if (s.find("delete") != std::string::npos || s.find("remove") != std::string::npos) {
            add_preview("Identify target files", "find / -name <pattern>", 0);
            add_preview("Remove matching files", "rm -rf <targets>", 2);
        } else {
            add_preview("Analyze intent", "straylight-intent parse \"" + std::string(text) + "\"", 0);
            add_preview("Execute resolved action", "<resolved command>", 1);
        }
    }
};

inline void render_intent_panel(IntentState& st) {
    if (st.history.empty()) st.init();

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("INTENT COMMANDER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Input area
    ImGui::BeginChild("##input_area", ImVec2(-1, 80), true);
    ImGui::TextColored(accent, "Natural Language Command");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160);
    bool enter = ImGui::InputTextWithHint("##intent_input",
        "e.g., 'restart the network service' or 'show disk usage'",
        st.input_text, sizeof(st.input_text), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Resolve", ImVec2(70, 24)) || enter) {
        if (strlen(st.input_text) > 0) {
            st.resolve(st.input_text);
            st.show_output = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(70, 24))) {
        memset(st.input_text, 0, sizeof(st.input_text));
        st.preview.clear();
        st.show_output = false;
    }
    ImGui::EndChild();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.55f;

    // Resolution preview
    ImGui::BeginChild("##preview", ImVec2(left_w, 0), true);
    ImGui::TextColored(accent, "Planned Actions");
    ImGui::Separator();
    ImGui::Spacing();

    if (st.preview.empty()) {
        ImGui::TextDisabled("Enter a command and click Resolve to see planned actions");
    } else {
        ImVec4 risk_colors[] = {
            ImVec4(0.0f, 1.0f, 0.67f, 1.0f),
            ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
            ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
        };

        for (int i = 0; i < (int)st.preview.size(); ++i) {
            auto& a = st.preview[i];
            ImGui::PushID(i);

            ImGui::Text("Step %d:", i + 1);
            ImGui::SameLine(70);
            ImGui::TextWrapped("%s", a.description);

            ImGui::TextDisabled("  Command:");
            ImGui::SameLine(100);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));
            char cmd_buf[256];
            snprintf(cmd_buf, 256, "%s", a.command);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##cmd", cmd_buf, 256, ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            ImGui::TextDisabled("  Risk:");
            ImGui::SameLine(100);
            ImGui::TextColored(risk_colors[a.risk], "[%s]", IntentState::risk_labels[a.risk]);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::PopID();
        }

        ImGui::Spacing();
        if (!st.executing) {
            bool has_dangerous = false;
            for (auto& a : st.preview) if (a.risk == 2) has_dangerous = true;

            if (has_dangerous) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.0f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.0f, 1.0f));
            }
            if (ImGui::Button("Execute All Steps", ImVec2(200, 34))) {
                st.executing = true;
                st.exec_progress = 0;
                st.show_output = true;
                snprintf(st.output_text, 2048, "Executing plan...\n");
            }
            if (has_dangerous) {
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Contains dangerous actions!");
            }
        } else {
            st.exec_progress += ImGui::GetIO().DeltaTime * 0.2f;
            if (st.exec_progress >= 1.0f) {
                st.exec_progress = 1.0f;
                st.executing = false;
                snprintf(st.output_text, 2048,
                         "Executing plan...\n"
                         "Step 1: Completed\n"
                         "Step 2: Completed\n"
                         "All steps executed successfully.\n");
                // Add to history
                HistoryEntry he{};
                snprintf(he.input, 256, "%s", st.input_text);
                snprintf(he.result, 256, "All %d steps completed", (int)st.preview.size());
                he.success = true;
                snprintf(he.timestamp, 32, "now");
                st.history.insert(st.history.begin(), he);
            }
            ImGui::ProgressBar(st.exec_progress, ImVec2(-1, 20), "Executing...");
        }

        // Output display
        if (st.show_output) {
            ImGui::Spacing();
            ImGui::TextColored(accent, "Output:");
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.04f, 0.04f, 0.06f, 1.0f));
            ImGui::InputTextMultiline("##output", st.output_text, sizeof(st.output_text),
                                       ImVec2(-1, 80), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Command history
    ImGui::BeginChild("##history", ImVec2(0, 0), true);
    ImGui::TextColored(accent, "Command History");
    ImGui::Separator();

    for (int i = 0; i < (int)st.history.size(); ++i) {
        auto& h = st.history[i];
        ImGui::PushID(1000 + i);

        ImGui::TextDisabled("[%s]", h.timestamp);
        ImGui::SameLine(60);
        if (h.success) {
            ImGui::TextColored(accent, "> ");
        } else {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "X ");
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", h.input);
        ImGui::TextDisabled("  %s", h.result);

        // Click to reuse
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            snprintf(st.input_text, 512, "%s", h.input);
        }

        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace straylight::intent
