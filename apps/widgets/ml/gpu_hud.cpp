// apps/widgets/ml/gpu_hud.cpp
#include "gpu_hud.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::GpuHudWidget, "gpu_hud", "GPU HUD", straylight::widgets::WidgetCategory::ML);
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace straylight::widgets {

// ── nvidia-smi CSV parsing ──────────────────────────────────────────────────

std::vector<GpuInfo> GpuHudWidget::parse_nvidia_smi(const std::string& csv) {
    std::vector<GpuInfo> out;
    std::istringstream ss(csv);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Expected CSV columns:
        // index, name, utilization.gpu [%], memory.used [MiB], memory.total [MiB],
        // temperature.gpu, power.draw [W], power.limit [W], clocks.gr [MHz], clocks.mem [MHz], fan.speed [%]
        GpuInfo g;
        char name_buf[256]{};
        int parsed = std::sscanf(line.c_str(),
            " %d , %255[^,] , %f , %f , %f , %f , %f , %f , %d , %d , %d",
            &g.index, name_buf, &g.utilization_pct,
            &g.memory_used_mb, &g.memory_total_mb,
            &g.temperature_c, &g.power_draw_w, &g.power_limit_w,
            &g.clock_graphics_mhz, &g.clock_memory_mhz, &g.fan_speed_pct);
        if (parsed >= 6) {
            g.name = name_buf;
            // Trim whitespace from name
            while (!g.name.empty() && g.name.front() == ' ') g.name.erase(g.name.begin());
            while (!g.name.empty() && g.name.back() == ' ') g.name.pop_back();
            out.push_back(std::move(g));
        }
    }
    return out;
}

std::vector<GpuInfo> GpuHudWidget::parse_sysfs_amd() {
    std::vector<GpuInfo> out;
    namespace fs = std::filesystem;
    const std::string hwmon_base = "/sys/class/drm";
    int idx = 0;
    for (int card = 0; card < 16; ++card) {
        std::string card_path = hwmon_base + "/card" + std::to_string(card) + "/device";
        if (!fs::exists(card_path)) continue;

        GpuInfo g;
        g.index = idx++;
        g.name = "AMD GPU " + std::to_string(card);

        // GPU busy percent
        {
            std::ifstream f(card_path + "/gpu_busy_percent");
            if (f) { f >> g.utilization_pct; }
        }
        // VRAM used
        {
            std::ifstream f(card_path + "/mem_info_vram_used");
            uint64_t bytes = 0;
            if (f) { f >> bytes; g.memory_used_mb = static_cast<float>(bytes) / (1024.0f * 1024.0f); }
        }
        // VRAM total
        {
            std::ifstream f(card_path + "/mem_info_vram_total");
            uint64_t bytes = 0;
            if (f) { f >> bytes; g.memory_total_mb = static_cast<float>(bytes) / (1024.0f * 1024.0f); }
        }
        // Temperature — search hwmon
        {
            std::string hwmon_dir = card_path + "/hwmon";
            if (fs::exists(hwmon_dir)) {
                for (auto& entry : fs::directory_iterator(hwmon_dir)) {
                    std::ifstream f(entry.path().string() + "/temp1_input");
                    int millideg = 0;
                    if (f) { f >> millideg; g.temperature_c = static_cast<float>(millideg) / 1000.0f; break; }
                }
            }
        }
        // Power
        {
            std::ifstream f(card_path + "/hwmon/hwmon0/power1_average");
            uint64_t microwatts = 0;
            if (f) { f >> microwatts; g.power_draw_w = static_cast<float>(microwatts) / 1e6f; }
        }
        // Clocks
        {
            std::ifstream f(card_path + "/pp_dpm_sclk");
            if (f) {
                std::string ln;
                while (std::getline(f, ln)) {
                    if (ln.find('*') != std::string::npos) {
                        int mhz = 0;
                        std::sscanf(ln.c_str(), "%*d: %dMhz", &mhz);
                        g.clock_graphics_mhz = mhz;
                        break;
                    }
                }
            }
        }

        out.push_back(std::move(g));
    }
    return out;
}

void GpuHudWidget::read_nvidia_smi() {
    FILE* pipe = ::popen(
        "nvidia-smi --query-gpu=index,name,utilization.gpu,memory.used,"
        "memory.total,temperature.gpu,power.draw,power.limit,"
        "clocks.current.graphics,clocks.current.memory,fan.speed "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!pipe) { nvidia_available_ = false; return; }

    std::string output;
    char buf[512];
    while (::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int rc = ::pclose(pipe);
    if (rc != 0) { nvidia_available_ = false; return; }

    nvidia_available_ = true;
    auto parsed = parse_nvidia_smi(output);

    // Merge into existing vector (preserve history)
    for (auto& pg : parsed) {
        bool found = false;
        for (auto& eg : gpus_) {
            if (eg.index == pg.index && eg.name == pg.name) {
                eg.utilization_pct = pg.utilization_pct;
                eg.memory_used_mb = pg.memory_used_mb;
                eg.memory_total_mb = pg.memory_total_mb;
                eg.temperature_c = pg.temperature_c;
                eg.power_draw_w = pg.power_draw_w;
                eg.power_limit_w = pg.power_limit_w;
                eg.clock_graphics_mhz = pg.clock_graphics_mhz;
                eg.clock_memory_mhz = pg.clock_memory_mhz;
                eg.fan_speed_pct = pg.fan_speed_pct;
                found = true;
                break;
            }
        }
        if (!found) {
            gpus_.push_back(std::move(pg));
        }
    }
}

void GpuHudWidget::read_sysfs() {
    auto amd = parse_sysfs_amd();
    if (!amd.empty()) {
        amd_available_ = true;
        for (auto& ag : amd) {
            bool found = false;
            for (auto& eg : gpus_) {
                if (eg.name == ag.name) {
                    eg.utilization_pct = ag.utilization_pct;
                    eg.memory_used_mb = ag.memory_used_mb;
                    eg.memory_total_mb = ag.memory_total_mb;
                    eg.temperature_c = ag.temperature_c;
                    eg.power_draw_w = ag.power_draw_w;
                    eg.clock_graphics_mhz = ag.clock_graphics_mhz;
                    found = true;
                    break;
                }
            }
            if (!found) gpus_.push_back(std::move(ag));
        }
    }
}

void GpuHudWidget::push_history(GpuInfo& gpu) {
    int idx = gpu.history_offset % GpuInfo::kHistoryLen;
    gpu.util_history[idx] = gpu.utilization_pct;
    gpu.temp_history[idx] = gpu.temperature_c;
    gpu.vram_history[idx] = (gpu.memory_total_mb > 0.0f)
        ? (gpu.memory_used_mb / gpu.memory_total_mb) * 100.0f : 0.0f;
    gpu.history_offset++;
}

void GpuHudWidget::update() {
    if (!should_update()) return;
    read_nvidia_smi();
    read_sysfs();
    for (auto& g : gpus_) push_history(g);
}

void GpuHudWidget::render(bool* p_open) {
    if (!ImGui::Begin("GPU HUD", p_open, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }

    if (gpus_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "No GPUs detected");
        ImGui::TextWrapped("Ensure nvidia-smi is in PATH or AMD sysfs is available.");
        ImGui::End();
        return;
    }

    // GPU selector tabs
    if (ImGui::BeginTabBar("##gpu_tabs")) {
        for (int i = 0; i < static_cast<int>(gpus_.size()); ++i) {
            char label[128];
            std::snprintf(label, sizeof(label), "GPU %d: %s###gpu%d",
                          gpus_[i].index, gpus_[i].name.c_str(), i);
            if (ImGui::BeginTabItem(label)) {
                selected_gpu_ = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    auto& gpu = gpus_[selected_gpu_];

    // Utilization bar
    ImGui::Text("Utilization");
    ImGui::SameLine(120);
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%.0f%%", gpu.utilization_pct);
    ImGui::ProgressBar(gpu.utilization_pct / 100.0f, ImVec2(-1, 0), overlay);

    // VRAM bar
    ImGui::Text("VRAM");
    ImGui::SameLine(120);
    std::snprintf(overlay, sizeof(overlay), "%.0f / %.0f MiB",
                  gpu.memory_used_mb, gpu.memory_total_mb);
    float vram_frac = (gpu.memory_total_mb > 0)
        ? gpu.memory_used_mb / gpu.memory_total_mb : 0.0f;
    ImGui::ProgressBar(vram_frac, ImVec2(-1, 0), overlay);

    // Temperature
    ImVec4 temp_color = (gpu.temperature_c > 85.0f) ? ImVec4(1, 0.2f, 0.2f, 1) :
                        (gpu.temperature_c > 70.0f) ? ImVec4(1, 0.8f, 0, 1) :
                                                       ImVec4(0.4f, 1, 0.4f, 1);
    ImGui::Text("Temperature");
    ImGui::SameLine(120);
    ImGui::TextColored(temp_color, "%.0f C", gpu.temperature_c);

    // Power
    ImGui::Text("Power");
    ImGui::SameLine(120);
    ImGui::Text("%.1f / %.1f W", gpu.power_draw_w, gpu.power_limit_w);

    // Clocks
    ImGui::Text("Core Clock");
    ImGui::SameLine(120);
    ImGui::Text("%d MHz", gpu.clock_graphics_mhz);

    ImGui::Text("Mem Clock");
    ImGui::SameLine(120);
    ImGui::Text("%d MHz", gpu.clock_memory_mhz);

    ImGui::Text("Fan");
    ImGui::SameLine(120);
    ImGui::Text("%d%%", gpu.fan_speed_pct);

    ImGui::Separator();

    // Utilization sparkline
    ImGui::Text("Utilization History");
    int count = std::min(gpu.history_offset, GpuInfo::kHistoryLen);
    if (count > 0) {
        // Build contiguous array from ring buffer
        std::array<float, GpuInfo::kHistoryLen> plot_data{};
        for (int j = 0; j < count; ++j) {
            int src = (gpu.history_offset - count + j) % GpuInfo::kHistoryLen;
            plot_data[j] = gpu.util_history[src];
        }
        ImGui::PlotLines("##util_spark", plot_data.data(), count,
                         0, nullptr, 0.0f, 100.0f, ImVec2(-1, 60));
    }

    // Temperature sparkline
    ImGui::Text("Temperature History");
    if (count > 0) {
        std::array<float, GpuInfo::kHistoryLen> plot_data{};
        for (int j = 0; j < count; ++j) {
            int src = (gpu.history_offset - count + j) % GpuInfo::kHistoryLen;
            plot_data[j] = gpu.temp_history[src];
        }
        ImGui::PlotLines("##temp_spark", plot_data.data(), count,
                         0, nullptr, 0.0f, 110.0f, ImVec2(-1, 60));
    }

    // VRAM usage sparkline
    ImGui::Text("VRAM Usage History");
    if (count > 0) {
        std::array<float, GpuInfo::kHistoryLen> plot_data{};
        for (int j = 0; j < count; ++j) {
            int src = (gpu.history_offset - count + j) % GpuInfo::kHistoryLen;
            plot_data[j] = gpu.vram_history[src];
        }
        ImGui::PlotLines("##vram_spark", plot_data.data(), count,
                         0, nullptr, 0.0f, 100.0f, ImVec2(-1, 60));
    }

    ImGui::End();
}

} // namespace straylight::widgets
