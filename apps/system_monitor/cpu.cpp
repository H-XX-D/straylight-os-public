// apps/system_monitor/cpu.cpp
// CPU usage monitoring — reads /proc/stat for per-core utilization
#include "cpu.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace straylight::sysmon {

CpuMonitor::CpuMonitor() = default;

Result<void, std::string> CpuMonitor::sample() {
    auto result = read_proc_stat();
    if (!result.has_value()) {
        return result;
    }

    if (first_sample_) {
        read_cpu_info();
        first_sample_ = false;
    }

    read_temperature();
    read_frequency();

    return Result<void, std::string>::ok();
}

Result<void, std::string> CpuMonitor::read_proc_stat() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot open /proc/stat");
    }

    std::string line;
    CpuTimes total_times;
    std::vector<CpuTimes> core_times;

    while (std::getline(file, line)) {
        if (line.compare(0, 3, "cpu") != 0) break;

        CpuTimes times;
        char name[16] = {};

        int scanned = sscanf(line.c_str(),
            "%15s %lu %lu %lu %lu %lu %lu %lu %lu",
            name,
            &times.user, &times.nice, &times.system, &times.idle,
            &times.iowait, &times.irq, &times.softirq, &times.steal);

        if (scanned < 5) continue;

        if (strcmp(name, "cpu") == 0) {
            // Aggregate CPU line
            if (!first_sample_) {
                uint64_t total_diff = times.total() - prev_total_.total();
                uint64_t active_diff = times.active() - prev_total_.active();
                if (total_diff > 0) {
                    info_.total_usage = static_cast<float>(active_diff) /
                                        static_cast<float>(total_diff) * 100.0f;
                }
                info_.total_history.push_back(info_.total_usage);
                while (static_cast<int>(info_.total_history.size()) > CpuInfo::kMaxHistory) {
                    info_.total_history.pop_front();
                }
            }
            total_times = times;
        } else {
            // Per-core line (cpu0, cpu1, ...)
            core_times.push_back(times);
        }
    }

    // Calculate per-core usage
    if (!first_sample_ && core_times.size() == prev_cores_.size()) {
        info_.core_count = static_cast<int>(core_times.size());

        if (info_.cores.size() != core_times.size()) {
            info_.cores.resize(core_times.size());
            for (size_t i = 0; i < core_times.size(); ++i) {
                info_.cores[i].core_id = static_cast<int>(i);
            }
        }

        for (size_t i = 0; i < core_times.size(); ++i) {
            uint64_t total_diff = core_times[i].total() - prev_cores_[i].total();
            uint64_t active_diff = core_times[i].active() - prev_cores_[i].active();

            float usage = 0.0f;
            if (total_diff > 0) {
                usage = static_cast<float>(active_diff) /
                        static_cast<float>(total_diff) * 100.0f;
            }

            info_.cores[i].usage_percent = usage;
            info_.cores[i].history.push_back(usage);
            while (static_cast<int>(info_.cores[i].history.size()) > CoreUsage::kMaxHistory) {
                info_.cores[i].history.pop_front();
            }
        }
    } else if (first_sample_) {
        info_.core_count = static_cast<int>(core_times.size());
        info_.cores.resize(core_times.size());
        for (size_t i = 0; i < core_times.size(); ++i) {
            info_.cores[i].core_id = static_cast<int>(i);
        }
    }

    prev_total_ = total_times;
    prev_cores_ = core_times;

    return Result<void, std::string>::ok();
}

void CpuMonitor::read_cpu_info() {
    std::ifstream file("/proc/cpuinfo");
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 10, "model name") == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                info_.model_name = line.substr(pos + 2);
            }
            break; // Only need one
        }
    }
}

void CpuMonitor::read_temperature() {
    // Try hwmon thermal zones
    std::vector<std::string> paths = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/hwmon/hwmon0/temp1_input",
        "/sys/class/hwmon/hwmon1/temp1_input",
    };

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            int temp_milli = 0;
            file >> temp_milli;
            info_.temperature = static_cast<float>(temp_milli) / 1000.0f;
            return;
        }
    }
}

void CpuMonitor::read_frequency() {
    std::ifstream file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (file.is_open()) {
        int freq_khz = 0;
        file >> freq_khz;
        info_.frequency_mhz = static_cast<float>(freq_khz) / 1000.0f;
    }
}

void CpuMonitor::render() {
    // CPU header
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "CPU");
    if (!info_.model_name.empty()) {
        ImGui::Text("%s", info_.model_name.c_str());
    }
    ImGui::Text("Cores: %d  |  Frequency: %.0f MHz  |  Temp: %.1f C",
               info_.core_count, info_.frequency_mhz, info_.temperature);

    ImGui::Spacing();

    // Total CPU usage graph
    ImGui::Text("Total Usage: %.1f%%", info_.total_usage);
    if (!info_.total_history.empty()) {
        std::vector<float> history(info_.total_history.begin(),
                                    info_.total_history.end());
        ImGui::PlotLines("##TotalCPU", history.data(),
                         static_cast<int>(history.size()),
                         0, nullptr, 0.0f, 100.0f, ImVec2(0, 80));
    }

    ImGui::Separator();

    // Per-core usage
    ImGui::Text("Per-Core Usage:");

    int cols = std::max(1, std::min(4,
        static_cast<int>(ImGui::GetContentRegionAvail().x / 250.0f)));
    int rows = (info_.core_count + cols - 1) / cols;

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            int idx = row * cols + col;
            if (idx >= info_.core_count) break;

            if (col > 0) ImGui::SameLine();

            ImGui::BeginGroup();

            const auto& core = info_.cores[static_cast<size_t>(idx)];

            // Color based on usage
            ImVec4 color;
            if (core.usage_percent < 50.0f) {
                color = ImVec4(0.0f, 1.0f, 0.67f, 1.0f); // Green
            } else if (core.usage_percent < 80.0f) {
                color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // Yellow
            } else {
                color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red
            }

            ImGui::TextColored(color, "Core %d: %.1f%%",
                              core.core_id, core.usage_percent);

            if (!core.history.empty()) {
                std::vector<float> history(core.history.begin(),
                                            core.history.end());

                ImGui::PushStyleColor(ImGuiCol_PlotLines,
                                      ImGui::GetColorU32(color));
                char label[32];
                snprintf(label, sizeof(label), "##core%d", core.core_id);
                ImGui::PlotLines(label, history.data(),
                                static_cast<int>(history.size()),
                                0, nullptr, 0.0f, 100.0f,
                                ImVec2(220, 40));
                ImGui::PopStyleColor();
            }

            ImGui::EndGroup();
        }
    }
}

} // namespace straylight::sysmon
