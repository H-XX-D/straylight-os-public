// apps/hub/dashboard.cpp
#include "dashboard.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/statvfs.h>
#include <sys/sysinfo.h>

namespace straylight::hub {

void Dashboard::sample() {
    if (first_sample_) {
        boot_time_ = std::chrono::steady_clock::now();
        first_sample_ = false;
    }

    sample_cpu();
    sample_memory();
    sample_gpu();
    sample_disks();
    sample_network();
    sample_uptime();
    compute_health_score();
}

void Dashboard::sample_cpu() {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return;

    std::string line;
    if (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string label;
        long long user, nice, sys, idle, iowait, irq, softirq, steal;
        ss >> label >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;

        long long total = user + nice + sys + idle + iowait + irq + softirq + steal;
        long long active = total - idle - iowait;

        if (prev_cpu_total_ > 0) {
            long long d_total = total - prev_cpu_total_;
            long long d_active = active - prev_cpu_active_;
            if (d_total > 0) {
                health_.cpu_usage = static_cast<float>(d_active) / static_cast<float>(d_total);
            }
        }

        prev_cpu_total_ = total;
        prev_cpu_active_ = active;
    }

    // CPU temperature
    std::ifstream temp_f("/sys/class/thermal/thermal_zone0/temp");
    if (temp_f.is_open()) {
        int millic = 0;
        temp_f >> millic;
        health_.cpu_temp_c = millic / 1000;
    }
}

void Dashboard::sample_memory() {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return;

    long long total = 0, available = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("MemTotal:") == 0) {
            std::sscanf(line.c_str(), "MemTotal: %lld", &total);
        } else if (line.find("MemAvailable:") == 0) {
            std::sscanf(line.c_str(), "MemAvailable: %lld", &available);
        }
    }

    health_.mem_total_mb = total / 1024;
    health_.mem_used_mb = (total - available) / 1024;
    if (total > 0) {
        health_.mem_usage = static_cast<float>(total - available) / static_cast<float>(total);
    }
}

void Dashboard::sample_gpu() {
    // AMD/Intel sysfs
    std::ifstream gpu_util("/sys/class/drm/card0/device/gpu_busy_percent");
    if (gpu_util.is_open()) {
        int pct = 0;
        gpu_util >> pct;
        health_.gpu_usage = static_cast<float>(pct) / 100.0f;
    }

    // GPU temperature
    std::ifstream gpu_temp("/sys/class/drm/card0/device/hwmon/hwmon0/temp1_input");
    if (gpu_temp.is_open()) {
        int millic = 0;
        gpu_temp >> millic;
        health_.gpu_temp_c = millic / 1000;
    }
}

void Dashboard::sample_disks() {
    health_.disks.clear();

    // Read /proc/mounts for filesystems
    std::ifstream f("/proc/mounts");
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string device, mount, fstype;
        ss >> device >> mount >> fstype;

        // Only real filesystems
        if (device[0] != '/' || fstype == "tmpfs" || fstype == "devtmpfs") continue;
        if (mount.find("/snap") == 0) continue;

        struct statvfs st{};
        if (::statvfs(mount.c_str(), &st) == 0 && st.f_blocks > 0) {
            SystemHealth::DiskUsage du;
            du.device = device;
            du.mount_point = mount;
            du.total_bytes = st.f_blocks * st.f_frsize;
            du.used_bytes = (st.f_blocks - st.f_bfree) * st.f_frsize;
            du.usage_pct = static_cast<float>(du.used_bytes) / static_cast<float>(du.total_bytes);
            health_.disks.push_back(du);
        }
    }
}

void Dashboard::sample_network() {
    std::ifstream f("/proc/net/dev");
    if (!f.is_open()) return;

    // Save previous values
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> prev;
    for (const auto& iface : health_.net_interfaces) {
        prev[iface.name] = {iface.rx_bytes, iface.tx_bytes};
    }

    health_.net_interfaces.clear();

    std::string line;
    int line_num = 0;
    while (std::getline(f, line)) {
        if (++line_num <= 2) continue; // Skip header lines

        std::istringstream ss(line);
        std::string name;
        ss >> name;
        if (name.empty()) continue;
        if (name.back() == ':') name.pop_back();
        if (name == "lo") continue;

        SystemHealth::NetInterface ni;
        ni.name = name;
        ss >> ni.rx_bytes;

        // Skip: packets, errs, drop, fifo, frame, compressed, multicast
        uint64_t dummy;
        for (int skip = 0; skip < 7; ++skip) ss >> dummy;

        ss >> ni.tx_bytes;

        // Compute rate from previous values
        auto it = prev.find(name);
        if (it != prev.end()) {
            ni.rx_bytes_prev = it->second.first;
            ni.tx_bytes_prev = it->second.second;

            uint64_t rx_diff = (ni.rx_bytes >= ni.rx_bytes_prev) ?
                ni.rx_bytes - ni.rx_bytes_prev : 0;
            uint64_t tx_diff = (ni.tx_bytes >= ni.tx_bytes_prev) ?
                ni.tx_bytes - ni.tx_bytes_prev : 0;

            ni.rx_rate_mbps = static_cast<float>(rx_diff) / (1024.0f * 1024.0f);
            ni.tx_rate_mbps = static_cast<float>(tx_diff) / (1024.0f * 1024.0f);
        }

        // Check interface state
        std::string state_path = "/sys/class/net/" + name + "/operstate";
        std::ifstream state_f(state_path);
        if (state_f.is_open()) {
            std::string state;
            state_f >> state;
            ni.up = (state == "up");
        }

        health_.net_interfaces.push_back(ni);
    }

    // Update network history for graph
    float total_rx = 0.0f, total_tx = 0.0f;
    for (const auto& ni : health_.net_interfaces) {
        total_rx += ni.rx_rate_mbps;
        total_tx += ni.tx_rate_mbps;
    }
    int idx = net_history_idx_ % NET_HISTORY;
    rx_history_[idx] = total_rx;
    tx_history_[idx] = total_tx;
    net_history_idx_++;
}

void Dashboard::sample_uptime() {
    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
        health_.uptime_seconds = static_cast<uint64_t>(si.uptime);
    }
}

void Dashboard::compute_health_score() {
    int score = 100;

    // CPU penalty
    if (health_.cpu_usage > 0.95f) score -= 20;
    else if (health_.cpu_usage > 0.85f) score -= 10;
    else if (health_.cpu_usage > 0.70f) score -= 5;

    // Memory penalty
    if (health_.mem_usage > 0.95f) score -= 25;
    else if (health_.mem_usage > 0.85f) score -= 15;
    else if (health_.mem_usage > 0.70f) score -= 5;

    // Temperature penalty
    if (health_.cpu_temp_c > 95) score -= 20;
    else if (health_.cpu_temp_c > 80) score -= 10;

    if (health_.gpu_temp_c > 95) score -= 15;
    else if (health_.gpu_temp_c > 80) score -= 8;

    // Disk penalty
    for (const auto& disk : health_.disks) {
        if (disk.usage_pct > 0.95f) score -= 15;
        else if (disk.usage_pct > 0.85f) score -= 5;
    }

    health_.health_score = std::max(0, std::min(100, score));
}

void Dashboard::render_gauge(const char* label, float value, ImVec4 color, float radius) {
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 center(cursor.x + radius, cursor.y + radius);
    auto* draw = ImGui::GetWindowDrawList();

    // Background arc
    float arc_start = 3.14159f * 0.75f;
    float arc_end = 3.14159f * 2.25f;
    int segments = 32;

    for (int i = 0; i < segments; ++i) {
        float a1 = arc_start + (arc_end - arc_start) * static_cast<float>(i) / static_cast<float>(segments);
        float a2 = arc_start + (arc_end - arc_start) * static_cast<float>(i + 1) / static_cast<float>(segments);
        ImVec2 p1(center.x + std::cos(a1) * radius * 0.8f, center.y + std::sin(a1) * radius * 0.8f);
        ImVec2 p2(center.x + std::cos(a2) * radius * 0.8f, center.y + std::sin(a2) * radius * 0.8f);
        draw->AddLine(p1, p2, IM_COL32(60, 60, 80, 200), 6.0f);
    }

    // Value arc
    float value_end = arc_start + (arc_end - arc_start) * value;
    ImU32 arc_color = ImGui::ColorConvertFloat4ToU32(color);
    for (int i = 0; i < segments; ++i) {
        float a1 = arc_start + (arc_end - arc_start) * static_cast<float>(i) / static_cast<float>(segments);
        float a2 = arc_start + (arc_end - arc_start) * static_cast<float>(i + 1) / static_cast<float>(segments);
        if (a1 > value_end) break;
        if (a2 > value_end) a2 = value_end;
        ImVec2 p1(center.x + std::cos(a1) * radius * 0.8f, center.y + std::sin(a1) * radius * 0.8f);
        ImVec2 p2(center.x + std::cos(a2) * radius * 0.8f, center.y + std::sin(a2) * radius * 0.8f);
        draw->AddLine(p1, p2, arc_color, 6.0f);
    }

    // Value text
    char val_buf[16];
    std::snprintf(val_buf, sizeof(val_buf), "%.0f%%", value * 100.0f);
    ImVec2 text_size = ImGui::CalcTextSize(val_buf);
    draw->AddText(ImVec2(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.3f),
                  arc_color, val_buf);

    // Label
    ImVec2 label_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(center.x - label_size.x * 0.5f, center.y + radius * 0.3f),
                  IM_COL32(180, 180, 180, 255), label);

    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f + 10.0f));
}

void Dashboard::render_disk_bars() {
    for (const auto& disk : health_.disks) {
        char label[256];
        float used_gb = static_cast<float>(disk.used_bytes) / (1024.0f * 1024.0f * 1024.0f);
        float total_gb = static_cast<float>(disk.total_bytes) / (1024.0f * 1024.0f * 1024.0f);
        std::snprintf(label, sizeof(label), "%s (%.1f/%.1f GB)",
                      disk.mount_point.c_str(), used_gb, total_gb);

        ImVec4 color;
        if (disk.usage_pct >= 0.9f) color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
        else if (disk.usage_pct >= 0.7f) color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
        else color = ImVec4(0.0f, 0.9f, 0.6f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", disk.usage_pct * 100.0f);
        ImGui::ProgressBar(disk.usage_pct, ImVec2(-1.0f, 16.0f), overlay);
        ImGui::PopStyleColor();

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", label);
    }
}

void Dashboard::render_network_graph() {
    int len = std::min(net_history_idx_, NET_HISTORY);
    int offset = net_history_idx_ % NET_HISTORY;

    float ordered_rx[NET_HISTORY]{};
    float ordered_tx[NET_HISTORY]{};
    for (int i = 0; i < len; ++i) {
        int src = (offset - len + i + NET_HISTORY) % NET_HISTORY;
        ordered_rx[i] = rx_history_[src];
        ordered_tx[i] = tx_history_[src];
    }

    float max_val = 1.0f;
    for (int i = 0; i < len; ++i) {
        if (ordered_rx[i] > max_val) max_val = ordered_rx[i];
        if (ordered_tx[i] > max_val) max_val = ordered_tx[i];
    }

    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.9f, 0.6f, 1.0f));
    ImGui::PlotLines("RX", ordered_rx, len, 0, nullptr, 0.0f, max_val * 1.2f, ImVec2(0, 50));
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
    ImGui::PlotLines("TX", ordered_tx, len, 0, nullptr, 0.0f, max_val * 1.2f, ImVec2(0, 50));
    ImGui::PopStyleColor();
}

void Dashboard::render_alerts() {
    if (health_.recent_alerts.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No active alerts");
        return;
    }

    for (const auto& alert : health_.recent_alerts) {
        ImVec4 color;
        if (alert.severity == "critical") color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
        else if (alert.severity == "warning") color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
        else color = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);

        ImGui::TextColored(color, "[%s]", alert.severity.c_str());
        ImGui::SameLine();
        ImGui::Text("%s: %s", alert.category.c_str(), alert.title.c_str());

        if (!alert.detail.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  %s", alert.detail.c_str());
        }
    }
}

void Dashboard::render() {
    // Health score header
    ImVec4 score_color;
    if (health_.health_score >= 80) score_color = ImVec4(0.0f, 0.9f, 0.6f, 1.0f);
    else if (health_.health_score >= 50) score_color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
    else score_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);

    ImGui::TextColored(score_color, "System Health: %d/100", health_.health_score);

    // Uptime
    uint64_t days = health_.uptime_seconds / 86400;
    uint64_t hours = (health_.uptime_seconds % 86400) / 3600;
    uint64_t mins = (health_.uptime_seconds % 3600) / 60;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200.0f);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                       "Uptime: %llud %lluh %llum", static_cast<unsigned long long>(days),
                       static_cast<unsigned long long>(hours), static_cast<unsigned long long>(mins));

    ImGui::Separator();

    // Gauges row
    float gauge_radius = 60.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float gauge_spacing = (avail - gauge_radius * 6.0f) / 4.0f;

    ImVec4 cpu_color = health_.cpu_usage >= 0.9f ? ImVec4(1, 0.2f, 0.2f, 1) :
                       health_.cpu_usage >= 0.7f ? ImVec4(1, 0.8f, 0.2f, 1) :
                       ImVec4(0, 0.9f, 0.6f, 1);
    ImVec4 mem_color = health_.mem_usage >= 0.9f ? ImVec4(1, 0.2f, 0.2f, 1) :
                       health_.mem_usage >= 0.7f ? ImVec4(1, 0.8f, 0.2f, 1) :
                       ImVec4(0.4f, 0.6f, 1.0f, 1);
    ImVec4 gpu_color = health_.gpu_usage >= 0.9f ? ImVec4(1, 0.2f, 0.2f, 1) :
                       health_.gpu_usage >= 0.7f ? ImVec4(1, 0.8f, 0.2f, 1) :
                       ImVec4(1, 0.6f, 0.2f, 1);

    render_gauge("CPU", health_.cpu_usage, cpu_color, gauge_radius);
    ImGui::SameLine(0.0f, gauge_spacing);
    render_gauge("RAM", health_.mem_usage, mem_color, gauge_radius);
    ImGui::SameLine(0.0f, gauge_spacing);
    render_gauge("GPU", health_.gpu_usage, gpu_color, gauge_radius);

    ImGui::Spacing();

    // Temperature display
    if (health_.cpu_temp_c > 0) {
        ImGui::Text("CPU Temp: %dC", health_.cpu_temp_c);
        ImGui::SameLine(200.0f);
    }
    if (health_.gpu_temp_c > 0) {
        ImGui::Text("GPU Temp: %dC", health_.gpu_temp_c);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Disk usage bars
    if (ImGui::CollapsingHeader("Disk Usage", ImGuiTreeNodeFlags_DefaultOpen)) {
        render_disk_bars();
    }

    // Network throughput graph
    if (ImGui::CollapsingHeader("Network Throughput", ImGuiTreeNodeFlags_DefaultOpen)) {
        render_network_graph();

        // Interface list
        for (const auto& ni : health_.net_interfaces) {
            ImVec4 state_color = ni.up ? ImVec4(0.0f, 0.9f, 0.6f, 1.0f)
                                       : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(state_color, "%s", ni.up ? "[UP]" : "[DN]");
            ImGui::SameLine();
            ImGui::Text("%s  RX: %.2f MB/s  TX: %.2f MB/s",
                        ni.name.c_str(), ni.rx_rate_mbps, ni.tx_rate_mbps);
        }
    }

    // Alerts
    if (ImGui::CollapsingHeader("Active Alerts", ImGuiTreeNodeFlags_DefaultOpen)) {
        render_alerts();
    }
}

} // namespace straylight::hub
