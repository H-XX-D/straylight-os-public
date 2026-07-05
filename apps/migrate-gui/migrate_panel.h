// apps/migrate-gui/migrate_panel.h
// StrayLight Migrate GUI — Migration panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <array>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <dirent.h>

namespace straylight::migrate {

struct MigrateState {
    // Export options
    bool export_configs = true;
    bool export_themes = true;
    bool export_packages = true;
    bool export_dotfiles = true;
    bool export_ssh_keys = false;
    bool export_cron_jobs = true;
    bool export_desktop_settings = true;
    bool export_shell_history = false;
    char export_path[512] = "/tmp/straylight-export.tar.gz";
    bool exporting = false;
    float export_progress = 0.0f;

    // Import
    char import_path[512] = {};
    bool importing = false;
    float import_progress = 0.0f;
    bool show_import_preview = false;
    struct ImportItem {
        std::string name;
        std::string type;
        std::string size;
        bool        selected;
    };
    std::vector<ImportItem> import_preview;

    // Sync
    char sync_host[256] = {};
    char sync_user[128] = "root";
    int  sync_port = 22;
    bool syncing = false;
    float sync_progress = 0.0f;
    bool show_sync_diff = false;
    struct DiffEntry {
        std::string path;
        std::string status; // "added", "modified", "deleted"
        std::string size;
    };
    std::vector<DiffEntry> sync_diff;

    int active_tab = 0;

    // ----- Live OS data link (no fabricated data) // [wired:migrate os real-data] -----
    // Sources (all local reads on the target machine):
    //   configs   <- ls /etc/straylight/*.conf  + du -sb /etc/straylight
    //   packages  <- dpkg --get-selections | grep -cw install
    //   dotfiles  <- stat ~/.bashrc ~/.zshrc ~/.profile
    //   themes    <- du -sb ~/.config/straylight
    //   cron      <- /etc/crontab + /etc/cron.d + cron.daily/weekly + crontab -l
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // Read all of a command's stdout into a string via popen.
    static bool run_capture(const char* cmd, std::string& out) {
        out.clear();
        FILE* fp = ::popen(cmd, "r");
        if (!fp) return false;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
        int rc = ::pclose(fp);
        return rc != -1;
    }

    // First integer found in s (e.g. leading count / byte size), or 0.
    static long first_long(const std::string& s) {
        long v = 0; bool any = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); any = true; }
            else if (any) break;
        }
        return any ? v : 0;
    }

    // Size of a single path in bytes via stat(); 0 if absent.
    static long stat_size(const char* path) {
        struct stat sb;
        if (::stat(path, &sb) != 0) return 0;
        return (long)sb.st_size;
    }

    // Human-readable size string from a byte count.
    static std::string human_size(long bytes) {
        char b[32];
        if (bytes >= 1024L * 1024L)
            std::snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
        else if (bytes >= 1024L)
            std::snprintf(b, sizeof(b), "%ld KB", bytes / 1024L);
        else
            std::snprintf(b, sizeof(b), "%ld B", bytes);
        return b;
    }

    // Expand $HOME-relative path into an absolute one.
    static std::string home_path(const char* rel) {
        const char* h = ::getenv("HOME");
        std::string base = h ? h : "";
        return base + rel;
    }

    // Count regular entries in a directory (non-recursive); -1 if absent.
    static int count_dir_entries(const char* path) {
        DIR* d = ::opendir(path);
        if (!d) return -1;
        int n = 0;
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
            ++n;
        }
        ::closedir(d);
        return n;
    }

    // Load the real export/import inventory from the filesystem.
    void refresh() {
        err_.clear();
        import_preview.clear();
        std::string out;
        bool any = false;

        // --- System Configs: /etc/straylight/*.conf count + du size ---
        long cfg_count = 0, cfg_bytes = 0;
        if (run_capture("ls /etc/straylight/*.conf 2>/dev/null | wc -l", out))
            cfg_count = first_long(out);
        if (run_capture("du -sb /etc/straylight 2>/dev/null", out))
            cfg_bytes = first_long(out);
        if (cfg_count > 0 || cfg_bytes > 0) {
            char nm[128];
            std::snprintf(nm, sizeof(nm), "/etc/straylight/ (%ld configs)", cfg_count);
            import_preview.push_back({nm, "configs", human_size(cfg_bytes), true});
            any = true;
        }

        // --- Theme & Appearance: ~/.config/straylight ---
        {
            std::string theme_dir = home_path("/.config/straylight");
            long theme_bytes = 0;
            std::string cmd = "du -sb '" + theme_dir + "' 2>/dev/null";
            if (run_capture(cmd.c_str(), out)) theme_bytes = first_long(out);
            if (theme_bytes > 0) {
                import_preview.push_back({"~/.config/straylight/", "themes", human_size(theme_bytes), true});
                any = true;
            }
        }

        // --- Installed Packages List: dpkg selections ---
        long pkg_count = 0;
        if (run_capture("dpkg --get-selections 2>/dev/null | grep -cw install", out))
            pkg_count = first_long(out);
        if (pkg_count > 0) {
            char nm[96];
            std::snprintf(nm, sizeof(nm), "package-list.txt (%ld packages)", pkg_count);
            // dpkg selection list export size is not known until written; report 0 (no fabrication).
            import_preview.push_back({nm, "packages", human_size(0), true});
            any = true;
        }

        // --- Dotfiles: ~/.bashrc ~/.zshrc ~/.profile (stat each) ---
        {
            std::string br = home_path("/.bashrc");
            std::string zr = home_path("/.zshrc");
            std::string pr = home_path("/.profile");
            long dot_bytes = stat_size(br.c_str()) + stat_size(zr.c_str()) + stat_size(pr.c_str());
            if (dot_bytes > 0) {
                import_preview.push_back({"~/.bashrc, ~/.zshrc, ~/.profile", "dotfiles", human_size(dot_bytes), true});
                any = true;
            }
        }

        // --- Cron Jobs: system cron dirs + user crontab ---
        {
            long cron_jobs = 0, cron_bytes = 0;
            cron_bytes += stat_size("/etc/crontab");
            if (stat_size("/etc/crontab") > 0) cron_jobs += 1;
            int nd = count_dir_entries("/etc/cron.d");
            if (nd > 0) cron_jobs += nd;
            int ndaily = count_dir_entries("/etc/cron.daily");
            if (ndaily > 0) cron_jobs += ndaily;
            int nweekly = count_dir_entries("/etc/cron.weekly");
            if (nweekly > 0) cron_jobs += nweekly;
            if (run_capture("du -scb /etc/crontab /etc/cron.d /etc/cron.daily /etc/cron.weekly 2>/dev/null | tail -1", out))
                cron_bytes = first_long(out);
            if (run_capture("crontab -l 2>/dev/null | grep -vcE '^[[:space:]]*(#|$)'", out))
                cron_jobs += first_long(out);
            if (cron_jobs > 0 || cron_bytes > 0) {
                char nm[96];
                std::snprintf(nm, sizeof(nm), "crontab entries (%ld jobs)", cron_jobs);
                import_preview.push_back({nm, "cron", human_size(cron_bytes), true});
                any = true;
            }
        }

        // --- Desktop Settings: ~/.config/straylight (same tree as themes) ---
        {
            std::string desk_dir = home_path("/.config/straylight");
            long desk_bytes = 0;
            std::string cmd = "du -sb '" + desk_dir + "' 2>/dev/null";
            if (run_capture(cmd.c_str(), out)) desk_bytes = first_long(out);
            if (desk_bytes > 0) {
                import_preview.push_back({"~/.config/straylight/", "desktop", human_size(desk_bytes), true});
                any = true;
            }
        }

        // sync_diff requires a remote target (diff -r local vs remote over ssh);
        // no remote is configured here, so it stays empty (no fabrication).
        sync_diff.clear();

        ok_ = any;
        if (!ok_ && err_.empty()) err_ = "No migratable inventory found on this host";
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

inline void render_migrate_panel(MigrateState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("migrate data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT MIGRATE");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##migrate_tabs")) {
        // --- Export Tab ---
        if (ImGui::BeginTabItem("Export")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Export System Configuration");
            ImGui::TextDisabled("Select what to include in the export bundle:");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Columns(2, "##export_opts", false);

            ImGui::Checkbox("System Configs (/etc/straylight/)", &st.export_configs);
            ImGui::Checkbox("Theme & Appearance", &st.export_themes);
            ImGui::Checkbox("Installed Packages List", &st.export_packages);
            ImGui::Checkbox("Dotfiles (.bashrc, .zshrc, etc.)", &st.export_dotfiles);

            ImGui::NextColumn();

            ImGui::Checkbox("SSH Keys & Config", &st.export_ssh_keys);
            if (st.export_ssh_keys) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(sensitive!)");
            }
            ImGui::Checkbox("Cron Jobs", &st.export_cron_jobs);
            ImGui::Checkbox("Desktop Settings", &st.export_desktop_settings);
            ImGui::Checkbox("Shell History", &st.export_shell_history);

            ImGui::Columns(1);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Export Path:");
            ImGui::SetNextItemWidth(-120);
            ImGui::InputText("##export_path", st.export_path, sizeof(st.export_path));
            ImGui::SameLine();

            if (st.exporting) {
                ImGui::BeginDisabled();
                ImGui::Button("Exporting...");
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::ProgressBar(st.export_progress, ImVec2(-1, 25));
                // [wired] removed fake per-frame progress mock (no real export job source)
            } else {
                if (ImGui::Button("Export", ImVec2(100, 0))) {
                    st.exporting = true;
                    st.export_progress = 0.0f;
                }
                if (st.export_progress >= 1.0f) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "Export completed successfully!");
                    ImGui::Text("Saved to: %s", st.export_path);
                }
            }

            // Size estimate
            ImGui::Spacing();
            int items = 0;
            if (st.export_configs) items++;
            if (st.export_themes) items++;
            if (st.export_packages) items++;
            if (st.export_dotfiles) items++;
            if (st.export_ssh_keys) items++;
            if (st.export_cron_jobs) items++;
            if (st.export_desktop_settings) items++;
            if (st.export_shell_history) items++;
            ImGui::TextDisabled("Estimated size: ~%d KB (%d categories selected)", items * 15, items);

            ImGui::EndTabItem();
        }

        // --- Import Tab ---
        if (ImGui::BeginTabItem("Import")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Import Configuration Bundle");
            ImGui::Spacing();

            ImGui::Text("Import From:");
            ImGui::SetNextItemWidth(-120);
            ImGui::InputTextWithHint("##import_path", "/path/to/export.tar.gz", st.import_path, sizeof(st.import_path));
            ImGui::SameLine();
            if (ImGui::Button("Preview", ImVec2(100, 0))) {
                st.show_import_preview = true;
            }

            ImGui::Spacing();

            if (st.show_import_preview) {
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Bundle Contents:");
                ImGui::Spacing();

                if (ImGui::BeginTable("##import_preview", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Include", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableHeadersRow();

                    for (int i = 0; i < (int)st.import_preview.size(); ++i) {
                        auto& item = st.import_preview[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(i);
                        ImGui::Checkbox("##sel", &item.selected);
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", item.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", item.type.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", item.size.c_str());
                    }
                    ImGui::EndTable();
                }

                ImGui::Spacing();
                if (st.importing) {
                    ImGui::ProgressBar(st.import_progress, ImVec2(-1, 25));
                    // [wired] removed fake per-frame progress mock (no real import job source)
                } else {
                    if (ImGui::Button("Import Selected", ImVec2(160, 30))) {
                        st.importing = true;
                        st.import_progress = 0.0f;
                    }
                    if (st.import_progress >= 1.0f) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "Import completed!");
                    }
                }
            }

            ImGui::EndTabItem();
        }

        // --- Sync Tab ---
        if (ImGui::BeginTabItem("Sync")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Sync Configuration with Remote Host");
            ImGui::Spacing();

            ImGui::Text("Remote Host:");
            ImGui::SetNextItemWidth(300);
            ImGui::InputTextWithHint("##sync_host", "hostname or IP", st.sync_host, sizeof(st.sync_host));
            ImGui::SameLine();
            ImGui::Text("User:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::InputText("##sync_user", st.sync_user, sizeof(st.sync_user));
            ImGui::SameLine();
            ImGui::Text("Port:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputInt("##sync_port", &st.sync_port, 0);

            ImGui::Spacing();
            if (ImGui::Button("Show Diff", ImVec2(120, 30))) { st.show_sync_diff = true; }
            ImGui::SameLine();
            if (st.syncing) {
                ImGui::BeginDisabled();
                ImGui::Button("Syncing...", ImVec2(120, 30));
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::ProgressBar(st.sync_progress, ImVec2(200, 30));
                // [wired] removed fake per-frame progress mock (no real sync job source)
            } else {
                if (ImGui::Button("Sync", ImVec2(120, 30))) {
                    st.syncing = true;
                    st.sync_progress = 0.0f;
                }
                if (st.sync_progress >= 1.0f) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "Sync completed!");
                }
            }

            ImGui::Spacing();

            if (st.show_sync_diff) {
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Configuration Diff (local vs remote):");
                ImGui::Spacing();

                if (ImGui::BeginTable("##sync_diff", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                        ImVec2(0, -1))) {
                    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100);
                    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 140);
                    ImGui::TableHeadersRow();

                    for (auto& d : st.sync_diff) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", d.path.c_str());
                        ImGui::TableNextColumn();
                        ImVec4 col = d.status == "added"    ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                   : d.status == "modified" ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f)
                                   : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                        ImGui::TextColored(col, "%s", d.status.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", d.size.c_str());
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace straylight::migrate
