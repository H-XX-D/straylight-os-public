// apps/widgets/system/cpu_topology.cpp
#include "cpu_topology.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::CpuTopologyWidget, "cpu_topology", "CPU Topology", straylight::widgets::WidgetCategory::System);
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace straylight::widgets {

std::vector<CpuCore> CpuTopologyWidget::parse_proc_cpuinfo(const std::string& content) {
    std::vector<CpuCore> cores;
    std::istringstream ss(content);
    std::string line;
    CpuCore current;
    bool in_block = false;

    while (std::getline(ss, line)) {
        if (line.empty()) {
            if (in_block) {
                cores.push_back(current);
                current = CpuCore{};
                in_block = false;
            }
            continue;
        }

        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string val = (colon + 1 < line.size()) ? line.substr(colon + 1) : "";
        // Trim
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());

        in_block = true;

        if (key == "processor") {
            current.cpu_id = std::stoi(val);
        } else if (key == "core id") {
            current.core_id = std::stoi(val);
        } else if (key == "physical id") {
            current.package_id = std::stoi(val);
        } else if (key == "cpu MHz") {
            current.freq_mhz = std::stof(val);
        }
    }
    if (in_block) {
        cores.push_back(current);
    }
    return cores;
}

void CpuTopologyWidget::discover_topology() {
    namespace fs = std::filesystem;

    // Read /proc/cpuinfo for basic topology
    {
        std::ifstream f("/proc/cpuinfo");
        if (f) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            cores_ = parse_proc_cpuinfo(content);

            // Extract model name from first block
            std::istringstream ss(content);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.find("model name") == 0) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        model_name_ = line.substr(colon + 2);
                    }
                    break;
                }
            }
        }
    }

    // Detect P/E cores via sysfs intel_pstate or cpufreq
    for (auto& core : cores_) {
        std::string cpu_path = "/sys/devices/system/cpu/cpu" + std::to_string(core.cpu_id);

        // Max frequency
        {
            std::ifstream f(cpu_path + "/cpufreq/cpuinfo_max_freq");
            int khz = 0;
            if (f >> khz) core.max_freq_mhz = static_cast<float>(khz) / 1000.0f;
        }
        {
            std::ifstream f(cpu_path + "/cpufreq/cpuinfo_min_freq");
            int khz = 0;
            if (f >> khz) core.min_freq_mhz = static_cast<float>(khz) / 1000.0f;
        }

        // NUMA node
        for (int n = 0; n < 16; ++n) {
            std::string node_path = "/sys/devices/system/node/node" + std::to_string(n);
            std::string cpu_file = node_path + "/cpu" + std::to_string(core.cpu_id);
            if (fs::exists(node_path + "/cpulist")) {
                std::ifstream f(node_path + "/cpulist");
                std::string cpulist;
                if (f >> cpulist) {
                    // Simple check: see if this CPU's topology/node maps here
                }
            }
            // Use topology/physical_package_id heuristic for NUMA mapping
            std::ifstream nf(cpu_path + "/topology/physical_package_id");
            int pkg = 0;
            if (nf >> pkg) core.numa_node = pkg;
        }

        // Intel hybrid core type detection
        std::string core_type_path = cpu_path + "/cpu_capacity";
        {
            std::ifstream f(core_type_path);
            int capacity = 0;
            if (f >> capacity) {
                core.core_type = (capacity >= 1024) ? "performance" : "efficiency";
            }
        }
    }

    // Count sockets
    int max_pkg = 0;
    for (auto& c : cores_) max_pkg = std::max(max_pkg, c.package_id);
    num_sockets_ = max_pkg + 1;

    topology_discovered_ = true;
}

void CpuTopologyWidget::read_frequencies() {
    for (auto& core : cores_) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(core.cpu_id) +
                           "/cpufreq/scaling_cur_freq";
        std::ifstream f(path);
        int khz = 0;
        if (f >> khz) core.freq_mhz = static_cast<float>(khz) / 1000.0f;
    }
}

void CpuTopologyWidget::read_temperatures() {
    // Try coretemp/hwmon
    namespace fs = std::filesystem;
    std::string hwmon_base = "/sys/class/hwmon";
    if (!fs::exists(hwmon_base)) return;

    for (auto& entry : fs::directory_iterator(hwmon_base)) {
        std::ifstream name_f(entry.path().string() + "/name");
        std::string driver;
        if (name_f) std::getline(name_f, driver);

        if (driver != "coretemp" && driver != "k10temp" && driver != "zenpower") continue;

        for (int i = 1; i <= 256; ++i) {
            std::string temp_path = entry.path().string() + "/temp" + std::to_string(i) + "_input";
            std::string label_path = entry.path().string() + "/temp" + std::to_string(i) + "_label";
            if (!fs::exists(temp_path)) break;

            std::ifstream tf(temp_path);
            int millideg = 0;
            if (!(tf >> millideg)) continue;
            float temp_c = static_cast<float>(millideg) / 1000.0f;

            // Try to map to a core
            std::ifstream lf(label_path);
            std::string label;
            if (lf) std::getline(lf, label);

            // "Core N" pattern
            int core_num = -1;
            if (std::sscanf(label.c_str(), "Core %d", &core_num) == 1) {
                for (auto& c : cores_) {
                    if (c.core_id == core_num) {
                        c.temperature_c = temp_c;
                    }
                }
            }
        }
    }
}

void CpuTopologyWidget::read_utilization() {
    std::ifstream f("/proc/stat");
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, 3) != "cpu" || line[3] == ' ') continue;

        int cpu_id = -1;
        uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        unsigned long long scanned_user = 0, scanned_nice = 0, scanned_sys = 0, scanned_idle = 0;
        unsigned long long scanned_iowait = 0, scanned_irq = 0, scanned_softirq = 0, scanned_steal = 0;
        if (std::sscanf(line.c_str(), "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                &cpu_id, &scanned_user, &scanned_nice, &scanned_sys, &scanned_idle, &scanned_iowait, &scanned_irq, &scanned_softirq, &scanned_steal) < 5)
            continue;
        user = scanned_user; nice = scanned_nice; sys = scanned_sys; idle = scanned_idle;
        iowait = scanned_iowait; irq = scanned_irq; softirq = scanned_softirq; steal = scanned_steal;

        uint64_t total = user + nice + sys + idle + iowait + irq + softirq + steal;
        uint64_t idle_total = idle + iowait;

        for (auto& c : cores_) {
            if (c.cpu_id == cpu_id) {
                if (c.prev_total > 0) {
                    uint64_t dt = total - c.prev_total;
                    uint64_t di = idle_total - c.prev_idle;
                    c.utilization_pct = (dt > 0) ? 100.0f * (1.0f - static_cast<float>(di) / static_cast<float>(dt)) : 0.0f;
                }
                c.prev_total = total;
                c.prev_idle = idle_total;
                break;
            }
        }
    }
}

void CpuTopologyWidget::read_numa() {
    namespace fs = std::filesystem;
    numa_nodes_.clear();

    for (int n = 0; n < 16; ++n) {
        std::string path = "/sys/devices/system/node/node" + std::to_string(n);
        if (!fs::exists(path)) break;

        NumaNode nn;
        nn.id = n;

        std::ifstream f(path + "/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            unsigned long long kb = 0;
            if (line.find("MemTotal") != std::string::npos) {
                std::sscanf(line.c_str(), "%*s %*d MemTotal: %llu kB", &kb);
                nn.mem_total_mb = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
            } else if (line.find("MemFree") != std::string::npos) {
                std::sscanf(line.c_str(), "%*s %*d MemFree: %llu kB", &kb);
                nn.mem_free_mb = static_cast<float>(static_cast<uint64_t>(kb)) / 1024.0f;
            }
        }

        for (auto& c : cores_) {
            if (c.numa_node == n) nn.cpu_ids.push_back(c.cpu_id);
        }

        numa_nodes_.push_back(std::move(nn));
    }
}

void CpuTopologyWidget::update() {
    if (!should_update()) return;
    if (!topology_discovered_) discover_topology();
    read_frequencies();
    read_temperatures();
    read_utilization();
    read_numa();
}

void CpuTopologyWidget::render(bool* p_open) {
    if (!ImGui::Begin("CPU Topology", p_open)) {
        ImGui::End();
        return;
    }

    if (cores_.empty()) {
        ImGui::TextWrapped("No CPU information available. Requires /proc/cpuinfo and /sys.");
        ImGui::End();
        return;
    }

    // Header info
    ImGui::Text("Model: %s", model_name_.c_str());
    ImGui::Text("Cores: %zu | Sockets: %d | NUMA Nodes: %zu",
                cores_.size(), num_sockets_, numa_nodes_.size());

    // Count P/E cores
    int p_cores = 0, e_cores = 0;
    for (auto& c : cores_) {
        if (c.core_type == "performance") p_cores++;
        else if (c.core_type == "efficiency") e_cores++;
    }
    if (p_cores > 0 || e_cores > 0) {
        ImGui::Text("Hybrid: %d P-cores, %d E-cores", p_cores, e_cores);
    }

    ImGui::RadioButton("Grid", &view_mode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Table", &view_mode_, 1);

    ImGui::Separator();

    if (view_mode_ == 0) {
        // Grid view — colored cells by utilization
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float cell = 24.0f;
        int cols = 16;
        if (static_cast<int>(cores_.size()) <= 16) cols = 8;

        for (int i = 0; i < static_cast<int>(cores_.size()); ++i) {
            auto& c = cores_[i];
            int col = i % cols;
            int row = i / cols;
            ImVec2 tl(origin.x + col * (cell + 2), origin.y + row * (cell + 2));
            ImVec2 br(tl.x + cell, tl.y + cell);

            float t = c.utilization_pct / 100.0f;
            ImVec4 color;
            if (c.core_type == "efficiency") {
                color = ImVec4(0.1f + t * 0.5f, 0.6f - t * 0.3f, 1.0f - t * 0.5f, 0.9f);
            } else {
                color = ImVec4(t, 1.0f - t * 0.7f, 0.1f, 0.9f);
            }
            dl->AddRectFilled(tl, br, ImGui::GetColorU32(color), 3.0f);

            // Tooltip
            ImGui::SetCursorScreenPos(tl);
            char btn_id[32]; std::snprintf(btn_id, sizeof(btn_id), "##cpu%d", i);
            ImGui::InvisibleButton(btn_id, ImVec2(cell, cell));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("CPU %d (Core %d, Pkg %d)", c.cpu_id, c.core_id, c.package_id);
                if (!c.core_type.empty()) ImGui::Text("Type: %s", c.core_type.c_str());
                ImGui::Text("Freq: %.0f MHz (%.0f-%.0f)", c.freq_mhz, c.min_freq_mhz, c.max_freq_mhz);
                ImGui::Text("Util: %.1f%%", c.utilization_pct);
                if (c.temperature_c > 0) ImGui::Text("Temp: %.0f C", c.temperature_c);
                ImGui::Text("NUMA: %d", c.numa_node);
                ImGui::EndTooltip();
            }
        }

        int total_rows = (static_cast<int>(cores_.size()) + cols - 1) / cols;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + total_rows * (cell + 2) + 4));
        ImGui::Dummy(ImVec2(0, 0));
    } else {
        // Table view
        if (ImGui::BeginTable("##cpu_table", 8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, ImGui::GetContentRegionAvail().y - 80))) {

            ImGui::TableSetupColumn("CPU");
            ImGui::TableSetupColumn("Core");
            ImGui::TableSetupColumn("Pkg");
            ImGui::TableSetupColumn("NUMA");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Freq (MHz)");
            ImGui::TableSetupColumn("Util");
            ImGui::TableSetupColumn("Temp (C)");
            ImGui::TableHeadersRow();

            for (auto& c : cores_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", c.cpu_id);
                ImGui::TableNextColumn(); ImGui::Text("%d", c.core_id);
                ImGui::TableNextColumn(); ImGui::Text("%d", c.package_id);
                ImGui::TableNextColumn(); ImGui::Text("%d", c.numa_node);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.core_type.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.0f", c.freq_mhz);
                ImGui::TableNextColumn();
                {
                    float t = c.utilization_pct / 100.0f;
                    ImVec4 col(t, 1.0f - t * 0.7f, 0.1f, 1.0f);
                    ImGui::TextColored(col, "%.1f%%", c.utilization_pct);
                }
                ImGui::TableNextColumn();
                if (c.temperature_c > 0) {
                    ImVec4 col = (c.temperature_c > 85) ? ImVec4(1, 0.2f, 0.2f, 1) :
                                 (c.temperature_c > 70) ? ImVec4(1, 0.8f, 0, 1) :
                                                           ImVec4(0.4f, 1, 0.4f, 1);
                    ImGui::TextColored(col, "%.0f", c.temperature_c);
                } else {
                    ImGui::Text("-");
                }
            }
            ImGui::EndTable();
        }
    }

    // NUMA summary
    if (!numa_nodes_.empty()) {
        ImGui::Separator();
        ImGui::Text("NUMA Nodes:");
        for (auto& nn : numa_nodes_) {
            float used = nn.mem_total_mb - nn.mem_free_mb;
            float frac = (nn.mem_total_mb > 0) ? used / nn.mem_total_mb : 0.0f;
            char ov[64]; std::snprintf(ov, sizeof(ov), "Node %d: %.0f/%.0f MiB (%zu CPUs)",
                                       nn.id, used, nn.mem_total_mb, nn.cpu_ids.size());
            ImGui::ProgressBar(frac, ImVec2(-1, 0), ov);
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
