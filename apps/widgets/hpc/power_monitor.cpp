// apps/widgets/hpc/power_monitor.cpp
#include "power_monitor.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::PowerMonitorWidget, "power_monitor", "Power Monitor", straylight::widgets::WidgetCategory::HPC);
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>

namespace straylight::widgets {

void PowerMonitorWidget::discover_rapl() {
    namespace fs = std::filesystem;
    const std::string rapl_base = "/sys/class/powercap/intel-rapl";
    if (!fs::exists(rapl_base)) {
        rapl_available_ = false;
        return;
    }

    rapl_available_ = true;
    rapl_domains_.clear();

    // Enumerate top-level and sub-domains
    for (auto& entry : fs::directory_iterator(rapl_base)) {
        std::string ename = entry.path().filename().string();
        if (ename.find("intel-rapl:") == std::string::npos) continue;

        auto add_domain = [&](const fs::path& p) {
            std::string name_path = p.string() + "/name";
            std::string energy_path = p.string() + "/energy_uj";
            if (!fs::exists(energy_path)) return;

            RaplDomain d;
            d.path = p.string();
            std::ifstream nf(name_path);
            if (nf) std::getline(nf, d.name);
            else d.name = p.filename().string();

            std::ifstream mf(p.string() + "/max_energy_range_uj");
            if (mf) mf >> d.max_energy_range_uj;

            std::ifstream cf(p.string() + "/constraint_0_max_power_uw");
            if (cf) {
                uint64_t max_uw = 0;
                cf >> max_uw;
                d.max_power_w = static_cast<float>(max_uw) / 1e6f;
            }

            rapl_domains_.push_back(std::move(d));
        };

        add_domain(entry.path());

        // Sub-domains
        for (auto& sub : fs::directory_iterator(entry.path())) {
            std::string sname = sub.path().filename().string();
            if (sname.find("intel-rapl:") != std::string::npos) {
                add_domain(sub.path());
            }
        }
    }
}

void PowerMonitorWidget::read_rapl() {
    if (!rapl_available_) return;

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last_energy_sample_).count();

    total_cpu_power_ = 0.0f;

    for (auto& d : rapl_domains_) {
        std::ifstream f(d.path + "/energy_uj");
        uint64_t energy = 0;
        if (f) f >> energy;

        if (d.prev_energy_uj > 0 && dt > 0.05f) {
            uint64_t delta;
            if (energy >= d.prev_energy_uj) {
                delta = energy - d.prev_energy_uj;
            } else {
                // Counter wrapped
                delta = (d.max_energy_range_uj - d.prev_energy_uj) + energy;
            }
            d.power_w = static_cast<float>(delta) / (dt * 1e6f);
        }
        d.energy_uj = energy;
        d.prev_energy_uj = energy;

        // Sum package-level domains for total
        if (d.name.find("package") != std::string::npos) {
            total_cpu_power_ += d.power_w;
        }
    }

    last_energy_sample_ = now;
}

void PowerMonitorWidget::read_gpu_power() {
    gpu_powers_.clear();
    total_gpu_power_ = 0.0f;

    FILE* pipe = ::popen(
        "nvidia-smi --query-gpu=index,name,power.draw,power.limit "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!pipe) return;

    char buf[512];
    while (::fgets(buf, sizeof(buf), pipe)) {
        GpuPower gp;
        char name_buf[256]{};
        if (std::sscanf(buf, " %d , %255[^,] , %f , %f",
                &gp.gpu_id, name_buf, &gp.power_w, &gp.limit_w) >= 3) {
            gp.name = name_buf;
            while (!gp.name.empty() && gp.name.front() == ' ') gp.name.erase(gp.name.begin());
            while (!gp.name.empty() && gp.name.back() == ' ') gp.name.pop_back();
            total_gpu_power_ += gp.power_w;
            gpu_powers_.push_back(std::move(gp));
        }
    }
    ::pclose(pipe);
}

void PowerMonitorWidget::push_history(RaplDomain& d) {
    int idx = d.hist_offset % RaplDomain::kHistLen;
    d.power_hist[idx] = d.power_w;
    d.hist_offset++;
}

void PowerMonitorWidget::update() {
    if (!should_update()) return;

    if (rapl_domains_.empty()) discover_rapl();
    read_rapl();
    read_gpu_power();
    for (auto& d : rapl_domains_) push_history(d);
}

void PowerMonitorWidget::render(bool* p_open) {
    if (!ImGui::Begin("Power Monitor", p_open)) {
        ImGui::End();
        return;
    }

    // Total power summary
    float total = total_cpu_power_ + total_gpu_power_;
    ImGui::Text("Total System Power: %.1f W", total);
    ImGui::Text("  CPU/Package: %.1f W | GPU: %.1f W", total_cpu_power_, total_gpu_power_);

    ImGui::Separator();

    // RAPL Domains
    if (rapl_available_ && !rapl_domains_.empty()) {
        ImGui::Text("RAPL Power Domains:");

        if (ImGui::BeginTable("##rapl_table", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                ImVec2(0, 150))) {

            ImGui::TableSetupColumn("Domain");
            ImGui::TableSetupColumn("Power (W)");
            ImGui::TableSetupColumn("Limit (W)");
            ImGui::TableSetupColumn("Usage");
            ImGui::TableHeadersRow();

            for (auto& d : rapl_domains_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", d.power_w);
                ImGui::TableNextColumn();
                if (d.max_power_w > 0) ImGui::Text("%.1f", d.max_power_w);
                else ImGui::Text("-");
                ImGui::TableNextColumn();
                if (d.max_power_w > 0) {
                    float frac = d.power_w / d.max_power_w;
                    char ov[16]; std::snprintf(ov, sizeof(ov), "%.0f%%", frac * 100.0f);
                    ImVec4 col = (frac > 0.9f) ? ImVec4(1, 0.2f, 0.2f, 1) :
                                 (frac > 0.7f) ? ImVec4(1, 0.8f, 0, 1) : ImVec4(0.3f, 0.8f, 0.3f, 1);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                    ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Text("-");
                }
            }
            ImGui::EndTable();
        }

        // Power history per domain
        for (auto& d : rapl_domains_) {
            int count = std::min(d.hist_offset, RaplDomain::kHistLen);
            if (count > 1) {
                std::array<float, RaplDomain::kHistLen> plot{};
                float max_p = 1.0f;
                for (int j = 0; j < count; ++j) {
                    int src = (d.hist_offset - count + j) % RaplDomain::kHistLen;
                    plot[j] = d.power_hist[src];
                    max_p = std::max(max_p, plot[j]);
                }
                char label[128];
                std::snprintf(label, sizeof(label), "%s (%.1f W)###%s",
                              d.name.c_str(), d.power_w, d.name.c_str());
                ImGui::PlotLines(label, plot.data(), count, 0, nullptr, 0.0f, max_p * 1.2f, ImVec2(-1, 40));
            }
        }
    } else {
        ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "RAPL not available (requires Intel CPU + root)");
    }

    // GPU Power
    if (!gpu_powers_.empty()) {
        ImGui::Separator();
        ImGui::Text("GPU Power:");
        for (auto& gp : gpu_powers_) {
            ImGui::Text("  GPU %d (%s)", gp.gpu_id, gp.name.c_str());
            ImGui::SameLine(300);
            float frac = (gp.limit_w > 0) ? gp.power_w / gp.limit_w : 0.0f;
            char ov[32]; std::snprintf(ov, sizeof(ov), "%.1f / %.1f W", gp.power_w, gp.limit_w);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
