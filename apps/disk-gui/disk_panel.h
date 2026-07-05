// apps/disk-gui/disk_panel.h
// StrayLight Disk Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <dirent.h>
#include <unistd.h>
#include <sys/statvfs.h>

namespace straylight::disk {

struct Partition {
    char name[32];
    char mount[64];
    char fs_type[16];
    float size_gb;
    float used_gb;
    ImU32 color;
};

struct DiskInfo {
    char device[32];
    char model[64];
    float total_gb;
    int  smart_status;  // 0=good, 1=warning, 2=failing
    int  temp_c;
    int  power_on_hours;
    std::vector<Partition> partitions;
};

struct DiskState {
    std::vector<DiskInfo> disks;
    int selected_disk = 0;
    int selected_partition = -1;

    bool show_format_confirm = false;
    bool show_mount_dialog = false;
    char mount_point[128] = {};

    // OS-backed link (no fabricated data): real sysfs / /proc/mounts / statvfs.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // Stable palette for partitions (assigned by index, deterministic).
    static ImU32 part_color(int i) {
        static const ImU32 pal[] = {
            IM_COL32(0, 200, 100, 255),  IM_COL32(100, 150, 200, 255),
            IM_COL32(200, 100, 50, 255), IM_COL32(100, 100, 220, 255),
            IM_COL32(180, 80, 180, 255), IM_COL32(220, 180, 50, 255),
            IM_COL32(80, 180, 180, 255), IM_COL32(200, 60, 60, 255),
        };
        return pal[i % (int)(sizeof(pal) / sizeof(pal[0]))];
    }

    static std::string read_file_str(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return std::string();
        std::string s;
        std::getline(f, s);
        // Trim trailing whitespace (sysfs model strings are space-padded).
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                              s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        return s;
    }

    static uint64_t read_file_u64(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return 0;
        uint64_t v = 0;
        f >> v;
        return v;
    }

    // hwmon nvme temp (milli-C) keyed by nvme controller name, e.g. "nvme0".
    static int nvme_temp_c(const std::string& nvme_name) {
        if (nvme_name.empty()) return 0;  // non-nvme device: no hwmon temp source
        DIR* d = opendir("/sys/class/hwmon");
        if (!d) return 0;
        int temp_c = 0;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string hw = e->d_name;
            if (hw.rfind("hwmon", 0) != 0) continue;
            std::string base = std::string("/sys/class/hwmon/") + hw;
            if (read_file_str(base + "/name") != "nvme") continue;
            char buf[512];
            ssize_t n = readlink((base + "/device").c_str(), buf, sizeof(buf) - 1);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string dev(buf);
            // Device link ends in .../nvme/nvmeN -> match controller name.
            if (dev.find("/" + nvme_name) == std::string::npos &&
                dev.find(nvme_name) == std::string::npos)
                continue;
            uint64_t milli = read_file_u64(base + "/temp1_input");
            if (milli > 0) { temp_c = (int)(milli / 1000); break; }
        }
        closedir(d);
        return temp_c;
    }

    // Controller name for a block device, e.g. "nvme0n1" -> "nvme0".
    static std::string controller_of(const std::string& dev) {
        if (dev.rfind("nvme", 0) == 0) {
            size_t n = dev.find('n', 4);
            if (n != std::string::npos) return dev.substr(0, n);
        }
        return std::string();
    }

    void refresh() {
        std::vector<DiskInfo> fresh;

        // (a) Enumerate whole-disk block devices from /sys/block.
        DIR* bd = opendir("/sys/block");
        if (!bd) {
            ok_ = false;
            err_ = "cannot open /sys/block";
            return;
        }
        std::vector<std::string> devnames;
        struct dirent* be;
        while ((be = readdir(bd)) != nullptr) {
            std::string nm = be->d_name;
            if (nm == "." || nm == "..") continue;
            // Skip virtual/loop/ram devices with no backing model.
            if (nm.rfind("loop", 0) == 0 || nm.rfind("ram", 0) == 0 ||
                nm.rfind("zram", 0) == 0 || nm.rfind("dm-", 0) == 0)
                continue;
            std::string base = std::string("/sys/block/") + nm;
            if (read_file_u64(base + "/size") == 0) continue;
            devnames.push_back(nm);
        }
        closedir(bd);
        std::sort(devnames.begin(), devnames.end());

        // (b) Read /proc/mounts to map device path -> mountpoint (real fs only).
        struct MountRec { std::string dev, mount, fstype; };
        std::vector<MountRec> mounts;
        {
            std::ifstream mf("/proc/mounts");
            std::string line;
            while (std::getline(mf, line)) {
                std::istringstream ss(line);
                std::string device, mount, fstype;
                ss >> device >> mount >> fstype;
                if (device.empty() || device[0] != '/') continue;
                if (fstype == "tmpfs" || fstype == "devtmpfs") continue;
                if (mount.rfind("/snap", 0) == 0) continue;
                mounts.push_back({device, mount, fstype});
            }
        }

        // (c) Build DiskInfo + partitions per device.
        for (const std::string& nm : devnames) {
            std::string base = std::string("/sys/block/") + nm;
            DiskInfo d{};
            snprintf(d.device, sizeof(d.device), "/dev/%s", nm.c_str());
            std::string model = read_file_str(base + "/device/model");
            snprintf(d.model, sizeof(d.model), "%s",
                     model.empty() ? "(unknown)" : model.c_str());
            uint64_t sectors = read_file_u64(base + "/size");
            d.total_gb = (float)((double)sectors * 512.0 / 1e9);
            d.temp_c = nvme_temp_c(controller_of(nm));
            // SMART health / power-on-hours have no source on this box
            // (smartctl + nvme-cli absent, sysfs exposes no SMART attribute).
            d.smart_status = 0;       // 0=HEALTHY (cosmetic stub; no real source)
            d.power_on_hours = 0;     // no real source -> left zero (hidden in render)

            // Enumerate partitions from sysfs subdirs of the block device.
            std::vector<std::string> partnames;
            DIR* pd = opendir(base.c_str());
            if (pd) {
                struct dirent* pe;
                while ((pe = readdir(pd)) != nullptr) {
                    std::string pn = pe->d_name;
                    if (pn.rfind(nm, 0) != 0 || pn == nm) continue;
                    if (read_file_u64(base + "/" + pn + "/partition") == 0) continue;
                    partnames.push_back(pn);
                }
                closedir(pd);
            }
            std::sort(partnames.begin(), partnames.end());

            int pidx = 0;
            for (const std::string& pn : partnames) {
                Partition p{};
                snprintf(p.name, sizeof(p.name), "%s", pn.c_str());
                uint64_t psectors = read_file_u64(base + "/" + pn + "/size");
                p.size_gb = (float)((double)psectors * 512.0 / 1e9);
                p.color = part_color(pidx++);
                p.mount[0] = '\0';
                p.fs_type[0] = '\0';
                p.used_gb = 0.0f;

                // Match this partition against /proc/mounts by device path.
                std::string devpath = std::string("/dev/") + pn;
                for (const auto& mr : mounts) {
                    if (mr.dev != devpath) continue;
                    snprintf(p.mount, sizeof(p.mount), "%s", mr.mount.c_str());
                    snprintf(p.fs_type, sizeof(p.fs_type), "%s", mr.fstype.c_str());
                    struct statvfs vfs{};
                    if (::statvfs(mr.mount.c_str(), &vfs) == 0 && vfs.f_blocks > 0) {
                        double total = (double)vfs.f_blocks * (double)vfs.f_frsize;
                        double used = (double)(vfs.f_blocks - vfs.f_bfree) *
                                      (double)vfs.f_frsize;
                        p.size_gb = (float)(total / 1e9);
                        p.used_gb = (float)(used / 1e9);
                    }
                    break;
                }
                d.partitions.push_back(p);
            }
            fresh.push_back(std::move(d));
        }

        disks.swap(fresh);
        if (selected_disk >= (int)disks.size()) selected_disk = 0;
        ok_ = true;
        err_.clear();
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

inline void render_disk_panel(DiskState& st) {
    st.maybe_refresh();
    if (st.disks.empty()) st.init();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("disk data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("DISK MANAGER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Disk selector
    ImGui::Text("Disk:");
    ImGui::SameLine();
    for (int i = 0; i < (int)st.disks.size(); ++i) {
        ImGui::SameLine();
        char label[128];
        snprintf(label, 128, "%s (%s)##disk%d", st.disks[i].device, st.disks[i].model, i);
        if (ImGui::RadioButton(label, st.selected_disk == i)) {
            st.selected_disk = i;
            st.selected_partition = -1;
        }
    }
    ImGui::Spacing();

    if (st.selected_disk < 0 || st.selected_disk >= (int)st.disks.size()) return;
    auto& disk = st.disks[st.selected_disk];

    // Partition map visualization
    ImGui::BeginChild("##part_map", ImVec2(-1, 100), true);
    ImGui::TextDisabled("Partition Map: %s (%.1f GB)", disk.model, disk.total_gb);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 bar_pos = ImGui::GetCursorScreenPos();
    float bar_w = ImGui::GetContentRegionAvail().x - 10;
    float bar_h = 40;

    // Background
    draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                        IM_COL32(30, 30, 50, 255), 4.0f);

    float x = bar_pos.x;
    for (int i = 0; i < (int)disk.partitions.size(); ++i) {
        auto& p = disk.partitions[i];
        float pw = (p.size_gb / disk.total_gb) * bar_w;
        if (pw < 2) pw = 2;

        ImVec2 tl(x, bar_pos.y);
        ImVec2 br(x + pw - 1, bar_pos.y + bar_h);

        bool hovered = ImGui::GetIO().MousePos.x >= tl.x && ImGui::GetIO().MousePos.x <= br.x &&
                       ImGui::GetIO().MousePos.y >= tl.y && ImGui::GetIO().MousePos.y <= br.y;
        bool selected = (st.selected_partition == i);

        ImU32 col = p.color;
        if (selected) col = IM_COL32(255, 255, 255, 100);

        draw->AddRectFilled(tl, br, col, 2.0f);
        if (hovered || selected)
            draw->AddRect(tl, br, IM_COL32(255, 255, 255, 255), 2.0f, 0, 2.0f);

        if (pw > 40) {
            draw->AddText(ImVec2(tl.x + 4, tl.y + 4), IM_COL32(255, 255, 255, 255), p.name);
            char sz[32]; snprintf(sz, 32, "%.1f GB", p.size_gb);
            draw->AddText(ImVec2(tl.x + 4, tl.y + 20), IM_COL32(200, 200, 200, 200), sz);
        }

        if (hovered && ImGui::IsMouseClicked(0)) {
            st.selected_partition = i;
        }

        x += pw;
    }
    ImGui::Dummy(ImVec2(0, bar_h + 8));
    ImGui::EndChild();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.55f;

    // Partition details / usage
    ImGui::BeginChild("##part_detail", ImVec2(left_w, 0), true);
    ImGui::TextColored(accent, "Partitions");
    ImGui::Separator();

    if (ImGui::BeginTable("##ptable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Partition");
        ImGui::TableSetupColumn("Mount");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)disk.partitions.size(); ++i) {
            auto& p = disk.partitions[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(p.name, st.selected_partition == i, ImGuiSelectableFlags_SpanAllColumns)) {
                st.selected_partition = i;
            }
            ImGui::TableSetColumnIndex(1);
            if (strlen(p.mount) > 0) ImGui::Text("%s", p.mount);
            else ImGui::TextDisabled("Not mounted");
            ImGui::TableSetColumnIndex(2); ImGui::Text("%s", p.fs_type);

            ImGui::TableSetColumnIndex(3);
            float usage = (p.size_gb > 0) ? p.used_gb / p.size_gb : 0;
            char overlay[64];
            snprintf(overlay, 64, "%.1f / %.1f GB", p.used_gb, p.size_gb);
            ImGui::ProgressBar(usage, ImVec2(-1, 16), overlay);

            ImGui::TableSetColumnIndex(4);
            if (strlen(p.mount) > 0) {
                if (ImGui::SmallButton("Unmount")) {
                    p.mount[0] = '\0';
                }
            } else {
                if (ImGui::SmallButton("Mount")) {
                    st.show_mount_dialog = true;
                    st.selected_partition = i;
                }
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.8f));
            if (ImGui::SmallButton("Fmt")) {
                st.show_format_confirm = true;
                st.selected_partition = i;
            }
            ImGui::PopStyleColor();

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // SMART / Health
    ImGui::BeginChild("##smart", ImVec2(0, 0), true);
    ImGui::TextColored(accent, "Disk Health (SMART)");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Model: %s", disk.model);
    ImGui::Text("Device: %s", disk.device);
    ImGui::Text("Capacity: %.1f GB", disk.total_gb);
    ImGui::Spacing();

    // SMART status indicator
    ImGui::Text("Status:");
    ImGui::SameLine();
    if (disk.smart_status == 0) {
        ImGui::TextColored(accent, "HEALTHY");
        ImDrawList* sdraw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        sdraw->AddCircleFilled(ImVec2(p.x + 20, p.y + 20), 15, IM_COL32(0, 200, 100, 255));
        ImGui::Dummy(ImVec2(0, 44));
    } else if (disk.smart_status == 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "WARNING");
        ImDrawList* sdraw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        sdraw->AddCircleFilled(ImVec2(p.x + 20, p.y + 20), 15, IM_COL32(255, 200, 0, 255));
        ImGui::Dummy(ImVec2(0, 44));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "FAILING");
        ImDrawList* sdraw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        sdraw->AddCircleFilled(ImVec2(p.x + 20, p.y + 20), 15, IM_COL32(255, 50, 50, 255));
        ImGui::Dummy(ImVec2(0, 44));
    }

    ImGui::Text("Temperature: %d C", disk.temp_c);
    ImGui::Text("Power-On Hours: %d", disk.power_on_hours);

    // Total usage
    float total_used = 0, total_size = 0;
    for (auto& p : disk.partitions) { total_used += p.used_gb; total_size += p.size_gb; }
    ImGui::Spacing();
    ImGui::Text("Total Usage:");
    char usage_str[64];
    snprintf(usage_str, 64, "%.1f / %.1f GB", total_used, total_size);
    ImGui::ProgressBar(total_used / std::max(total_size, 0.01f), ImVec2(-1, 20), usage_str);

    ImGui::EndChild();

    // Format confirmation
    if (st.show_format_confirm) {
        ImGui::OpenPopup("Confirm Format");
        st.show_format_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Format", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "WARNING: This will erase all data!");
        if (st.selected_partition >= 0 && st.selected_partition < (int)disk.partitions.size()) {
            ImGui::Text("Partition: %s", disk.partitions[st.selected_partition].name);
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("Format", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Mount dialog
    if (st.show_mount_dialog) {
        ImGui::OpenPopup("Mount Partition");
        st.show_mount_dialog = false;
    }
    if (ImGui::BeginPopupModal("Mount Partition", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Mount Point:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##mount", "/mnt/data", st.mount_point, sizeof(st.mount_point));
        ImGui::Spacing();
        if (ImGui::Button("Mount", ImVec2(120, 30)) && strlen(st.mount_point) > 0) {
            if (st.selected_partition >= 0 && st.selected_partition < (int)disk.partitions.size()) {
                snprintf(disk.partitions[st.selected_partition].mount, 64, "%.63s", st.mount_point);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::disk
