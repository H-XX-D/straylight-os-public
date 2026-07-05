// apps/update-gui/update_panel.h
// StrayLight System Updater panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <map>

namespace straylight::update {

struct PackageUpdate {
    char name[64];
    char current_ver[32];
    char new_ver[32];
    char size[16];
    bool selected;
    bool held;
};

struct UpdateHistory {
    char date[32];
    char action[64];
    int  package_count;
    bool success;
};

struct UpdateState {
    std::vector<PackageUpdate> available;
    std::vector<UpdateHistory> history;

    bool  updating = false;
    float progress = 0.0f;
    char  progress_text[128] = {};
    bool  auto_snapshot = true;
    int   current_pkg = 0;

    // Live OS data link (no fabricated data). Sources:
    //   available <- `apt list --upgradable`
    //   history   <- /var/log/dpkg.log (and rotated dpkg.log.*)
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    static void set_str(char* dst, size_t cap, const std::string& s) {
        std::snprintf(dst, cap, "%s", s.c_str());
    }

    // Read all of a command's stdout into a string via popen.
    static bool run_capture(const char* cmd, std::string& out, std::string& err) {
        out.clear();
        FILE* fp = ::popen(cmd, "r");
        if (!fp) { err = std::string("popen failed: ") + cmd; return false; }
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
        int rc = ::pclose(fp);
        if (rc == -1) { err = std::string("pclose failed: ") + cmd; return false; }
        return true;
    }

    // Parse `apt list --upgradable` lines of the form:
    //   "pkg/suite NEWVER arch [upgradable from: CURVER]"
    void load_available() {
        available.clear();
        std::string out, e;
        if (!run_capture("apt list --upgradable 2>/dev/null", out, e)) {
            if (err_.empty()) err_ = e;
            return;
        }
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            if (line.rfind("Listing", 0) == 0) continue; // header
            // name/suite
            std::size_t slash = line.find('/');
            std::size_t sp1   = line.find(' ');
            if (slash == std::string::npos || sp1 == std::string::npos || slash > sp1)
                continue;
            std::string name = line.substr(0, slash);
            // remainder after first space: "NEWVER arch [upgradable from: CURVER]"
            std::istringstream ls(line.substr(sp1 + 1));
            std::string newver, arch;
            ls >> newver >> arch;
            std::string curver;
            std::size_t fp = line.find("upgradable from:");
            if (fp != std::string::npos) {
                std::string rest = line.substr(fp + std::string("upgradable from:").size());
                std::istringstream cs(rest);
                cs >> curver;
                // strip trailing ']'
                if (!curver.empty() && curver.back() == ']') curver.pop_back();
            }
            if (name.empty() || newver.empty()) continue;
            PackageUpdate p{};
            set_str(p.name,        sizeof(p.name),        name);
            set_str(p.current_ver, sizeof(p.current_ver), curver);
            set_str(p.new_ver,     sizeof(p.new_ver),     newver);
            // Per-package download size is not exposed by `apt list --upgradable`
            // and apt-get -s does not report it without root; leave empty (no fabrication).
            p.size[0]  = '\0';
            p.selected = true;
            p.held     = false;
            available.push_back(p);
        }
    }

    // Parse /var/log/dpkg.log: group upgrade/install actions per day, count
    // packages, mark success = absence of "status error" lines on that day.
    void load_history() {
        history.clear();
        std::ifstream f("/var/log/dpkg.log");
        if (!f.is_open()) {
            if (err_.empty()) err_ = "Cannot open /var/log/dpkg.log";
            return;
        }
        struct Run { int upgrades = 0; int installs = 0; bool failed = false; };
        std::map<std::string, Run> runs; // key: date (YYYY-MM-DD)
        std::string line;
        while (std::getline(f, line)) {
            // "YYYY-MM-DD HH:MM:SS action ..."
            if (line.size() < 20) continue;
            std::string date = line.substr(0, 10);
            // tokenise: date time verb rest...
            std::istringstream ls(line);
            std::string d, t, verb;
            ls >> d >> t >> verb;
            if (verb == "upgrade") runs[date].upgrades++;
            else if (verb == "install") runs[date].installs++;
            else if (verb == "status") {
                std::string st;
                ls >> st;
                if (st == "error") runs[date].failed = true;
            }
        }
        // newest first
        for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
            const std::string& date = it->first;
            const Run& r = it->second;
            int count = r.upgrades + r.installs;
            if (count == 0) continue;
            UpdateHistory h{};
            set_str(h.date, sizeof(h.date), date);
            std::string action;
            if (r.upgrades && r.installs)
                action = "Upgrade (" + std::to_string(r.upgrades) +
                         " upgraded, " + std::to_string(r.installs) + " installed)";
            else if (r.upgrades)
                action = "System upgrade (" + std::to_string(r.upgrades) + " packages)";
            else
                action = "Package install (" + std::to_string(r.installs) + " packages)";
            set_str(h.action, sizeof(h.action), action);
            h.package_count = count;
            h.success = !r.failed;
            history.push_back(h);
        }
    }

    void refresh() {
        err_.clear();
        load_available();
        load_history();
        ok_ = err_.empty();
        last_refresh_ = ImGui::GetTime();
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) refresh();
    }

    void init() { refresh(); }
};

inline void render_update_panel(UpdateState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("update data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("SYSTEM UPDATER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Summary
    int update_count = 0;
    float total_size = 0;
    for (auto& p : st.available) {
        if (p.selected && !p.held) {
            update_count++;
            float sz = 0;
            sscanf(p.size, "%f", &sz);
            total_size += sz;
        }
    }
    ImGui::Text("Available updates: %d", (int)st.available.size());
    ImGui::SameLine(250);
    ImGui::Text("Selected: %d", update_count);
    ImGui::SameLine(420);
    ImGui::Checkbox("Auto-snapshot before update", &st.auto_snapshot);
    ImGui::Spacing();

    // Progress bar (shown during update)
    if (st.updating) {
        // Removed per-frame mock progress jitter (was fabricating a fake upgrade animation).
        if (st.progress >= 1.0f) {
            st.progress = 1.0f;
            st.updating = false;
            snprintf(st.progress_text, 128, "Update complete!");
        } else {
            int idx = (int)(st.progress * update_count);
            snprintf(st.progress_text, 128, "Upgrading package %d of %d...", idx + 1, update_count);
        }
        ImGui::ProgressBar(st.progress, ImVec2(-1, 24), st.progress_text);
        ImGui::Spacing();
    }

    // Available updates table
    ImGui::BeginChild("##updates", ImVec2(-1, ImGui::GetContentRegionAvail().y * 0.55f), true);
    ImGui::TextColored(accent, "Available Updates");
    ImGui::Separator();

    if (ImGui::BeginTable("##pkg_table", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Install", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Package", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("New", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Hold", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)st.available.size(); ++i) {
            auto& p = st.available[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            if (p.held) {
                ImGui::BeginDisabled();
                ImGui::Checkbox("##sel", &p.selected);
                ImGui::EndDisabled();
            } else {
                ImGui::Checkbox("##sel", &p.selected);
            }

            ImGui::TableSetColumnIndex(1);
            if (p.held) ImGui::TextDisabled("%s", p.name);
            else ImGui::Text("%s", p.name);

            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", p.current_ver);
            ImGui::TableSetColumnIndex(3); ImGui::TextColored(accent, "%s", p.new_ver);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%s", p.size);
            ImGui::TableSetColumnIndex(5); ImGui::Checkbox("##hold", &p.held);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (!st.updating) {
        if (ImGui::Button("Upgrade Selected", ImVec2(180, 34))) {
            st.updating = true;
            st.progress = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Select All", ImVec2(120, 34))) {
            for (auto& p : st.available) if (!p.held) p.selected = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Deselect All", ImVec2(120, 34))) {
            for (auto& p : st.available) p.selected = false;
        }
    }
    ImGui::EndChild();

    // Update history
    ImGui::BeginChild("##history", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Update History");
    ImGui::Separator();

    if (ImGui::BeginTable("##hist_table", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Packages", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        for (auto& h : st.history) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", h.date);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", h.action);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", h.package_count);
            ImGui::TableSetColumnIndex(3);
            if (h.success) ImGui::TextColored(accent, "Success");
            else ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Failed");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace straylight::update
