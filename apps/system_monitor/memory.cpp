// apps/system_monitor/memory.cpp
// Memory usage monitoring via /proc/meminfo
#include "memory.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight::sysmon {

std::string format_kb(uint64_t kb) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (kb < 1024) {
        ss << kb << " KB";
    } else if (kb < 1024 * 1024) {
        ss << static_cast<double>(kb) / 1024.0 << " MB";
    } else {
        ss << static_cast<double>(kb) / (1024.0 * 1024.0) << " GB";
    }
    return ss.str();
}

MemoryMonitor::MemoryMonitor() = default;

Result<void, std::string> MemoryMonitor::sample() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot open /proc/meminfo");
    }

    std::string line;
    while (std::getline(file, line)) {
        char key[64] = {};
        uint64_t value = 0;

        if (sscanf(line.c_str(), "%63[^:]: %lu kB", key, &value) >= 2) {
            if (strcmp(key, "MemTotal") == 0) {
                info_.total_kb = value;
            } else if (strcmp(key, "MemFree") == 0) {
                info_.free_kb = value;
            } else if (strcmp(key, "MemAvailable") == 0) {
                info_.available_kb = value;
            } else if (strcmp(key, "Buffers") == 0) {
                info_.buffers_kb = value;
            } else if (strcmp(key, "Cached") == 0) {
                info_.cached_kb = value;
            } else if (strcmp(key, "Slab") == 0) {
                info_.slab_kb = value;
            } else if (strcmp(key, "SwapTotal") == 0) {
                info_.swap_total_kb = value;
            } else if (strcmp(key, "SwapFree") == 0) {
                info_.swap_free_kb = value;
            }
        }
    }

    // Update history
    info_.usage_history.push_back(info_.used_percent());
    while (static_cast<int>(info_.usage_history.size()) > MemoryInfo::kMaxHistory) {
        info_.usage_history.pop_front();
    }

    info_.swap_history.push_back(info_.swap_used_percent());
    while (static_cast<int>(info_.swap_history.size()) > MemoryInfo::kMaxHistory) {
        info_.swap_history.pop_front();
    }

    return Result<void, std::string>::ok();
}

void MemoryMonitor::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Memory");
    ImGui::Spacing();

    // RAM usage bar
    float used_pct = info_.used_percent();
    ImVec4 bar_color;
    if (used_pct < 60.0f) {
        bar_color = ImVec4(0.0f, 0.8f, 0.53f, 1.0f);
    } else if (used_pct < 85.0f) {
        bar_color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
    } else {
        bar_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    }

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%s / %s (%.1f%%)",
             format_kb(info_.used_kb()).c_str(),
             format_kb(info_.total_kb).c_str(),
             used_pct);
    ImGui::ProgressBar(used_pct / 100.0f, ImVec2(-1, 25), overlay);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // Memory usage graph
    ImGui::Text("Usage History:");
    if (!info_.usage_history.empty()) {
        std::vector<float> history(info_.usage_history.begin(),
                                    info_.usage_history.end());
        ImGui::PlotLines("##MemUsage", history.data(),
                         static_cast<int>(history.size()),
                         0, nullptr, 0.0f, 100.0f, ImVec2(0, 100));
    }

    ImGui::Separator();

    // Memory breakdown
    ImGui::Columns(2, "memCols", false);
    ImGui::Text("Total:");     ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.total_kb).c_str()); ImGui::NextColumn();
    ImGui::Text("Used:");      ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.used_kb()).c_str()); ImGui::NextColumn();
    ImGui::Text("Free:");      ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.free_kb).c_str()); ImGui::NextColumn();
    ImGui::Text("Available:"); ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.available_kb).c_str()); ImGui::NextColumn();
    ImGui::Text("Buffers:");   ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.buffers_kb).c_str()); ImGui::NextColumn();
    ImGui::Text("Cached:");    ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.cached_kb).c_str()); ImGui::NextColumn();
    ImGui::Text("Slab:");      ImGui::NextColumn();
    ImGui::Text("%s", format_kb(info_.slab_kb).c_str()); ImGui::NextColumn();
    ImGui::Columns(1);

    ImGui::Separator();

    // Swap
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Swap");

    if (info_.swap_total_kb > 0) {
        float swap_pct = info_.swap_used_percent();
        snprintf(overlay, sizeof(overlay), "%s / %s (%.1f%%)",
                 format_kb(info_.swap_used_kb()).c_str(),
                 format_kb(info_.swap_total_kb).c_str(),
                 swap_pct);

        ImVec4 swap_color = swap_pct < 50.0f
                                ? ImVec4(0.0f, 0.8f, 0.53f, 1.0f)
                                : ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, swap_color);
        ImGui::ProgressBar(swap_pct / 100.0f, ImVec2(-1, 20), overlay);
        ImGui::PopStyleColor();

        if (!info_.swap_history.empty()) {
            std::vector<float> history(info_.swap_history.begin(),
                                        info_.swap_history.end());
            ImGui::PlotLines("##SwapUsage", history.data(),
                             static_cast<int>(history.size()),
                             0, nullptr, 0.0f, 100.0f, ImVec2(0, 60));
        }
    } else {
        ImGui::TextDisabled("No swap configured");
    }
}

} // namespace straylight::sysmon
