// apps/hub/gpu_panel.cpp
#include "gpu_panel.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>

namespace straylight::hub {

static std::string exec_cmd(const std::string& cmd) {
    std::array<char, 4096> buf;
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        result += buf.data();
    }
    ::pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

static int read_sysfs_int(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return 0;
    int val = 0;
    f >> val;
    return val;
}

void GpuPanel::detect_gpus() {
    detected_ = true;
    gpus_.clear();

    // Try NVIDIA first via nvidia-smi
    std::string nv_count = exec_cmd("nvidia-smi --query-gpu=count --format=csv,noheader,nounits 2>/dev/null");
    if (!nv_count.empty()) {
        int count = 0;
        try { count = std::stoi(nv_count); } catch (...) {}

        for (int i = 0; i < count; ++i) {
            GpuInfo gpu;
            gpu.index = i;

            std::string query = "nvidia-smi --query-gpu=name,driver_version,pci.bus_id "
                                "--format=csv,noheader -i " + std::to_string(i) + " 2>/dev/null";
            std::string info = exec_cmd(query);
            if (!info.empty()) {
                // Parse CSV: name, driver, pci_bus
                std::istringstream ss(info);
                std::string part;
                int field = 0;
                while (std::getline(ss, part, ',')) {
                    // Trim whitespace
                    while (!part.empty() && part[0] == ' ') part.erase(0, 1);
                    switch (field) {
                    case 0: gpu.name = part; break;
                    case 1: gpu.driver = "NVIDIA " + part; break;
                    case 2: gpu.pci_bus = part; break;
                    }
                    field++;
                }
            }

            gpus_.push_back(gpu);
        }
    }

    // Try AMD/Intel via DRM sysfs
    DIR* drm = ::opendir("/sys/class/drm/");
    if (drm) {
        struct dirent* entry;
        while ((entry = ::readdir(drm)) != nullptr) {
            std::string name = entry->d_name;
            // Look for card0, card1, etc (not renderD)
            if (name.find("card") != 0 || name.find("render") != std::string::npos) continue;
            if (name.size() > 5) continue; // card0-card9

            std::string base = "/sys/class/drm/" + name + "/device/";

            // Check if this is an actual GPU (has gpu_busy_percent or similar)
            std::string vendor_path = base + "vendor";
            std::ifstream vf(vendor_path);
            if (!vf.is_open()) continue;

            std::string vendor;
            vf >> vendor;

            // Skip if we already detected this GPU via nvidia-smi
            bool is_nvidia = (vendor == "0x10de");
            if (is_nvidia && !gpus_.empty()) continue;

            GpuInfo gpu;
            gpu.index = static_cast<int>(gpus_.size());

            bool is_amd = (vendor == "0x1002");
            bool is_intel = (vendor == "0x8086");

            if (is_amd) {
                gpu.driver = "amdgpu";
                gpu.name = "AMD GPU " + name;
                // Try to get marketing name from product
                std::string product = exec_cmd("cat " + base + "product_name 2>/dev/null");
                if (!product.empty()) gpu.name = product;
            } else if (is_intel) {
                gpu.driver = "i915";
                gpu.name = "Intel GPU " + name;
            } else if (is_nvidia) {
                gpu.driver = "nvidia";
                gpu.name = "NVIDIA GPU " + name;
            } else {
                continue;
            }

            gpus_.push_back(gpu);
        }
        ::closedir(drm);
    }

    if (gpus_.empty()) {
        // Add a placeholder
        GpuInfo gpu;
        gpu.name = "No GPU detected";
        gpu.driver = "none";
        gpus_.push_back(gpu);
    }
}

void GpuPanel::sample_nvidia(GpuInfo& gpu) {
    std::string query = "nvidia-smi --query-gpu=utilization.gpu,utilization.memory,"
                        "memory.used,memory.total,temperature.gpu,power.draw,power.limit,"
                        "fan.speed,clocks.current.graphics,clocks.current.memory "
                        "--format=csv,noheader,nounits -i " +
                        std::to_string(gpu.index) + " 2>/dev/null";
    std::string info = exec_cmd(query);
    if (info.empty()) return;

    std::istringstream ss(info);
    std::string part;
    int field = 0;
    while (std::getline(ss, part, ',')) {
        while (!part.empty() && part[0] == ' ') part.erase(0, 1);
        try {
            switch (field) {
            case 0: gpu.utilization = std::stof(part) / 100.0f; break;
            case 1: gpu.memory_usage = std::stof(part) / 100.0f; break;
            case 2: gpu.memory_used_mb = static_cast<uint64_t>(std::stol(part)); break;
            case 3: gpu.memory_total_mb = static_cast<uint64_t>(std::stol(part)); break;
            case 4: gpu.temperature_c = std::stoi(part); break;
            case 5: gpu.power_draw_w = static_cast<int>(std::stof(part)); break;
            case 6: gpu.power_limit_w = static_cast<int>(std::stof(part)); break;
            case 7: gpu.fan_speed_pct = std::stoi(part); break;
            case 8: gpu.clock_core_mhz = std::stoi(part); break;
            case 9: gpu.clock_memory_mhz = std::stoi(part); break;
            }
        } catch (...) {}
        field++;
    }

    // Recompute memory_usage from actual used/total
    if (gpu.memory_total_mb > 0) {
        gpu.memory_usage = static_cast<float>(gpu.memory_used_mb) /
                           static_cast<float>(gpu.memory_total_mb);
    }
}

void GpuPanel::sample_amd(GpuInfo& gpu) {
    std::string base = "/sys/class/drm/card" + std::to_string(gpu.index) + "/device/";

    gpu.utilization = static_cast<float>(read_sysfs_int(base + "gpu_busy_percent")) / 100.0f;

    int64_t vram_used = 0, vram_total = 0;
    {
        std::ifstream f(base + "mem_info_vram_used");
        if (f.is_open()) f >> vram_used;
    }
    {
        std::ifstream f(base + "mem_info_vram_total");
        if (f.is_open()) f >> vram_total;
    }
    gpu.memory_used_mb = static_cast<uint64_t>(vram_used / (1024 * 1024));
    gpu.memory_total_mb = static_cast<uint64_t>(vram_total / (1024 * 1024));
    if (vram_total > 0) {
        gpu.memory_usage = static_cast<float>(vram_used) / static_cast<float>(vram_total);
    }

    // Temperature from hwmon
    std::string hwmon_base = base + "hwmon/";
    DIR* hwmon = ::opendir(hwmon_base.c_str());
    if (hwmon) {
        struct dirent* entry = ::readdir(hwmon);
        while (entry) {
            std::string n = entry->d_name;
            if (n.find("hwmon") == 0) {
                std::string hwmon_path = hwmon_base + n + "/";
                int temp_milli = read_sysfs_int(hwmon_path + "temp1_input");
                gpu.temperature_c = temp_milli / 1000;

                int fan_input = read_sysfs_int(hwmon_path + "fan1_input");
                int fan_max = read_sysfs_int(hwmon_path + "fan1_max");
                if (fan_max > 0) {
                    gpu.fan_speed_pct = (fan_input * 100) / fan_max;
                }

                int power_avg = read_sysfs_int(hwmon_path + "power1_average");
                gpu.power_draw_w = power_avg / 1000000; // microwatts to watts

                break;
            }
            entry = ::readdir(hwmon);
        }
        ::closedir(hwmon);
    }

    gpu.clock_core_mhz = read_sysfs_int(base + "pp_dpm_sclk");
    gpu.clock_memory_mhz = read_sysfs_int(base + "pp_dpm_mclk");
}

void GpuPanel::sample_intel(GpuInfo& gpu) {
    // Intel GPU sysfs is more limited
    std::string base = "/sys/class/drm/card" + std::to_string(gpu.index) + "/";

    // Check for i915 specific files
    std::string gt_base = base + "gt/gt0/";
    gpu.clock_core_mhz = read_sysfs_int(gt_base + "freq0/cur_freq");
}

void GpuPanel::sample() {
    if (!detected_) {
        detect_gpus();
    }

    for (auto& gpu : gpus_) {
        if (gpu.driver.find("NVIDIA") != std::string::npos || gpu.driver == "nvidia") {
            sample_nvidia(gpu);
        } else if (gpu.driver == "amdgpu") {
            sample_amd(gpu);
        } else if (gpu.driver == "i915") {
            sample_intel(gpu);
        }

        // Update history
        int idx = gpu.history_idx % GpuInfo::HISTORY_LEN;
        gpu.util_history[idx] = gpu.utilization;
        gpu.temp_history[idx] = static_cast<float>(gpu.temperature_c);
        gpu.mem_history[idx] = gpu.memory_usage;
        gpu.history_idx++;
    }
}

void GpuPanel::render_gpu_card(GpuInfo& gpu) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.14f, 1.0f));
    ImGui::BeginChild(("gpu_" + std::to_string(gpu.index)).c_str(),
                       ImVec2(0, 280), true);

    // GPU name and driver
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f), "GPU %d: %s",
                       gpu.index, gpu.name.c_str());
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Driver: %s  PCI: %s",
                       gpu.driver.c_str(), gpu.pci_bus.c_str());

    ImGui::Separator();

    // Utilization bar
    ImVec4 util_color = gpu.utilization >= 0.9f ? ImVec4(1, 0.2f, 0.2f, 1) :
                        gpu.utilization >= 0.7f ? ImVec4(1, 0.8f, 0.2f, 1) :
                        ImVec4(0, 0.9f, 0.6f, 1);
    ImGui::Text("Utilization:");
    ImGui::SameLine(120.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, util_color);
    char util_overlay[16];
    std::snprintf(util_overlay, sizeof(util_overlay), "%.0f%%", gpu.utilization * 100.0f);
    ImGui::ProgressBar(gpu.utilization, ImVec2(-1, 16), util_overlay);
    ImGui::PopStyleColor();

    // VRAM bar
    ImGui::Text("VRAM:");
    ImGui::SameLine(120.0f);
    ImVec4 vram_color = gpu.memory_usage >= 0.9f ? ImVec4(1, 0.2f, 0.2f, 1) :
                        gpu.memory_usage >= 0.7f ? ImVec4(1, 0.8f, 0.2f, 1) :
                        ImVec4(0.4f, 0.6f, 1.0f, 1);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vram_color);
    char vram_overlay[32];
    std::snprintf(vram_overlay, sizeof(vram_overlay), "%llu/%llu MB",
                  static_cast<unsigned long long>(gpu.memory_used_mb),
                  static_cast<unsigned long long>(gpu.memory_total_mb));
    ImGui::ProgressBar(gpu.memory_usage, ImVec2(-1, 16), vram_overlay);
    ImGui::PopStyleColor();

    // Stats row
    ImGui::Columns(4, nullptr, false);

    // Temperature
    ImVec4 temp_color = gpu.temperature_c >= 90 ? ImVec4(1, 0.2f, 0.2f, 1) :
                        gpu.temperature_c >= 75 ? ImVec4(1, 0.8f, 0.2f, 1) :
                        ImVec4(0.7f, 0.7f, 0.7f, 1);
    ImGui::Text("Temp:");
    ImGui::TextColored(temp_color, "%dC", gpu.temperature_c);
    ImGui::NextColumn();

    // Power
    ImGui::Text("Power:");
    if (gpu.power_limit_w > 0) {
        ImGui::Text("%dW / %dW", gpu.power_draw_w, gpu.power_limit_w);
    } else {
        ImGui::Text("%dW", gpu.power_draw_w);
    }
    ImGui::NextColumn();

    // Fan
    ImGui::Text("Fan:");
    ImGui::Text("%d%%", gpu.fan_speed_pct);
    ImGui::NextColumn();

    // Clocks
    ImGui::Text("Clocks:");
    ImGui::Text("%d / %d MHz", gpu.clock_core_mhz, gpu.clock_memory_mhz);
    ImGui::NextColumn();

    ImGui::Columns(1);

    // Mini sparklines
    int len = std::min(gpu.history_idx, GpuInfo::HISTORY_LEN);
    int offset = gpu.history_idx % GpuInfo::HISTORY_LEN;

    float ordered[GpuInfo::HISTORY_LEN];
    for (int i = 0; i < len; ++i) {
        int src = (offset - len + i + GpuInfo::HISTORY_LEN) % GpuInfo::HISTORY_LEN;
        ordered[i] = gpu.util_history[src];
    }
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0, 0.9f, 0.6f, 1));
    ImGui::PlotLines("##util", ordered, len, 0, "Util", 0.0f, 1.0f, ImVec2(0, 30));
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void GpuPanel::render() {
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f), "GPU Overview");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%zu device%s)",
                       gpus_.size(), gpus_.size() != 1 ? "s" : "");
    ImGui::Separator();

    for (auto& gpu : gpus_) {
        render_gpu_card(gpu);
        ImGui::Spacing();
    }
}

} // namespace straylight::hub
