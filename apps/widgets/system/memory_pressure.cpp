// apps/widgets/system/memory_pressure.cpp
#include "memory_pressure.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::MemoryPressureWidget, "memory_pressure", "Memory Pressure", straylight::widgets::WidgetCategory::System);
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace straylight::widgets {

PsiMetrics MemoryPressureWidget::parse_psi_file(const std::string& path) {
    PsiMetrics m;
    std::ifstream f(path);
    if (!f) return m;

    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, 4) == "some") {
            unsigned long long total = 0;
            std::sscanf(line.c_str(), "some avg10=%f avg60=%f avg300=%f total=%llu",
                        &m.some_avg10, &m.some_avg60, &m.some_avg300, &total);
            m.some_total = static_cast<uint64_t>(total);
        } else if (line.substr(0, 4) == "full") {
            unsigned long long total = 0;
            std::sscanf(line.c_str(), "full avg10=%f avg60=%f avg300=%f total=%llu",
                        &m.full_avg10, &m.full_avg60, &m.full_avg300, &total);
            m.full_total = static_cast<uint64_t>(total);
        }
    }
    return m;
}

ImVec4 MemoryPressureWidget::pressure_color(float avg10) {
    if (avg10 > 50.0f) return ImVec4(1, 0.1f, 0.1f, 1);
    if (avg10 > 20.0f) return ImVec4(1, 0.5f, 0, 1);
    if (avg10 > 5.0f)  return ImVec4(1, 0.8f, 0, 1);
    return ImVec4(0.4f, 1, 0.4f, 1);
}

void MemoryPressureWidget::read_psi() {
    mem_psi_ = parse_psi_file("/proc/pressure/memory");
    cpu_psi_ = parse_psi_file("/proc/pressure/cpu");
    io_psi_ = parse_psi_file("/proc/pressure/io");
    psi_available_ = (mem_psi_.some_total > 0 || cpu_psi_.some_total > 0);
}

void MemoryPressureWidget::read_meminfo() {
    std::ifstream f("/proc/meminfo");
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        unsigned long long kb = 0;
        if (line.find("MemTotal:") == 0) {
            std::sscanf(line.c_str(), "MemTotal: %llu kB", &kb);
            mem_total_mb_ = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
        } else if (line.find("MemAvailable:") == 0) {
            std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb);
            mem_available_mb_ = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
        } else if (line.find("SwapTotal:") == 0) {
            std::sscanf(line.c_str(), "SwapTotal: %llu kB", &kb);
            swap_total_mb_ = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
        } else if (line.find("SwapFree:") == 0) {
            std::sscanf(line.c_str(), "SwapFree: %llu kB", &kb);
            // swap_used = total - free
            // We'll compute after loop
        } else if (line.find("Cached:") == 0) {
            std::sscanf(line.c_str(), "Cached: %llu kB", &kb);
            cached_mb_ = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
        } else if (line.find("Buffers:") == 0) {
            std::sscanf(line.c_str(), "Buffers: %llu kB", &kb);
            buffers_mb_ = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
        } else if (line.find("Slab:") == 0) {
            std::sscanf(line.c_str(), "Slab: %llu kB", &kb);
            slab_mb_ = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
        }
    }

    mem_used_mb_ = mem_total_mb_ - mem_available_mb_;

    // Re-read swap free for swap_used calculation
    {
        std::ifstream sf("/proc/meminfo");
        std::string ln;
        while (std::getline(sf, ln)) {
            unsigned long long kb = 0;
            if (ln.find("SwapFree:") == 0) {
                std::sscanf(ln.c_str(), "SwapFree: %llu kB", &kb);
                swap_used_mb_ = swap_total_mb_ - static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
                break;
            }
        }
    }
}

void MemoryPressureWidget::update() {
    if (!should_update()) return;
    read_psi();
    read_meminfo();

    // Push history
    int idx = hist_offset_ % kHistLen;
    mem_psi_hist_[idx] = mem_psi_.some_avg10;
    mem_used_hist_[idx] = (mem_total_mb_ > 0) ? (mem_used_mb_ / mem_total_mb_) * 100.0f : 0.0f;
    hist_offset_++;
}

void MemoryPressureWidget::render(bool* p_open) {
    if (!ImGui::Begin("Memory Pressure", p_open)) {
        ImGui::End();
        return;
    }

    // Memory usage bars
    ImGui::Text("System Memory");
    {
        float frac = (mem_total_mb_ > 0) ? mem_used_mb_ / mem_total_mb_ : 0.0f;
        char ov[64]; std::snprintf(ov, sizeof(ov), "%.0f / %.0f MiB (%.0f%%)",
                                   mem_used_mb_, mem_total_mb_, frac * 100.0f);
        ImVec4 col = (frac > 0.9f) ? ImVec4(1, 0.2f, 0.2f, 1) :
                     (frac > 0.7f) ? ImVec4(1, 0.8f, 0, 1) : ImVec4(0.3f, 0.8f, 0.3f, 1);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
        ImGui::PopStyleColor();
    }

    // Breakdown
    ImGui::Text("  Cached: %.0f MiB | Buffers: %.0f MiB | Slab: %.0f MiB",
                cached_mb_, buffers_mb_, slab_mb_);

    if (swap_total_mb_ > 0) {
        float frac = swap_used_mb_ / swap_total_mb_;
        char ov[64]; std::snprintf(ov, sizeof(ov), "Swap: %.0f / %.0f MiB",
                                   swap_used_mb_, swap_total_mb_);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
    }

    ImGui::Separator();

    // PSI Section
    if (!psi_available_) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "PSI not available (requires kernel 4.20+)");
    } else {
        ImGui::Text("Pressure Stall Information (PSI)");

        auto render_psi_row = [](const char* label, const PsiMetrics& m) {
            ImGui::Text("%s", label);
            ImGui::SameLine(100);
            ImVec4 c10 = pressure_color(m.some_avg10);
            ImVec4 c60 = pressure_color(m.some_avg60);
            ImVec4 c300 = pressure_color(m.some_avg300);

            ImGui::TextColored(c10, "10s:%.2f%%", m.some_avg10);
            ImGui::SameLine();
            ImGui::TextColored(c60, "60s:%.2f%%", m.some_avg60);
            ImGui::SameLine();
            ImGui::TextColored(c300, "300s:%.2f%%", m.some_avg300);

            if (m.full_total > 0) {
                ImGui::Text("  full");
                ImGui::SameLine(100);
                ImGui::Text("10s:%.2f%% 60s:%.2f%% 300s:%.2f%%",
                            m.full_avg10, m.full_avg60, m.full_avg300);
            }
        };

        render_psi_row("Memory", mem_psi_);
        render_psi_row("CPU", cpu_psi_);
        render_psi_row("I/O", io_psi_);
    }

    // History plots
    ImGui::Separator();
    int count = std::min(hist_offset_, kHistLen);
    if (count > 0) {
        std::array<float, kHistLen> psi_plot{}, used_plot{};
        for (int j = 0; j < count; ++j) {
            int src = (hist_offset_ - count + j) % kHistLen;
            psi_plot[j] = mem_psi_hist_[src];
            used_plot[j] = mem_used_hist_[src];
        }

        ImGui::Text("Memory PSI (some avg10)");
        ImGui::PlotLines("##mem_psi_hist", psi_plot.data(), count,
                         0, nullptr, 0.0f, 100.0f, ImVec2(-1, 50));

        ImGui::Text("Memory Usage %%");
        ImGui::PlotLines("##mem_used_hist", used_plot.data(), count,
                         0, nullptr, 0.0f, 100.0f, ImVec2(-1, 50));
    }

    ImGui::End();
}

} // namespace straylight::widgets
