// apps/hub/service_panel.cpp
#include "service_panel.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <dirent.h>
#include <signal.h>

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
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

ServicePanel::ServicePanel() {
    init_service_list();
}

void ServicePanel::init_service_list() {
    // All StrayLight daemons and services
    struct SvcDef {
        const char* name;
        const char* display;
        const char* desc;
        const char* module;
        const char* probe;
        bool kernel;
    };

    static const SvcDef defs[] = {
        {"straylight-core",      "Core",         "Kernel subsystem manager",          "", "", false},
        {"straylight-bus",       "Bus",          "Inter-process message bus",         "", "", false},
        {"straylight-registry",  "Registry",     "Service registry and discovery",    "", "", false},
        {"straylight-scheduler", "Scheduler",    "Process and GPU scheduler",         "", "/proc/straylight/sched/status", false},
        {"straylight-entropy",   "Entropy",      "Random number generator",           "", "/proc/straylight/entropy", false},
        {"straylight-alice",     "Alice",        "AI system health monitor",          "", "", false},
        {"straylight-autotune",  "Autotune",     "Hardware performance tuner",        "", "", false},
        {"straylight-flux",      "Flux",         "Realtime data stream processor",    "", "", false},
        {"straylight-swarm",     "Swarm",        "Multi-GPU mesh manager",            "", "/dev/straylight-vpu", false},
        {"straylight-remote",    "Remote",       "Remote access daemon",              "", "", false},
        {"straylight-hv",        "Hypervisor",   "KVM/VMX kernel extension",          "straylight_hv", "/proc/straylight/hv/stats", true},
        {"straylight-vpu",       "VPU",          "GPU memory slab kernel allocator",  "straylight_vpu", "/sys/kernel/straylight-vpu/slab_usage", true},
        {"straylight-sched",     "Sched .ko",    "ML-aware scheduler kernel surface", "straylight_sched", "/proc/straylight/sched/status", true},
        {"straylight-entropy-ko","Entropy .ko",  "Hardware entropy kernel source",    "straylight_entropy", "/proc/straylight/entropy", true},
        {"straylight-enclave-ko","Enclave .ko",  "Enclave/EPC kernel surface",        "straylight_enclave", "/dev/straylight-enclave", true},
        {"straylight-xdp",       "XDP",          "eBPF datapath attachment",          "", "/usr/lib/straylight/bpf/xdp_stats.bpf.o", true},
    };

    std::lock_guard lock(services_mu_);
    services_.clear();
    for (const auto& def : defs) {
        ServiceInfo svc;
        svc.name = def.name;
        svc.display_name = def.display;
        svc.description = def.desc;
        svc.module_name = def.module;
        svc.probe_path = def.probe;
        svc.kernel_surface = def.kernel;
        svc.status = ServiceInfo::Status::Unknown;
        services_.push_back(svc);
    }
}

void ServicePanel::query_service_status(ServiceInfo& svc) {
    if (svc.kernel_surface) {
        bool online = false;
        if (!svc.module_name.empty()) {
            std::string cmd = "awk '{print $1}' /proc/modules 2>/dev/null | grep -qx " +
                              svc.module_name;
            online = (::system(cmd.c_str()) == 0);
        } else if (svc.name == "straylight-xdp") {
            online = exec_cmd("/usr/bin/straylight-xdp stats --iface lo --skb 2>/dev/null | grep -q 'prog_id: [1-9]' && echo online").find("online") != std::string::npos;
        }
        if (!online && !svc.probe_path.empty()) {
            online = std::filesystem::exists(svc.probe_path);
        }
        svc.status = online ? ServiceInfo::Status::Running : ServiceInfo::Status::Stopped;
        svc.pid = 0;
        svc.uptime = online ? "kernel" : "";
        return;
    }

    // Check if process is running by looking for it in /proc (Linux)
    // or using pgrep
    std::string cmd = "pgrep -x " + svc.name + " 2>/dev/null";
    std::string pid_str = exec_cmd(cmd);

    if (!pid_str.empty()) {
        svc.status = ServiceInfo::Status::Running;
        try {
            svc.pid = std::stoi(pid_str);
        } catch (...) {
            svc.pid = 0;
        }

        // Get uptime from /proc/pid/stat
        std::string stat_path = "/proc/" + pid_str + "/stat";
        std::ifstream stat_f(stat_path);
        if (stat_f.is_open()) {
            // We'll just show time since start
            std::string cmd2 = "ps -p " + pid_str + " -o etime= 2>/dev/null";
            svc.uptime = exec_cmd(cmd2);
        }
    } else {
        svc.status = ServiceInfo::Status::Stopped;
        svc.pid = 0;
        svc.uptime.clear();
    }
}

void ServicePanel::read_service_log(ServiceInfo& svc, int lines) {
    // Try journalctl first
    std::string cmd = "journalctl -u " + svc.name +
                      " --no-pager -n " + std::to_string(lines) + " 2>/dev/null";
    svc.log_tail = exec_cmd(cmd);

    // Fallback: check /var/log/straylight/
    if (svc.log_tail.empty()) {
        std::string log_path = "/var/log/straylight/" + svc.name + ".log";
        std::ifstream f(log_path);
        if (f.is_open()) {
            // Read last N lines
            std::vector<std::string> all_lines;
            std::string line;
            while (std::getline(f, line)) {
                all_lines.push_back(line);
            }

            std::ostringstream ss;
            size_t start = (all_lines.size() > static_cast<size_t>(lines)) ?
                all_lines.size() - static_cast<size_t>(lines) : 0;
            for (size_t i = start; i < all_lines.size(); ++i) {
                ss << all_lines[i] << "\n";
            }
            svc.log_tail = ss.str();
        }
    }

    if (svc.log_tail.empty()) {
        svc.log_tail = "(no logs available)";
    }
}

void ServicePanel::service_action(const std::string& name, const std::string& action) {
    std::string cmd;
    if (action == "start") {
        cmd = "systemctl start " + name + " 2>&1 || " + name + " &";
    } else if (action == "stop") {
        cmd = "systemctl stop " + name + " 2>&1 || pkill -x " + name;
    } else if (action == "restart") {
        cmd = "systemctl restart " + name + " 2>&1 || (pkill -x " + name + "; sleep 1; " + name + " &)";
    }

    if (!cmd.empty()) {
        std::thread([cmd]() {
            [[maybe_unused]] const int rc = std::system(cmd.c_str());
        }).detach();
    }
}

void ServicePanel::refresh() {
    std::lock_guard lock(services_mu_);
    for (auto& svc : services_) {
        query_service_status(svc);
    }
}

ImVec4 ServicePanel::status_color(ServiceInfo::Status s) const {
    switch (s) {
    case ServiceInfo::Status::Running: return ImVec4(0.0f, 0.9f, 0.6f, 1.0f);
    case ServiceInfo::Status::Stopped: return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    case ServiceInfo::Status::Error:   return ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
    case ServiceInfo::Status::Unknown: return ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

const char* ServicePanel::status_text(ServiceInfo::Status s) const {
    switch (s) {
    case ServiceInfo::Status::Running: return "RUNNING";
    case ServiceInfo::Status::Stopped: return "STOPPED";
    case ServiceInfo::Status::Error:   return "ERROR";
    case ServiceInfo::Status::Unknown: return "UNKNOWN";
    }
    return "?";
}

void ServicePanel::render() {
    refresh_timer_ += ImGui::GetIO().DeltaTime;
    if (refresh_timer_ >= refresh_interval_) {
        refresh_timer_ = 0.0f;
        std::thread([this]() { refresh(); }).detach();
    }

    if (ImGui::Button("Refresh All")) {
        std::thread([this]() { refresh(); }).detach();
    }
    ImGui::SameLine();

    // Count running
    std::lock_guard lock(services_mu_);
    int running = 0;
    for (const auto& s : services_) {
        if (s.status == ServiceInfo::Status::Running) ++running;
    }
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "%d/%zu services running", running, services_.size());

    ImGui::Separator();

    for (size_t i = 0; i < services_.size(); ++i) {
        auto& svc = services_[i];
        ImGui::PushID(static_cast<int>(i));

        // Status dot
        ImVec4 color = status_color(svc.status);
        ImGui::TextColored(color, "%s", status_text(svc.status));
        ImGui::SameLine(100.0f);

        // Service name and description
        ImGui::Text("%s", svc.display_name.c_str());
        if (svc.kernel_surface) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "[kernel]");
        }
        ImGui::SameLine(200.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", svc.description.c_str());

        // Action buttons
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 250.0f);

        if (svc.status == ServiceInfo::Status::Running) {
            if (svc.pid > 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "PID:%d", svc.pid);
                ImGui::SameLine();
            }
            if (!svc.uptime.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", svc.uptime.c_str());
                ImGui::SameLine();
            }
        }

        ImGui::SameLine(ImGui::GetContentRegionMax().x - 160.0f);

        if (!svc.kernel_surface) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.4f, 0.2f, 0.8f));
            if (ImGui::SmallButton("Start")) {
                service_action(svc.name, "start");
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 0.8f));
            if (ImGui::SmallButton("Stop")) {
                service_action(svc.name, "stop");
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.1f, 0.8f));
            if (ImGui::SmallButton("Restart")) {
                service_action(svc.name, "restart");
            }
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s",
                               svc.probe_path.empty() ? "module" : svc.probe_path.c_str());
        }

        ImGui::SameLine();
        if (ImGui::SmallButton(svc.log_expanded ? "Hide Log" : "Log")) {
            svc.log_expanded = !svc.log_expanded;
            if (svc.log_expanded) {
                read_service_log(svc, 20);
            }
        }

        // Log viewer
        if (svc.log_expanded && !svc.log_tail.empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.12f, 1.0f));
            ImGui::BeginChild(("log_" + svc.name).c_str(), ImVec2(0, 150), true);
            ImGui::TextUnformatted(svc.log_tail.c_str());
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (ImGui::SmallButton("Refresh Log")) {
                read_service_log(svc, 20);
            }
        }

        ImGui::Separator();
        ImGui::PopID();
    }
}

} // namespace straylight::hub
