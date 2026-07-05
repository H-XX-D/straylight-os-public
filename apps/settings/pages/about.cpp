// apps/settings/pages/about.cpp
// About page — system identity and hardware summary
#include "about.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>

namespace straylight::settings {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Format seconds to "Xd Xh Xm" uptime string.
std::string format_uptime(double seconds) {
    auto total = static_cast<uint64_t>(seconds);
    uint64_t days    = total / 86400;
    uint64_t hours   = (total % 86400) / 3600;
    uint64_t minutes = (total % 3600) / 60;

    char buf[64];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%lud %luh %lum",
                 (unsigned long)days, (unsigned long)hours,
                 (unsigned long)minutes);
    } else if (hours > 0) {
        snprintf(buf, sizeof(buf), "%luh %lum %lus",
                 (unsigned long)hours, (unsigned long)minutes,
                 (unsigned long)(total % 60));
    } else {
        snprintf(buf, sizeof(buf), "%lum %lus",
                 (unsigned long)minutes, (unsigned long)(total % 60));
    }
    return buf;
}

/// Format kibibytes to human-readable GiB / MiB string.
std::string format_memory(uint64_t kb) {
    char buf[64];
    if (kb >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GiB",
                 static_cast<double>(kb) / (1024.0 * 1024.0));
    } else {
        snprintf(buf, sizeof(buf), "%.0f MiB",
                 static_cast<double>(kb) / 1024.0);
    }
    return buf;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------

void AboutPage::load() {
    // --- OS release -------------------------------------------------------
    // Try /etc/straylight-release first (StrayLight-specific)
    {
        std::ifstream f("/etc/straylight-release");
        if (f.is_open()) {
            std::getline(f, os_name_);
            std::getline(f, os_version_);
        }
    }

    // Fallback: parse /etc/os-release
    if (os_name_.empty()) {
        std::ifstream f("/etc/os-release");
        std::string line;
        while (std::getline(f, line)) {
            auto strip_quotes = [](const std::string& s) -> std::string {
                if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                    return s.substr(1, s.size() - 2);
                }
                return s;
            };

            if (line.compare(0, 8, "PRETTY_N") == 0) {
                auto pos = line.find('=');
                if (pos != std::string::npos) {
                    os_name_ = strip_quotes(line.substr(pos + 1));
                }
            } else if (line.compare(0, 12, "VERSION_ID=\"") == 0 ||
                       line.compare(0, 11, "VERSION_ID=") == 0) {
                auto pos = line.find('=');
                if (pos != std::string::npos) {
                    os_version_ = strip_quotes(line.substr(pos + 1));
                }
            }
        }
    }

    if (os_name_.empty()) os_name_ = "StrayLight OS";

    // --- Build info -------------------------------------------------------
    {
        std::ifstream f("/etc/straylight-build");
        if (f.is_open()) {
            std::getline(f, build_date_);
            std::getline(f, build_commit_);
        }
    }

    // --- Kernel info via uname --------------------------------------------
    {
        struct utsname u{};
        if (uname(&u) == 0) {
            kernel_release_ = u.release;
            kernel_arch_    = u.machine;
            hostname_       = u.nodename;
        }
    }

    // --- CPU info ---------------------------------------------------------
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        int core_count = 0;
        while (std::getline(f, line)) {
            if (line.compare(0, 10, "model name") == 0 && cpu_model_.empty()) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    cpu_model_ = line.substr(pos + 2);
                    // Trim trailing whitespace
                    while (!cpu_model_.empty() &&
                           (cpu_model_.back() == ' ' || cpu_model_.back() == '\t' ||
                            cpu_model_.back() == '\n' || cpu_model_.back() == '\r')) {
                        cpu_model_.pop_back();
                    }
                }
            }
            if (line.compare(0, 9, "processor") == 0) {
                ++core_count;
            }
        }
        cpu_cores_ = core_count;
    }

    // --- Memory info ------------------------------------------------------
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            uint64_t val = 0;
            if (sscanf(line.c_str(), "MemTotal: %lu kB", &val) == 1) {
                mem_total_kb_ = val;
                break;
            }
        }
    }

    // --- GPU info ---------------------------------------------------------
    // Try nvidia-smi first for NVIDIA cards
    gpu_name_.clear();
    {
        FILE* pipe = popen("nvidia-smi --query-gpu=name --format=csv,noheader "
                           "2>/dev/null", "r");
        if (pipe) {
            char buf[256];
            if (fgets(buf, sizeof(buf), pipe)) {
                gpu_name_ = buf;
                // Trim trailing newline/spaces
                while (!gpu_name_.empty() &&
                       (gpu_name_.back() == '\n' || gpu_name_.back() == '\r' ||
                        gpu_name_.back() == ' ')) {
                    gpu_name_.pop_back();
                }
            }
            pclose(pipe);
        }
    }

    // Fallback: scan /sys/class/drm for card names
    if (gpu_name_.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
            std::string name = entry.path().filename().string();
            // Match cardN (not cardN-connector)
            if (name.compare(0, 4, "card") != 0) continue;
            if (name.find('-') != std::string::npos) continue;

            // Try to read device/label
            std::ifstream label_f(entry.path() / "device" / "label");
            if (label_f.is_open()) {
                std::getline(label_f, gpu_name_);
                if (!gpu_name_.empty()) break;
            }

            // Try lspci for PCI device description
            fs::path driver_link = entry.path() / "device" / "driver";
            if (fs::is_symlink(driver_link, ec)) {
                auto target = fs::read_symlink(driver_link, ec);
                if (!ec) gpu_name_ = target.filename().string() + " GPU";
                break;
            }
        }
    }

    if (gpu_name_.empty()) gpu_name_ = "Unknown";

    // --- Disk summary from /proc/partitions -------------------------------
    {
        std::ifstream f("/proc/partitions");
        std::string line;
        std::ostringstream disk_ss;
        int disk_count = 0;
        // Skip header
        std::getline(f, line);
        std::getline(f, line);
        while (std::getline(f, line)) {
            unsigned int major = 0, minor_n = 0;
            unsigned long blocks = 0;
            char dev_name[64] = {};
            if (sscanf(line.c_str(), " %u %u %lu %63s",
                       &major, &minor_n, &blocks, dev_name) >= 4) {
                std::string dn = dev_name;
                // Only show whole disk devices (no partition numbers)
                // e.g. sda, nvme0n1 — not sda1, nvme0n1p1
                bool is_whole = true;
                for (char c : dn) {
                    if (c >= '0' && c <= '9') {
                        // Exclude names ending in a digit (like sda1) but allow
                        // nvme0n1 which ends in a digit for the namespace number
                        // Heuristic: nvme devices have 'n' before final digit sequence
                        if (dn.find("nvme") == std::string::npos) {
                            is_whole = false;
                        }
                        break;
                    }
                }
                // More robust: skip if it matches "sdXN" or "hdXN" patterns
                if (dn.size() >= 3 &&
                    (dn[0] == 's' || dn[0] == 'h') && dn[1] == 'd' &&
                    dn[2] >= 'a' && dn[2] <= 'z' && dn.size() > 3) {
                    is_whole = false;
                }

                if (is_whole && blocks > 1000) {
                    double gb = static_cast<double>(blocks) / (1024.0 * 1024.0);
                    if (disk_count > 0) disk_ss << ", ";
                    disk_ss << dn << " (" << std::fixed;
                    disk_ss.precision(0);
                    disk_ss << gb << " GB)";
                    ++disk_count;
                }
            }
        }
        disk_info_ = disk_ss.str();
        if (disk_info_.empty()) disk_info_ = "Unknown";
    }

    // --- Uptime from /proc/uptime ----------------------------------------
    {
        std::ifstream f("/proc/uptime");
        double uptime_sec = 0.0;
        if (f.is_open()) {
            f >> uptime_sec;
            uptime_str_ = format_uptime(uptime_sec);
        }
    }
}

// ---------------------------------------------------------------------------
// render()
// ---------------------------------------------------------------------------

void AboutPage::render() {
    // Accent header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("About StrayLight OS");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Draw a stylized hexagon logo using the draw list
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pos       = ImGui::GetCursorScreenPos();
    float cx = pos.x + 36.0f;
    float cy = pos.y + 36.0f;
    ImU32 accent_col = IM_COL32(0, 255, 170, 255);
    draw->AddNgonFilled(ImVec2(cx, cy), 30.0f, accent_col, 6);
    draw->AddNgon(ImVec2(cx, cy), 38.0f, IM_COL32(0, 200, 130, 180), 6, 2.0f);
    ImGui::Dummy(ImVec2(0, 75.0f)); // Reserve space for logo

    // --- OS Info ---------------------------------------------------------
    ImGui::SeparatorText("Operating System");

    auto row = [](const char* label, const char* value) {
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 160.0f);
        ImGui::TextDisabled("%s", label);
        ImGui::NextColumn();
        ImGui::TextUnformatted(value);
        ImGui::NextColumn();
        ImGui::Columns(1);
    };

    row("OS:",          os_name_.c_str());
    if (!os_version_.empty())    row("Version:",    os_version_.c_str());
    if (!build_date_.empty())    row("Build Date:", build_date_.c_str());
    if (!build_commit_.empty())  row("Commit:",     build_commit_.c_str());
    row("Hostname:",    hostname_.c_str());
    row("Kernel:",      kernel_release_.c_str());
    row("Architecture:", kernel_arch_.c_str());
    row("Uptime:",      uptime_str_.c_str());

    ImGui::Spacing();

    // --- Hardware --------------------------------------------------------
    ImGui::SeparatorText("Hardware");

    row("CPU:", cpu_model_.c_str());

    char cores_buf[32];
    snprintf(cores_buf, sizeof(cores_buf), "%d logical processors", cpu_cores_);
    row("Cores:", cores_buf);

    row("RAM:",  format_memory(mem_total_kb_).c_str());
    row("GPU:",  gpu_name_.c_str());
    row("Disks:", disk_info_.c_str());

    ImGui::Spacing();

    // --- System Actions --------------------------------------------------
    ImGui::SeparatorText("System");

    if (ImGui::Button("Reload system info", ImVec2(160, 0))) {
        load();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy to clipboard", ImVec2(140, 0))) {
        std::ostringstream info;
        info << os_name_;
        if (!os_version_.empty()) info << " " << os_version_;
        info << "\nKernel: " << kernel_release_ << " (" << kernel_arch_ << ")";
        info << "\nCPU: " << cpu_model_;
        info << "\nRAM: " << format_memory(mem_total_kb_);
        info << "\nGPU: " << gpu_name_;
        ImGui::SetClipboardText(info.str().c_str());
    }
}

} // namespace straylight::settings
