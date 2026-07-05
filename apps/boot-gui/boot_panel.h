// apps/boot-gui/boot_panel.h
// StrayLight Boot Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <sys/stat.h>
#include <glob.h>

namespace straylight::boot {

struct KernelEntry {
    char version[64];
    char path[128];
    char initramfs[128];
    char date[32];
    bool is_default;
    bool is_fallback;
};

struct BootState {
    std::vector<KernelEntry> kernels;
    int  selected_kernel = 0;
    char boot_params[512] = "";
    int  timeout_sec = 5;

    bool show_rebuild_confirm = false;
    bool rebuilding = false;
    float rebuild_progress = 0.0f;

    // Live OS data source (no fabricated kernels). All reads are plain
    // file/stat/glob against /boot, /proc/cmdline, /etc/default/grub.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    static void set_str(char* dst, size_t cap, const std::string& s) {
        if (cap == 0) return;
        size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        memcpy(dst, s.data(), n);
        dst[n] = '\0';
    }

    // Read an entire small text file. Returns false on failure.
    static bool read_file(const std::string& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    // Format a unix mtime as YYYY-MM-DD (local time). Empty on failure.
    static std::string mtime_date(const std::string& path) {
        struct stat stbuf;
        if (stat(path.c_str(), &stbuf) != 0) return std::string();
        std::time_t t = (std::time_t)stbuf.st_mtime;
        std::tm tmv{};
        if (!localtime_r(&t, &tmv)) return std::string();
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        return std::string(buf);
    }

    // Pull the BOOT_IMAGE= basename version from a /proc/cmdline string.
    // e.g. "BOOT_IMAGE=/boot/vmlinuz-6.12.94+deb13-amd64 root=..." -> version.
    static std::string boot_image_version(const std::string& cmdline) {
        const std::string key = "BOOT_IMAGE=";
        size_t p = cmdline.find(key);
        if (p == std::string::npos) return std::string();
        p += key.size();
        size_t e = cmdline.find_first_of(" \t\r\n", p);
        std::string val = cmdline.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
        const std::string pfx = "/boot/vmlinuz-";
        size_t vp = val.find(pfx);
        if (vp != std::string::npos) return val.substr(vp + pfx.size());
        // Fallback: basename after last '/'
        size_t slash = val.rfind('/');
        std::string base = (slash == std::string::npos) ? val : val.substr(slash + 1);
        const std::string vp2 = "vmlinuz-";
        if (base.rfind(vp2, 0) == 0) return base.substr(vp2.size());
        return std::string();
    }

    // Strip surrounding quotes and trailing CR/whitespace from a grub value.
    static std::string unquote(std::string v) {
        // trim trailing whitespace/CR
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' ||
                              v.back() == ' '  || v.back() == '\t')) v.pop_back();
        // trim leading whitespace
        size_t s = 0; while (s < v.size() && (v[s] == ' ' || v[s] == '\t')) ++s;
        v = v.substr(s);
        if (v.size() >= 2 && ((v.front() == '"'  && v.back() == '"') ||
                              (v.front() == '\'' && v.back() == '\''))) {
            v = v.substr(1, v.size() - 2);
        }
        return v;
    }

    // Find KEY=value in /etc/default/grub content (ignores commented lines).
    static bool grub_value(const std::string& grub, const std::string& key, std::string& out) {
        std::istringstream is(grub);
        std::string line;
        while (std::getline(is, line)) {
            size_t s = 0; while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
            if (s < line.size() && line[s] == '#') continue;
            std::string trimmed = line.substr(s);
            if (trimmed.rfind(key + "=", 0) == 0) {
                out = unquote(trimmed.substr(key.size() + 1));
                return true;
            }
        }
        return false;
    }

    void refresh() {
        ok_ = false;
        err_.clear();

        // Enumerate installed kernels via glob of /boot/vmlinuz-*.
        std::vector<std::string> versions;
        glob_t g;
        std::memset(&g, 0, sizeof(g));
        int gr = glob("/boot/vmlinuz-*", 0, nullptr, &g);
        if (gr == 0) {
            const std::string pfx = "/boot/vmlinuz-";
            for (size_t i = 0; i < g.gl_pathc; ++i) {
                std::string full = g.gl_pathv[i];
                if (full.rfind(pfx, 0) == 0)
                    versions.push_back(full.substr(pfx.size()));
            }
        }
        globfree(&g);
        std::sort(versions.begin(), versions.end());

        if (versions.empty()) {
            err_ = "no kernels found in /boot (glob /boot/vmlinuz-* empty)";
            kernels.clear();
            return;
        }

        // Determine the default kernel version from BOOT_IMAGE in /proc/cmdline.
        // grub-editenv saved_entry has no value on this box; cmdline is source.
        std::string cmdline;
        bool have_cmdline = read_file("/proc/cmdline", cmdline);
        std::string default_ver = have_cmdline ? boot_image_version(cmdline) : std::string();

        // Rebuild the kernel list from real data.
        kernels.clear();
        for (const auto& v : versions) {
            KernelEntry k;
            std::memset(&k, 0, sizeof(k));
            std::string path = "/boot/vmlinuz-" + v;
            std::string initramfs = "/boot/initrd.img-" + v;
            set_str(k.version,   sizeof(k.version),   v);
            set_str(k.path,      sizeof(k.path),      path);
            set_str(k.initramfs, sizeof(k.initramfs), initramfs);
            set_str(k.date,      sizeof(k.date),      mtime_date(path));
            k.is_default  = (!default_ver.empty() && v == default_ver);
            // is_fallback is a dracut/Fedora concept with no Debian/GRUB
            // equivalent on this system; leave false (never fabricated).
            k.is_fallback = false;
            kernels.push_back(k);
        }

        // Keep the current selection in range.
        if (selected_kernel < 0 || selected_kernel >= (int)kernels.size())
            selected_kernel = 0;

        // Live boot parameters: prefer the running /proc/cmdline.
        if (have_cmdline) {
            std::string line = cmdline;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            set_str(boot_params, sizeof(boot_params), line);
        } else {
            // Fall back to GRUB_CMDLINE_LINUX* from /etc/default/grub.
            std::string grub;
            if (read_file("/etc/default/grub", grub)) {
                std::string def, base;
                grub_value(grub, "GRUB_CMDLINE_LINUX", base);
                grub_value(grub, "GRUB_CMDLINE_LINUX_DEFAULT", def);
                std::string combined = base;
                if (!def.empty()) { if (!combined.empty()) combined += " "; combined += def; }
                set_str(boot_params, sizeof(boot_params), combined);
            }
        }

        // Timeout from GRUB_TIMEOUT in /etc/default/grub.
        std::string grub;
        if (read_file("/etc/default/grub", grub)) {
            std::string t;
            if (grub_value(grub, "GRUB_TIMEOUT", t)) {
                try { timeout_sec = std::stoi(t); }
                catch (...) { /* leave existing value if unparseable */ }
                if (timeout_sec < 0)  timeout_sec = 0;
                if (timeout_sec > 60) timeout_sec = 60;
            }
        }

        ok_ = true;
        // grub.cfg titles are intentionally not parsed: it is root-only for
        // user straylight (Permission denied). Titles derive from filenames.
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() { refresh(); }
};

inline void render_boot_panel(BootState& st) {
    st.maybe_refresh();
    if (st.kernels.empty()) st.init();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("boot data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("BOOT MANAGER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.45f;

    // Kernel list
    ImGui::BeginChild("##kernel_list", ImVec2(left_w, ImGui::GetContentRegionAvail().y * 0.6f), true);
    ImGui::TextColored(accent, "Installed Kernels");
    ImGui::Separator();

    for (int i = 0; i < (int)st.kernels.size(); ++i) {
        auto& k = st.kernels[i];
        ImGui::PushID(i);

        bool selected = (i == st.selected_kernel);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        char label[256];
        snprintf(label, 256, "%s%s%s", k.version,
                 k.is_default ? " [DEFAULT]" : "",
                 k.is_fallback ? " [FALLBACK]" : "");

        if (ImGui::Selectable(label, selected, 0, ImVec2(0, 40))) {
            st.selected_kernel = i;
        }

        // Default marker badge
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (k.is_default) {
            ImVec2 bp(pos.x + ImGui::GetContentRegionAvail().x - 80, pos.y + 4);
            draw->AddRectFilled(bp, ImVec2(bp.x + 70, bp.y + 18),
                                IM_COL32(0, 100, 60, 255), 3.0f);
            draw->AddText(ImVec2(bp.x + 6, bp.y + 2), IM_COL32(255, 255, 255, 255), "DEFAULT");
        }
        if (k.is_fallback) {
            ImVec2 bp(pos.x + ImGui::GetContentRegionAvail().x - 80, pos.y + 4);
            draw->AddRectFilled(bp, ImVec2(bp.x + 70, bp.y + 18),
                                IM_COL32(120, 80, 0, 255), 3.0f);
            draw->AddText(ImVec2(bp.x + 4, bp.y + 2), IM_COL32(255, 255, 255, 255), "FALLBACK");
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Kernel details
    ImGui::BeginChild("##kernel_detail", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.6f), true);
    if (st.selected_kernel >= 0 && st.selected_kernel < (int)st.kernels.size()) {
        auto& k = st.kernels[st.selected_kernel];

        ImGui::TextColored(accent, "%s", k.version);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Kernel Path:");  ImGui::SameLine(140); ImGui::Text("%s", k.path);
        ImGui::Text("Initramfs:");    ImGui::SameLine(140); ImGui::Text("%s", k.initramfs);
        ImGui::Text("Date:");         ImGui::SameLine(140); ImGui::Text("%s", k.date);
        ImGui::Text("Default:");      ImGui::SameLine(140);
        if (k.is_default) ImGui::TextColored(accent, "Yes");
        else ImGui::Text("No");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!k.is_default && !k.is_fallback) {
            if (ImGui::Button("Set as Default", ImVec2(160, 30))) {
                for (auto& kk : st.kernels) kk.is_default = false;
                k.is_default = true;
            }
            ImGui::SameLine();
        }

        if (ImGui::Button("Rebuild Initramfs", ImVec2(180, 30))) {
            st.show_rebuild_confirm = true;
        }

        if (!k.is_default && !k.is_fallback) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.8f));
            if (ImGui::Button("Remove", ImVec2(120, 30))) {
                st.kernels.erase(st.kernels.begin() + st.selected_kernel);
                st.selected_kernel = 0;
            }
            ImGui::PopStyleColor();
        }

        // Rebuild progress
        if (st.rebuilding) {
            ImGui::Spacing();
            // Per-frame mock removed: rebuild progress was a simulated
            // animation driven by DeltaTime, not real initramfs rebuild
            // state. No real progress source exists, so do not fabricate.
            ImGui::ProgressBar(st.rebuild_progress, ImVec2(-1, 20), "Rebuilding initramfs...");
        }
    }
    ImGui::EndChild();

    // Boot parameters
    ImGui::BeginChild("##boot_params", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Boot Configuration");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Boot Parameters:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##params", st.boot_params, sizeof(st.boot_params), ImVec2(-1, 60));

    ImGui::Spacing();
    ImGui::Text("Timeout:");
    ImGui::SameLine(100);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##timeout", &st.timeout_sec);
    if (st.timeout_sec < 0) st.timeout_sec = 0;
    if (st.timeout_sec > 60) st.timeout_sec = 60;
    ImGui::SameLine();
    ImGui::Text("seconds");

    ImGui::Spacing();
    if (ImGui::Button("Save Configuration", ImVec2(200, 30))) {
        // Save boot config
    }

    ImGui::EndChild();

    // Rebuild confirmation
    if (st.show_rebuild_confirm) {
        ImGui::OpenPopup("Rebuild Initramfs");
        st.show_rebuild_confirm = false;
    }
    if (ImGui::BeginPopupModal("Rebuild Initramfs", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rebuild initramfs for %s?",
                     st.kernels[st.selected_kernel].version);
        ImGui::TextDisabled("This may take a moment.");
        ImGui::Spacing();
        if (ImGui::Button("Rebuild", ImVec2(120, 30))) {
            st.rebuilding = true;
            st.rebuild_progress = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::boot
