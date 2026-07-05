// apps/system_monitor/process.cpp
// Process list monitoring via /proc filesystem
#include "process.h"

#include "memory.h" // for format_kb

#include <imgui.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>

namespace straylight::sysmon {

ProcessMonitor::ProcessMonitor() {
    page_size_ = sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0) page_size_ = 4096;

    clock_ticks_ = sysconf(_SC_CLK_TCK);
    if (clock_ticks_ <= 0) clock_ticks_ = 100;
}

Result<void, std::string> ProcessMonitor::sample() {
    // Read total CPU time from /proc/stat
    {
        std::ifstream stat_file("/proc/stat");
        if (stat_file.is_open()) {
            std::string line;
            std::getline(stat_file, line);
            uint64_t user = 0, nice = 0, system = 0, idle = 0;
            uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
            sscanf(line.c_str(),
                   "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle,
                   &iowait, &irq, &softirq, &steal);
            prev_total_cpu_time_ = total_cpu_time_;
            total_cpu_time_ = user + nice + system + idle + iowait + irq +
                              softirq + steal;
        }
    }

    // Read total memory
    {
        std::ifstream mem_file("/proc/meminfo");
        if (mem_file.is_open()) {
            std::string line;
            while (std::getline(mem_file, line)) {
                if (line.compare(0, 8, "MemTotal") == 0) {
                    sscanf(line.c_str(), "MemTotal: %lu kB", &total_memory_kb_);
                    break;
                }
            }
        }
    }

    // Save previous process list for CPU delta
    prev_processes_ = processes_;

    // Scan /proc for PIDs
    processes_.clear();

    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        return Result<void, std::string>::error("Cannot open /proc");
    }

    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != nullptr) {
        // Only numeric directory names are PIDs
        if (entry->d_type != DT_DIR) continue;

        pid_t pid = 0;
        bool is_pid = true;
        for (const char* p = entry->d_name; *p; ++p) {
            if (*p < '0' || *p > '9') {
                is_pid = false;
                break;
            }
            pid = pid * 10 + (*p - '0');
        }
        if (!is_pid || pid <= 0) continue;

        ProcessInfo info;
        info.pid = pid;

        read_proc_stat(pid, info);
        read_proc_status(pid, info);

        processes_.push_back(std::move(info));
    }
    closedir(proc_dir);

    // Calculate CPU percentages
    if (!first_sample_) {
        calculate_cpu_percent();
    }
    first_sample_ = false;

    apply_sort_and_filter();

    return Result<void, std::string>::ok();
}

void ProcessMonitor::read_proc_stat(pid_t pid, ProcessInfo& info) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    std::getline(file, line);

    // /proc/[pid]/stat format is tricky because comm can contain spaces/parens
    // Format: pid (comm) state ppid ...
    auto open_paren = line.find('(');
    auto close_paren = line.rfind(')');

    if (open_paren == std::string::npos || close_paren == std::string::npos) {
        return;
    }

    info.comm = line.substr(open_paren + 1, close_paren - open_paren - 1);

    // Parse fields after the closing paren
    const char* rest = line.c_str() + close_paren + 2; // skip ") "

    char state = '?';
    int ppid = 0;
    int pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
    unsigned int flags = 0;
    unsigned long minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
    unsigned long utime = 0, stime = 0;
    long cutime = 0, cstime = 0;
    long priority = 0, nice = 0, num_threads = 0, itrealvalue = 0;
    unsigned long long starttime = 0;
    unsigned long vsize = 0;
    long rss = 0;

    sscanf(rest,
           "%c %d %d %d %d %d %u %lu %lu %lu %lu "
           "%lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %ld",
           &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
           &minflt, &cminflt, &majflt, &cmajflt,
           &utime, &stime, &cutime, &cstime,
           &priority, &nice, &num_threads, &itrealvalue,
           &starttime, &vsize, &rss);

    info.state = state;
    info.ppid = ppid;
    info.utime = utime;
    info.stime = stime;
    info.starttime = starttime;
    info.priority = static_cast<int>(priority);
    info.nice = static_cast<int>(nice);
    info.threads = static_cast<int>(num_threads);
    info.vsize_kb = vsize / 1024;
    info.rss_kb = static_cast<uint64_t>(rss) *
                  static_cast<uint64_t>(page_size_) / 1024;

    // Memory percentage
    if (total_memory_kb_ > 0) {
        info.mem_percent = static_cast<float>(info.rss_kb) /
                           static_cast<float>(total_memory_kb_) * 100.0f;
    }
}

void ProcessMonitor::read_proc_status(pid_t pid, ProcessInfo& info) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
            uid_t uid = 0;
            sscanf(line.c_str(), "Uid: %u", &uid);
            struct passwd* pw = getpwuid(uid);
            if (pw) {
                info.username = pw->pw_name;
            } else {
                info.username = std::to_string(uid);
            }
            break;
        }
    }
}

void ProcessMonitor::calculate_cpu_percent() {
    uint64_t cpu_delta = total_cpu_time_ - prev_total_cpu_time_;
    if (cpu_delta == 0) return;

    for (auto& proc : processes_) {
        // Find previous entry
        for (const auto& prev : prev_processes_) {
            if (prev.pid == proc.pid) {
                uint64_t proc_delta = (proc.utime + proc.stime) -
                                      (prev.utime + prev.stime);
                proc.cpu_percent =
                    static_cast<float>(proc_delta) /
                    static_cast<float>(cpu_delta) * 100.0f;
                break;
            }
        }
    }
}

void ProcessMonitor::set_sort(ProcessSort sort, bool ascending) {
    sort_by_ = sort;
    sort_ascending_ = ascending;
    apply_sort_and_filter();
}

void ProcessMonitor::set_filter(const std::string& filter) {
    filter_ = filter;
    apply_sort_and_filter();
}

void ProcessMonitor::apply_sort_and_filter() {
    filtered_processes_.clear();

    for (const auto& proc : processes_) {
        if (!filter_.empty()) {
            std::string lower_name = proc.comm;
            std::string lower_filter = filter_;
            std::transform(lower_name.begin(), lower_name.end(),
                          lower_name.begin(), ::tolower);
            std::transform(lower_filter.begin(), lower_filter.end(),
                          lower_filter.begin(), ::tolower);
            if (lower_name.find(lower_filter) == std::string::npos) {
                continue;
            }
        }
        filtered_processes_.push_back(proc);
    }

    auto comparator = [this](const ProcessInfo& a, const ProcessInfo& b) {
        bool less = false;
        switch (sort_by_) {
        case ProcessSort::Pid:
            less = a.pid < b.pid;
            break;
        case ProcessSort::Name: {
            std::string an = a.comm, bn = b.comm;
            std::transform(an.begin(), an.end(), an.begin(), ::tolower);
            std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
            less = an < bn;
            break;
        }
        case ProcessSort::Cpu:
            less = a.cpu_percent < b.cpu_percent;
            break;
        case ProcessSort::Memory:
            less = a.mem_percent < b.mem_percent;
            break;
        case ProcessSort::State:
            less = a.state < b.state;
            break;
        }
        return sort_ascending_ ? less : !less;
    };

    std::sort(filtered_processes_.begin(), filtered_processes_.end(), comparator);
}

Result<void, std::string> ProcessMonitor::kill_process(pid_t pid, int signal) {
    if (::kill(pid, signal) != 0) {
        return Result<void, std::string>::error(
            "Failed to send signal: " + std::string(strerror(errno)));
    }
    return Result<void, std::string>::ok();
}

void ProcessMonitor::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Processes");
    ImGui::Text("Total: %d", static_cast<int>(processes_.size()));

    // Filter
    static char filter_buf[128] = {};
    ImGui::SameLine(ImGui::GetWindowWidth() - 350);
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("Filter##proc", filter_buf, sizeof(filter_buf))) {
        set_filter(filter_buf);
    }

    // Kill button
    ImGui::SameLine();
    if (selected_pid_ > 0) {
        if (ImGui::Button("Kill")) {
            ImGui::OpenPopup("KillProcess");
        }
    }

    // Kill popup
    if (ImGui::BeginPopup("KillProcess")) {
        ImGui::Text("Send signal to PID %d:", selected_pid_);
        if (ImGui::MenuItem("SIGTERM (15)")) {
            kill_process(selected_pid_, SIGTERM);
        }
        if (ImGui::MenuItem("SIGKILL (9)")) {
            kill_process(selected_pid_, SIGKILL);
        }
        if (ImGui::MenuItem("SIGSTOP (19)")) {
            kill_process(selected_pid_, SIGSTOP);
        }
        if (ImGui::MenuItem("SIGCONT (18)")) {
            kill_process(selected_pid_, SIGCONT);
        }
        if (ImGui::MenuItem("SIGHUP (1)")) {
            kill_process(selected_pid_, SIGHUP);
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // Process table
    if (ImGui::BeginChild("ProcessList", ImVec2(0, 0), false)) {
        ImGui::Columns(8, "procCols", true);
        ImGui::SetColumnWidth(0, 70);  // PID
        ImGui::SetColumnWidth(1, 180); // Name
        ImGui::SetColumnWidth(2, 100); // User
        ImGui::SetColumnWidth(3, 50);  // State
        ImGui::SetColumnWidth(4, 80);  // CPU%
        ImGui::SetColumnWidth(5, 80);  // MEM%
        ImGui::SetColumnWidth(6, 80);  // RSS
        ImGui::SetColumnWidth(7, 60);  // Threads

        // Sortable headers
        if (ImGui::Selectable("PID##h")) set_sort(ProcessSort::Pid);
        ImGui::NextColumn();
        if (ImGui::Selectable("Name##h")) set_sort(ProcessSort::Name, true);
        ImGui::NextColumn();
        ImGui::Text("User");
        ImGui::NextColumn();
        if (ImGui::Selectable("State##h")) set_sort(ProcessSort::State, true);
        ImGui::NextColumn();
        if (ImGui::Selectable("CPU%%##h")) set_sort(ProcessSort::Cpu, false);
        ImGui::NextColumn();
        if (ImGui::Selectable("MEM%%##h")) set_sort(ProcessSort::Memory, false);
        ImGui::NextColumn();
        ImGui::Text("RSS");
        ImGui::NextColumn();
        ImGui::Text("THR");
        ImGui::NextColumn();
        ImGui::Separator();

        // Process rows
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filtered_processes_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& proc = filtered_processes_[static_cast<size_t>(i)];
                bool selected = (proc.pid == selected_pid_);

                // PID
                char pid_buf[32];
                snprintf(pid_buf, sizeof(pid_buf), "%d##pid%d", proc.pid, proc.pid);
                if (ImGui::Selectable(pid_buf, selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_pid_ = proc.pid;
                }
                ImGui::NextColumn();

                // Name
                ImGui::Text("%s", proc.comm.c_str());
                ImGui::NextColumn();

                // User
                ImGui::Text("%s", proc.username.c_str());
                ImGui::NextColumn();

                // State with color
                ImVec4 state_color;
                switch (proc.state) {
                case 'R': state_color = ImVec4(0.0f, 1.0f, 0.5f, 1.0f); break;
                case 'S': state_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); break;
                case 'D': state_color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;
                case 'Z': state_color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); break;
                case 'T': state_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); break;
                default:  state_color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); break;
                }
                ImGui::TextColored(state_color, "%c", proc.state);
                ImGui::NextColumn();

                // CPU%
                ImVec4 cpu_color;
                if (proc.cpu_percent > 50.0f) {
                    cpu_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                } else if (proc.cpu_percent > 10.0f) {
                    cpu_color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                } else {
                    cpu_color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
                }
                ImGui::TextColored(cpu_color, "%.1f", proc.cpu_percent);
                ImGui::NextColumn();

                // MEM%
                ImGui::Text("%.1f", proc.mem_percent);
                ImGui::NextColumn();

                // RSS
                ImGui::Text("%s", format_kb(proc.rss_kb).c_str());
                ImGui::NextColumn();

                // Threads
                ImGui::Text("%d", proc.threads);
                ImGui::NextColumn();
            }
        }
        clipper.End();

        ImGui::Columns(1);
    }
    ImGui::EndChild();
}

} // namespace straylight::sysmon
