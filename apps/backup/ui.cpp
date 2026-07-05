// apps/backup/ui.cpp
// BackupApp ImGui panels — profile CRUD, schedule editor, history, restore dialog
#include "ui.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight::backup {

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Format a time_point as "YYYY-MM-DD HH:MM:SS".
std::string fmt_time(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    if (t == 0) return "Never";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

/// Format byte count as human-readable string.
std::string fmt_bytes(uint64_t b) {
    constexpr uint64_t KB = 1024;
    constexpr uint64_t MB = 1024 * KB;
    constexpr uint64_t GB = 1024 * MB;
    char buf[32];
    if (b >= GB)      std::snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(GB));
    else if (b >= MB) std::snprintf(buf, sizeof(buf), "%.2f MB", double(b) / double(MB));
    else if (b >= KB) std::snprintf(buf, sizeof(buf), "%.1f KB", double(b) / double(KB));
    else              std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
    return buf;
}

/// Config directory for profiles JSON.
fs::path profiles_path() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".config" / "straylight"
                         : fs::path("/tmp");
    std::error_code ec;
    fs::create_directories(base, ec);
    return base / "backup-profiles.json";
}

/// Draw a left-aligned accent-coloured section header.
void section_header(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

} // namespace

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------

void BackupApp::apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding  = 3.0f;
    s.ItemSpacing    = ImVec2(8.0f, 6.0f);
    s.FramePadding   = ImVec2(6.0f, 4.0f);
    s.WindowPadding  = ImVec2(12.0f, 12.0f);
    s.IndentSpacing  = 16.0f;
    s.ScrollbarSize  = 12.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.14f, 0.14f, 0.22f, 1.0f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f, 0.20f, 0.30f, 1.0f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.25f, 0.25f, 0.38f, 1.0f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    c[ImGuiCol_Button]           = ImVec4(0.0f,  0.55f, 0.38f, 0.8f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.0f,  0.80f, 0.55f, 1.0f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.0f,  1.00f, 0.67f, 1.0f);
    c[ImGuiCol_Header]           = ImVec4(0.0f,  0.55f, 0.38f, 0.6f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.0f,  0.80f, 0.55f, 0.8f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.0f,  1.00f, 0.67f, 1.0f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.0f,  0.80f, 0.55f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.0f,  1.00f, 0.67f, 1.0f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.0f,  1.00f, 0.67f, 1.0f);
    c[ImGuiCol_Separator]        = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    c[ImGuiCol_Border]           = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    c[ImGuiCol_Text]             = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    c[ImGuiCol_PlotHistogram]    = ImVec4(0.0f,  0.80f, 0.55f, 1.0f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.0f, 1.0f, 0.67f, 1.0f);
}

// ---------------------------------------------------------------------------
// Profile persistence
// ---------------------------------------------------------------------------

void BackupApp::load_profiles() {
    const fs::path p = profiles_path();
    if (!fs::exists(p)) return;

    std::ifstream f(p);
    if (!f) return;

    json root;
    try { f >> root; } catch (...) { return; }

    profiles_.clear();
    for (const auto& jp : root.value("profiles", json::array())) {
        BackupProfile bp;
        bp.name           = jp.value("name", "");
        bp.source         = jp.value("source", "");
        bp.destination    = jp.value("destination", "");
        bp.compress       = jp.value("compress", true);
        bp.delete_removed = jp.value("delete_removed", false);
        for (const auto& ex : jp.value("excludes", json::array())) {
            bp.excludes.push_back(ex.get<std::string>());
        }
        profiles_.push_back(std::move(bp));
    }
    SL_INFO("Loaded {} backup profile(s)", profiles_.size());
}

void BackupApp::save_profiles() const {
    json root;
    json jarr = json::array();
    for (const auto& bp : profiles_) {
        json jp;
        jp["name"]           = bp.name;
        jp["source"]         = bp.source.string();
        jp["destination"]    = bp.destination.string();
        jp["compress"]       = bp.compress;
        jp["delete_removed"] = bp.delete_removed;
        jp["excludes"]       = bp.excludes;
        jarr.push_back(std::move(jp));
    }
    root["profiles"] = std::move(jarr);

    std::ofstream f(profiles_path());
    if (f) f << root.dump(2);
}

// ---------------------------------------------------------------------------
// Panel: Profiles (left column)
// ---------------------------------------------------------------------------

void BackupApp::render_profiles() {
    section_header("Backup Profiles");

    // Profile list
    float list_h = 180.0f;
    if (ImGui::BeginChild("##prof_list", ImVec2(0.0f, list_h), true)) {
        for (int i = 0; i < static_cast<int>(profiles_.size()); ++i) {
            bool selected = (sel_ == i);
            char lbl[256];
            std::snprintf(lbl, sizeof(lbl), "%s##prof%d",
                          profiles_[i].name.c_str(), i);
            if (ImGui::Selectable(lbl, selected)) {
                sel_ = i;
                // Pre-fill edit buffers
                const auto& p = profiles_[i];
                std::strncpy(prof_name_, p.name.c_str(), sizeof(prof_name_) - 1);
                std::strncpy(prof_src_,  p.source.c_str(), sizeof(prof_src_) - 1);
                std::strncpy(prof_dst_,  p.destination.c_str(), sizeof(prof_dst_) - 1);
                std::string excl_joined;
                for (size_t j = 0; j < p.excludes.size(); ++j) {
                    if (j) excl_joined += ',';
                    excl_joined += p.excludes[j];
                }
                std::strncpy(prof_excl_, excl_joined.c_str(), sizeof(prof_excl_) - 1);
            }
            // Subtle status dot
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18.0f);
            ImGui::TextDisabled("src: %s", profiles_[i].source.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    section_header("Edit / New Profile");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pname", prof_name_, sizeof(prof_name_));
    ImGui::SameLine(0.0f, 0.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Profile name");

    ImGui::Text("Name"); ImGui::SameLine(80.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pname2", prof_name_, sizeof(prof_name_));

    ImGui::Text("Source"); ImGui::SameLine(80.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##psrc", prof_src_, sizeof(prof_src_));

    ImGui::Text("Dest"); ImGui::SameLine(80.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pdst", prof_dst_, sizeof(prof_dst_));

    ImGui::Text("Exclude"); ImGui::SameLine(80.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pexcl", prof_excl_, sizeof(prof_excl_));
    ImGui::SameLine();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Comma-separated rsync exclude patterns");

    // Options row
    static bool s_compress = true, s_delete = false;
    if (sel_ >= 0 && sel_ < static_cast<int>(profiles_.size())) {
        s_compress = profiles_[sel_].compress;
        s_delete   = profiles_[sel_].delete_removed;
    }
    ImGui::Checkbox("Compress", &s_compress);
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::Checkbox("Delete removed", &s_delete);

    ImGui::Spacing();
    if (ImGui::Button("Save Profile", ImVec2(120.0f, 0.0f))) {
        BackupProfile bp;
        bp.name           = prof_name_;
        bp.source         = prof_src_;
        bp.destination    = prof_dst_;
        bp.compress       = s_compress;
        bp.delete_removed = s_delete;

        // Parse excludes
        std::istringstream ss(prof_excl_);
        std::string token;
        while (std::getline(ss, token, ',')) {
            // trim whitespace
            while (!token.empty() && token.front() == ' ') token.erase(token.begin());
            while (!token.empty() && token.back()  == ' ') token.pop_back();
            if (!token.empty()) bp.excludes.push_back(token);
        }

        if (sel_ >= 0 && sel_ < static_cast<int>(profiles_.size())) {
            profiles_[sel_] = std::move(bp);
        } else {
            profiles_.push_back(std::move(bp));
            sel_ = static_cast<int>(profiles_.size()) - 1;
        }
        save_profiles();
        status_ = "Profile saved.";
    }

    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("New", ImVec2(60.0f, 0.0f))) {
        sel_ = -1;
        std::memset(prof_name_, 0, sizeof(prof_name_));
        std::memset(prof_src_,  0, sizeof(prof_src_));
        std::memset(prof_dst_,  0, sizeof(prof_dst_));
        std::memset(prof_excl_, 0, sizeof(prof_excl_));
    }

    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Delete", ImVec2(70.0f, 0.0f)) &&
        sel_ >= 0 && sel_ < static_cast<int>(profiles_.size())) {
        profiles_.erase(profiles_.begin() + sel_);
        sel_ = -1;
        save_profiles();
        status_ = "Profile deleted.";
    }

    // Run backup button — async
    ImGui::Spacing();
    bool running = active_.valid() &&
                   active_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;

    if (running) {
        ImGui::ProgressBar(progress_, ImVec2(-1.0f, 0.0f));
        ImGui::TextUnformatted(status_.c_str());
    } else {
        // Collect result if finished
        if (active_.valid()) {
            auto res = active_.get();
            if (res.has_value()) {
                const auto& run = res.value();
                status_ = run.success
                    ? "Backup completed: " + fmt_bytes(run.bytes) + " transferred."
                    : "Backup FAILED: " + run.error_msg;
            } else {
                status_ = "Error: " + res.error().message();
            }
            progress_ = 0.0f;
        }

        if (sel_ >= 0 && sel_ < static_cast<int>(profiles_.size())) {
            if (ImGui::Button("Run Backup Now", ImVec2(140.0f, 0.0f))) {
                progress_ = 0.0f;
                status_   = "Running...";
                BackupProfile prof = profiles_[sel_];
                active_ = std::async(std::launch::async,
                    [this, prof = std::move(prof)]() mutable
                        -> Result<BackupRun, SLError>
                    {
                        return engine_.run_backup(prof,
                            [this](int pct, uint64_t /*bytes*/, const std::string& file) {
                                progress_ = float(pct) / 100.0f;
                                status_   = file.empty() ? "Transferring..." : file;
                            });
                    });
            }
        } else {
            ImGui::TextDisabled("Select a profile to run a backup.");
        }
    }
}

// ---------------------------------------------------------------------------
// Panel: Schedule editor
// ---------------------------------------------------------------------------

void BackupApp::render_schedule() {
    section_header("Schedules");

    if (profiles_.empty()) {
        ImGui::TextDisabled("No profiles defined yet.");
        return;
    }

    // Select profile to schedule
    static int prof_idx = 0;
    if (prof_idx >= static_cast<int>(profiles_.size())) prof_idx = 0;

    ImGui::Text("Profile"); ImGui::SameLine(100.0f);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##sched_prof",
                          profiles_[prof_idx].name.c_str())) {
        for (int i = 0; i < static_cast<int>(profiles_.size()); ++i) {
            bool sel = (i == prof_idx);
            if (ImGui::Selectable(profiles_[i].name.c_str(), sel)) prof_idx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Text("Interval"); ImGui::SameLine(100.0f);
    ImGui::SetNextItemWidth(130.0f);
    static const char* kIntervals[] = { "Hourly", "Daily", "Weekly", "Custom" };
    ImGui::Combo("##sched_iv", &sched_interval_, kIntervals, 4);

    if (sched_interval_ == 1 || sched_interval_ == 2) {
        ImGui::Text("Hour (0-23)"); ImGui::SameLine(100.0f);
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("##sched_hour", &sched_hour_);
        sched_hour_ = std::clamp(sched_hour_, 0, 23);
    }

    if (sched_interval_ == 2) {
        static const char* kDays[] = {
            "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
        };
        ImGui::Text("Weekday"); ImGui::SameLine(100.0f);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::Combo("##sched_wd", &sched_weekday_, kDays, 7);
    }

    if (sched_interval_ == 3) {
        ImGui::Text("Every (s)"); ImGui::SameLine(100.0f);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("##sched_cs", &sched_custom_s_);
        if (sched_custom_s_ < 60) sched_custom_s_ = 60;
    }

    ImGui::Checkbox("Enabled", &sched_enabled_);

    ImGui::Spacing();
    if (ImGui::Button("Apply Schedule", ImVec2(130.0f, 0.0f))) {
        Schedule s;
        s.profile_name    = profiles_[prof_idx].name;
        s.interval        = static_cast<Interval>(sched_interval_);
        s.custom_interval = std::chrono::seconds(sched_custom_s_);
        s.hour            = sched_hour_;
        s.weekday         = sched_weekday_;
        s.enabled         = sched_enabled_;
        scheduler_.add(std::move(s));
        (void)scheduler_.save();
        status_ = "Schedule saved.";
    }

    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Remove Schedule", ImVec2(150.0f, 0.0f))) {
        scheduler_.remove(profiles_[prof_idx].name);
        (void)scheduler_.save();
        status_ = "Schedule removed.";
    }

    ImGui::Spacing();
    section_header("Active Schedules");

    if (ImGui::BeginTable("##sched_table", 4,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg   |
                          ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Profile",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Interval", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Last Run", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Enabled",  ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        for (const auto& sch : scheduler_.schedules()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(sch.profile_name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(kIntervals[static_cast<int>(sch.interval)]);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(fmt_time(sch.last_run).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(sch.enabled ? "Yes" : "No");
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Panel: History + Restore dialog
// ---------------------------------------------------------------------------

void BackupApp::render_history() {
    section_header("Backup History");

    if (sel_ < 0 || sel_ >= static_cast<int>(profiles_.size())) {
        ImGui::TextDisabled("Select a profile in the Profiles tab.");
        return;
    }

    const BackupProfile& prof = profiles_[sel_];

    auto hist_res = engine_.history(prof);
    if (!hist_res.has_value()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Cannot read history: %s",
                           hist_res.error().message().c_str());
        return;
    }

    const auto& runs = hist_res.value();
    if (runs.empty()) {
        ImGui::TextDisabled("No runs recorded for this profile.");
        return;
    }

    ImGui::Text("Profile: %s  |  %zu run(s)", prof.name.c_str(), runs.size());
    ImGui::Spacing();

    if (ImGui::BeginTable("##hist_table", 5,
                          ImGuiTableFlags_Borders       |
                          ImGuiTableFlags_RowBg         |
                          ImGuiTableFlags_ScrollY       |
                          ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0.0f, 240.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Timestamp",  ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Status",     ImGuiTableColumnFlags_WidthFixed,  70.0f);
        ImGui::TableSetupColumn("Bytes",      ImGuiTableColumnFlags_WidthFixed,  90.0f);
        ImGui::TableSetupColumn("Snapshot",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Restore",    ImGuiTableColumnFlags_WidthFixed,  65.0f);
        ImGui::TableHeadersRow();

        for (int i = static_cast<int>(runs.size()) - 1; i >= 0; --i) {
            const auto& r = runs[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(fmt_time(r.timestamp).c_str());

            ImGui::TableSetColumnIndex(1);
            if (r.success) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "OK");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),  "FAIL");
                if (ImGui::IsItemHovered() && !r.error_msg.empty())
                    ImGui::SetTooltip("%s", r.error_msg.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(fmt_bytes(r.bytes).c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.snapshot_dir.c_str());

            ImGui::TableSetColumnIndex(4);
            char btn_lbl[32];
            std::snprintf(btn_lbl, sizeof(btn_lbl), "Restore##r%d", i);
            if (ImGui::SmallButton(btn_lbl)) {
                restore_snap_idx_ = i;
                std::memset(restore_dst_, 0, sizeof(restore_dst_));
                ImGui::OpenPopup("RestoreDialog");
            }
        }
        ImGui::EndTable();
    }

    // --- Restore Dialog -------------------------------------------------
    ImGui::SetNextWindowSize(ImVec2(420.0f, 170.0f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("RestoreDialog", nullptr,
                               ImGuiWindowFlags_NoResize)) {
        ImGui::Text("Restore snapshot to directory:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##restore_dst", restore_dst_, sizeof(restore_dst_));
        ImGui::Spacing();

        if (ImGui::Button("Restore", ImVec2(100.0f, 0.0f))) {
            if (restore_snap_idx_ >= 0 &&
                restore_snap_idx_ < static_cast<int>(runs.size()) &&
                restore_dst_[0] != '\0') {

                const auto& snap = runs[restore_snap_idx_];
                auto res = engine_.restore(prof, fs::path(restore_dst_),
                                            snap.timestamp);
                status_ = res.has_value()
                    ? "Restore completed."
                    : "Restore failed: " + res.error().message();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button("Cancel", ImVec2(80.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Status bar
    if (!status_.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted(status_.c_str());
    }
}

// ---------------------------------------------------------------------------
// Top-level run()
// ---------------------------------------------------------------------------

int BackupApp::run(int /*argc*/, char* /*argv*/[]) {
    load_profiles();
    (void)scheduler_.load();
    // Scheduler is started from main after EGL/ImGui init to avoid races;
    // here we just signal readiness.
    return 0;
}

} // namespace straylight::backup
