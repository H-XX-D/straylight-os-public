// apps/cron-gui/cron_panel.h
// StrayLight Cron GUI — Task Scheduler panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <fstream>
#include <sstream>
#include <dirent.h>

namespace straylight::cron {

struct TaskExecution {
    std::string timestamp;
    int         exit_code;
    float       duration_sec;
    std::string stdout_text;
    std::string stderr_text;
};

struct CronTask {
    std::string name;
    std::string command;
    std::string schedule;
    std::string last_run;
    std::string next_run;
    int         status; // 0=idle, 1=running, 2=failed, 3=disabled
    bool        enabled;
    std::string resource_constraints;
    std::vector<std::string> dependencies;
    std::vector<TaskExecution> history;
};

struct CronState {
    std::vector<CronTask> tasks;
    int selected_index = -1;

    // Add task dialog
    bool show_add_dialog = false;
    char new_name[128] = {};
    char new_command[512] = {};
    int  new_schedule_idx = 0;
    char new_custom_schedule[64] = {};
    char new_constraints[256] = {};

    // Delete
    bool show_delete_confirm = false;

    // // [wired:os] cron-gui real data source
    // OS-backed source: no daemon. Reads systemd timers + classic cron files.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // Proven popen helper (copied from apps/hub/service_panel.cpp).
    static std::string exec_cmd(const std::string& cmd) {
        std::array<char, 4096> buf;
        std::string result;
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return "";
        while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            result += buf.data();
        }
        ::pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        return result;
    }

    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // Extract `argv[]= ... ;` payload from a systemd ExecStart property value,
    // which looks like: { path=/x ; argv[]=/x arg1 arg2 ; ignore_errors=no ; ... }
    static std::string parse_execstart(const std::string& v) {
        size_t a = v.find("argv[]=");
        if (a == std::string::npos) return trim(v);
        a += 7; // strlen("argv[]=")
        size_t e = v.find(" ;", a);
        if (e == std::string::npos) e = v.size();
        return trim(v.substr(a, e - a));
    }

    void ensure_source() {
        // systemctl presence is our "connection". No socket; this is the
        // /proc + /sys + systemd pattern used by apps/system_monitor and apps/hub.
        std::string probe = exec_cmd("command -v systemctl 2>/dev/null");
        if (probe.empty()) {
            ok_ = false;
            err_ = "systemctl not found on PATH (no systemd timer source)";
            return;
        }
        ok_ = true;
        err_.clear();
    }

    // Map systemd state -> CronTask.status (0=idle,1=running,2=failed,3=disabled).
    static int map_status(const std::string& is_active, const std::string& is_enabled,
                          const std::string& result, const std::string& exec_status) {
        if (is_enabled == "disabled" || is_enabled == "masked") return 3;
        if (is_active == "active" || is_active == "activating") return 1;
        if (result == "success" && (exec_status.empty() || exec_status == "0")) return 0;
        if (!exec_status.empty() && exec_status != "0") return 2;
        if (result != "success" && !result.empty()) return 2;
        return 0;
    }

    // Parse the 5-field cron expr + trailing command from one cron line.
    // Returns true and fills schedule/command on success. Handles both
    // /etc/crontab and /etc/cron.d/ style lines (which carry a user field).
    static bool parse_cron_line(const std::string& raw, bool has_user,
                                std::string& schedule, std::string& command) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') return false;
        if (line.find('=') != std::string::npos && line.find(' ') == std::string::npos)
            return false; // env assignment like SHELL=/bin/sh
        std::istringstream iss(line);
        std::string f[5];
        for (int i = 0; i < 5; ++i) {
            if (!(iss >> f[i])) return false;
        }
        // crontab lines may use macros (@daily etc.) -> first token starts with @
        if (!f[0].empty() && f[0][0] == '@') return false;
        schedule = f[0] + " " + f[1] + " " + f[2] + " " + f[3] + " " + f[4];
        std::string rest;
        std::getline(iss, rest);
        rest = trim(rest);
        if (has_user) {
            // strip the leading user field
            std::istringstream r2(rest);
            std::string user;
            r2 >> user;
            std::string cmd;
            std::getline(r2, cmd);
            command = trim(cmd);
        } else {
            command = rest;
        }
        return true;
    }

    void load_systemd_timers() {
        // Enumerate timer units (machine-readable, one per line).
        std::string units = exec_cmd(
            "systemctl list-timers --all --no-pager --no-legend 2>/dev/null "
            "| awk '{for(i=1;i<=NF;i++) if($i ~ /\\.timer$/){print $i; break}}'");
        std::istringstream uss(units);
        std::string unit;
        while (std::getline(uss, unit)) {
            unit = trim(unit);
            if (unit.empty()) continue;

            CronTask t;
            t.name = unit;
            if (t.name.size() > 6 && t.name.substr(t.name.size() - 6) == ".timer")
                t.name = t.name.substr(0, t.name.size() - 6);

            std::string desc = exec_cmd("systemctl show " + unit + " -p Description --value 2>/dev/null");
            std::string next = exec_cmd("systemctl show " + unit + " -p NextElapseUSecRealtime --value 2>/dev/null");
            std::string last = exec_cmd("systemctl show " + unit + " -p LastTriggerUSec --value 2>/dev/null");
            std::string svc  = exec_cmd("systemctl show " + unit + " -p Unit --value 2>/dev/null");

            t.next_run = trim(next);
            t.last_run = trim(last);
            t.schedule = trim(desc); // timer cadence is described by systemd; cron pass overrides if matched

            if (!svc.empty()) {
                std::string execv = exec_cmd("systemctl show " + svc + " -p ExecStart --value 2>/dev/null");
                std::string emain = exec_cmd("systemctl show " + svc + " -p ExecMainStatus --value 2>/dev/null");
                std::string res   = exec_cmd("systemctl show " + svc + " -p Result --value 2>/dev/null");
                std::string emts  = exec_cmd("systemctl show " + svc + " -p ExecMainExitTimestamp --value 2>/dev/null");
                std::string act   = exec_cmd("systemctl is-active " + svc + " 2>/dev/null");
                std::string ena   = exec_cmd("systemctl is-enabled " + svc + " 2>/dev/null");

                t.command = parse_execstart(execv);
                emain = trim(emain);
                t.status = map_status(trim(act), trim(ena), trim(res), emain);
                t.enabled = (t.status != 3);

                // One real history entry from the last invocation's exit status.
                if (!emts.empty() || !emain.empty()) {
                    TaskExecution h;
                    h.timestamp = trim(emts);
                    int ec = 0;
                    if (!emain.empty()) { try { ec = std::stoi(emain); } catch (...) { ec = 0; } }
                    h.exit_code = ec;
                    h.duration_sec = 0.0f; // not exposed without journald parsing
                    t.history.push_back(std::move(h));
                }
            } else {
                t.enabled = true;
                t.status = 0;
            }
            tasks.push_back(std::move(t));
        }
    }

    void apply_cron_schedules() {
        // Read raw cron exprs from /etc/crontab and /etc/cron.d/*; attach the
        // schedule string + command to any task whose command matches, and add
        // standalone cron entries that have no systemd timer.
        std::vector<std::pair<std::string, std::string>> entries; // schedule, command
        auto ingest = [&](const std::string& path, bool has_user) {
            std::ifstream f(path);
            if (!f.is_open()) return;
            std::string line;
            while (std::getline(f, line)) {
                std::string sched, cmd;
                if (parse_cron_line(line, has_user, sched, cmd) && !cmd.empty()) {
                    entries.emplace_back(sched, cmd);
                }
            }
        };
        ingest("/etc/crontab", true);
        // /etc/cron.d/ directory
        if (DIR* d = ::opendir("/etc/cron.d")) {
            struct dirent* de;
            while ((de = ::readdir(d)) != nullptr) {
                std::string nm = de->d_name;
                if (nm == "." || nm == ".." || nm == ".placeholder") continue;
                ingest(std::string("/etc/cron.d/") + nm, true);
            }
            ::closedir(d);
        }
        // User crontab (straylight) -- no user field in these lines.
        std::string usercron = exec_cmd("crontab -l 2>/dev/null");
        {
            std::istringstream uss(usercron);
            std::string line;
            while (std::getline(uss, line)) {
                std::string sched, cmd;
                if (parse_cron_line(line, false, sched, cmd) && !cmd.empty()) {
                    entries.emplace_back(sched, cmd);
                }
            }
        }

        for (auto& e : entries) {
            bool attached = false;
            for (auto& t : tasks) {
                if (!t.command.empty() && e.second.find(t.command) != std::string::npos) {
                    t.schedule = e.first;
                    attached = true;
                    break;
                }
            }
            if (!attached) {
                CronTask t;
                // Derive a name from the command's leading token.
                std::string nm = e.second;
                size_t sp = nm.find_first_of(" \t");
                if (sp != std::string::npos) nm = nm.substr(0, sp);
                size_t slash = nm.find_last_of('/');
                if (slash != std::string::npos) nm = nm.substr(slash + 1);
                t.name = nm.empty() ? std::string("cron") : nm;
                t.command = e.second;
                t.schedule = e.first;
                t.status = 0;
                t.enabled = true;
                tasks.push_back(std::move(t));
            }
        }
    }

    void refresh() {
        ensure_source();
        if (!ok_) return;
        tasks.clear();
        selected_index = -1;
        load_systemd_timers();
        apply_cron_schedules();
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

inline void render_cron_panel(CronState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("cron source unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT CRON");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    if (ImGui::Button("Add Task", ImVec2(120, 30))) {
        st.show_add_dialog = true;
        memset(st.new_name, 0, sizeof(st.new_name));
        memset(st.new_command, 0, sizeof(st.new_command));
        memset(st.new_custom_schedule, 0, sizeof(st.new_custom_schedule));
        memset(st.new_constraints, 0, sizeof(st.new_constraints));
        st.new_schedule_idx = 0;
    }
    ImGui::SameLine();
    bool has_sel = st.selected_index >= 0 && st.selected_index < (int)st.tasks.size();
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Run Now", ImVec2(100, 30))) {
        if (has_sel) {
            st.tasks[st.selected_index].status = 1;
            st.tasks[st.selected_index].last_run = "2026-03-16 10:30:00";
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
    if (ImGui::Button("Delete", ImVec2(80, 30))) { st.show_delete_confirm = true; }
    ImGui::PopStyleColor();
    if (!has_sel) ImGui::EndDisabled();

    ImGui::Spacing();

    // Task table
    float table_h = ImGui::GetContentRegionAvail().y * 0.45f;
    if (ImGui::BeginChild("##task_table_area", ImVec2(0, table_h), false)) {
        if (ImGui::BeginTable("##tasks", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Schedule", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Last Run", ImGuiTableColumnFlags_WidthFixed, 160);
            ImGui::TableSetupColumn("Next Run", ImGuiTableColumnFlags_WidthFixed, 160);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Toggle", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)st.tasks.size(); ++i) {
                auto& t = st.tasks[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                bool sel = (i == st.selected_index);
                if (ImGui::Selectable(t.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                    st.selected_index = i;
                }

                ImGui::TableNextColumn();
                ImGui::Text("%s", t.schedule.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%s", t.last_run.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%s", t.next_run.c_str());

                ImGui::TableNextColumn();
                ImVec4 status_col;
                const char* status_str;
                switch (t.status) {
                    case 0: status_col = ImVec4(0.2f, 1.0f, 0.5f, 1.0f); status_str = "Idle"; break;
                    case 1: status_col = ImVec4(0.4f, 0.7f, 1.0f, 1.0f); status_str = "Running"; break;
                    case 2: status_col = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); status_str = "Failed"; break;
                    default: status_col = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); status_str = "Disabled"; break;
                }
                ImGui::TextColored(status_col, "%s", status_str);

                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Checkbox("##enable", &t.enabled)) {
                    t.status = t.enabled ? 0 : 3;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Spacing();

    // Task detail panel
    if (ImGui::BeginChild("##task_detail", ImVec2(0, -1), false)) {
        if (has_sel) {
            auto& t = st.tasks[st.selected_index];
            ImGui::Columns(2, "##detail_cols", true);

            // Left: task info
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", t.name.c_str());
            ImGui::Separator();
            ImGui::Text("Command:");
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s", t.command.c_str());
            ImGui::InputTextMultiline("##cmd", cmd, sizeof(cmd), ImVec2(-1, 60), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
            ImGui::Text("Schedule: %s", t.schedule.c_str());
            if (!t.resource_constraints.empty())
                ImGui::Text("Constraints: %s", t.resource_constraints.c_str());
            if (!t.dependencies.empty()) {
                ImGui::Text("Dependencies:");
                for (auto& d : t.dependencies) {
                    ImGui::SameLine();
                    ImGui::SmallButton(d.c_str());
                }
            }

            ImGui::NextColumn();

            // Right: execution history
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Execution History");
            ImGui::Separator();
            for (auto& h : t.history) {
                ImVec4 col = h.exit_code == 0 ? ImVec4(0.2f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                ImGui::TextColored(col, "[exit %d]", h.exit_code);
                ImGui::SameLine();
                ImGui::Text("%s (%.1fs)", h.timestamp.c_str(), h.duration_sec);

                if (!h.stdout_text.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::TextWrapped("  stdout: %s", h.stdout_text.c_str());
                    ImGui::PopStyleColor();
                }
                if (!h.stderr_text.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
                    ImGui::TextWrapped("  stderr: %s", h.stderr_text.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
            }

            ImGui::Columns(1);
        } else {
            ImGui::TextDisabled("Select a task to view details");
        }
    }
    ImGui::EndChild();

    // Add Task dialog
    if (st.show_add_dialog) {
        ImGui::OpenPopup("Add Task");
        st.show_add_dialog = false;
    }
    if (ImGui::BeginPopupModal("Add Task", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Task Name:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputText("##task_name", st.new_name, sizeof(st.new_name));

        ImGui::Text("Command:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextMultiline("##task_cmd", st.new_command, sizeof(st.new_command), ImVec2(400, 60));

        ImGui::Text("Schedule:");
        const char* schedules[] = {"Every 5 minutes", "Hourly", "Daily at 3 AM", "Weekly (Sunday)", "Custom..."};
        ImGui::SetNextItemWidth(400);
        ImGui::Combo("##schedule", &st.new_schedule_idx, schedules, 5);
        if (st.new_schedule_idx == 4) {
            ImGui::SetNextItemWidth(400);
            ImGui::InputTextWithHint("##custom_sched", "cron expression (e.g. */5 * * * *)",
                                     st.new_custom_schedule, sizeof(st.new_custom_schedule));
        }

        ImGui::Text("Resource Constraints:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##constraints", "e.g. max-cpu=50% max-mem=2G",
                                 st.new_constraints, sizeof(st.new_constraints));

        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(120, 30))) {
            if (strlen(st.new_name) > 0 && strlen(st.new_command) > 0) {
                CronTask t;
                t.name = st.new_name;
                t.command = st.new_command;
                const char* cron_exprs[] = {"*/5 * * * *", "0 * * * *", "0 3 * * *", "0 0 * * 0"};
                t.schedule = st.new_schedule_idx < 4 ? cron_exprs[st.new_schedule_idx] : st.new_custom_schedule;
                t.last_run = "never";
                t.next_run = "pending";
                t.status = 0;
                t.enabled = true;
                t.resource_constraints = st.new_constraints;
                st.tasks.push_back(t);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Delete confirmation
    if (st.show_delete_confirm) {
        ImGui::OpenPopup("Confirm Delete##cron");
        st.show_delete_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete##cron", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (has_sel) {
            ImGui::Text("Delete task '%s'?", st.tasks[st.selected_index].name.c_str());
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("Delete", ImVec2(120, 30))) {
            if (has_sel) {
                st.tasks.erase(st.tasks.begin() + st.selected_index);
                st.selected_index = -1;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

} // namespace straylight::cron
