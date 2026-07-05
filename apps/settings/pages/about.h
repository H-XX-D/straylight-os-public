// apps/settings/pages/about.h
// About this system — OS version, kernel, hardware summary
#pragma once

#include "../settings_page.h"

#include <cstdint>
#include <string>

namespace straylight::settings {

/// About page — reads and displays system identity and hardware summary.
class AboutPage : public SettingsPage {
public:
    AboutPage()  = default;
    ~AboutPage() = default;

    [[nodiscard]] const char* label() const override { return "About"; }

    /// Read OS version, kernel, hardware info from /proc and /etc.
    void load() override;

    /// Render the about panel.
    void render() override;

private:
    std::string os_name_;         // From /etc/straylight-release or /etc/os-release
    std::string os_version_;      // OS version string
    std::string kernel_release_;  // From uname
    std::string kernel_arch_;     // Machine architecture
    std::string hostname_;        // From uname
    std::string cpu_model_;       // From /proc/cpuinfo
    int         cpu_cores_     = 0;
    uint64_t    mem_total_kb_  = 0;  // From /proc/meminfo
    std::string gpu_name_;       // From /sys/class/drm or lspci
    std::string disk_info_;      // Block device summary from /proc/partitions
    std::string uptime_str_;     // Formatted uptime from /proc/uptime

    // Build info
    std::string build_date_;
    std::string build_commit_;
};

} // namespace straylight::settings
