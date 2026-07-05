// tools/trace/report.cpp
#include "report.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace straylight {

std::string TraceReport::format_duration(uint64_t ns) {
    if (ns < 1000) {
        return std::to_string(ns) + "ns";
    } else if (ns < 1000000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fus", static_cast<double>(ns) / 1e3);
        return buf;
    } else if (ns < 1000000000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2fms", static_cast<double>(ns) / 1e6);
        return buf;
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3fs", static_cast<double>(ns) / 1e9);
        return buf;
    }
}

std::string TraceReport::format_bytes(uint64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        return buf;
    } else if (bytes < 1024ULL * 1024 * 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return buf;
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }
}

void TraceReport::print_summary(const TraceData& data) {
    std::cout << "\n";
    std::cout << "=== StrayLight Trace Report ===\n";
    std::cout << "Command:      " << data.command << "\n";
    std::cout << "PID:          " << data.traced_pid << "\n";
    std::cout << "Exit code:    " << data.exit_code << "\n";
    std::cout << "Duration:     " << format_duration(data.total_duration_ns) << "\n";
    std::cout << "Total calls:  " << data.total_syscalls << "\n";
    std::cout << "\n";

    // Top 10 slowest syscalls by total time
    std::cout << "--- Top 10 Slowest Syscalls (by total time) ---\n";
    std::vector<std::pair<std::string, uint64_t>> sorted_time(
        data.syscall_total_time_ns.begin(), data.syscall_total_time_ns.end());
    std::sort(sorted_time.begin(), sorted_time.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::cout << std::left << std::setw(20) << "Syscall"
              << std::right << std::setw(12) << "Total Time"
              << std::setw(10) << "Count"
              << std::setw(12) << "Avg Time" << "\n";
    std::cout << std::string(54, '-') << "\n";

    int shown = 0;
    for (const auto& [name, total_ns] : sorted_time) {
        if (shown >= 10) break;
        uint64_t count = 0;
        auto it = data.syscall_counts.find(name);
        if (it != data.syscall_counts.end()) count = it->second;

        uint64_t avg_ns = count > 0 ? total_ns / count : 0;

        std::cout << std::left << std::setw(20) << name
                  << std::right << std::setw(12) << format_duration(total_ns)
                  << std::setw(10) << count
                  << std::setw(12) << format_duration(avg_ns) << "\n";
        shown++;
    }

    // Top 10 most frequent syscalls
    std::cout << "\n--- Top 10 Most Frequent Syscalls ---\n";
    std::vector<std::pair<std::string, uint64_t>> sorted_count(
        data.syscall_counts.begin(), data.syscall_counts.end());
    std::sort(sorted_count.begin(), sorted_count.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::cout << std::left << std::setw(20) << "Syscall"
              << std::right << std::setw(10) << "Count"
              << std::setw(10) << "% Total" << "\n";
    std::cout << std::string(40, '-') << "\n";

    shown = 0;
    for (const auto& [name, count] : sorted_count) {
        if (shown >= 10) break;
        double pct = data.total_syscalls > 0 ?
            (static_cast<double>(count) / static_cast<double>(data.total_syscalls)) * 100.0 : 0.0;

        char pct_buf[16];
        std::snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct);

        std::cout << std::left << std::setw(20) << name
                  << std::right << std::setw(10) << count
                  << std::setw(10) << pct_buf << "\n";
        shown++;
    }

    // File I/O hotspots
    if (!data.file_io.empty()) {
        std::cout << "\n--- File I/O Hotspots ---\n";

        auto sorted_io = data.file_io;
        std::sort(sorted_io.begin(), sorted_io.end(),
                  [](const FileIORecord& a, const FileIORecord& b) {
                      return a.total_latency_ns > b.total_latency_ns;
                  });

        std::cout << std::left << std::setw(40) << "Path"
                  << std::right << std::setw(10) << "Read"
                  << std::setw(10) << "Write"
                  << std::setw(12) << "Latency" << "\n";
        std::cout << std::string(72, '-') << "\n";

        shown = 0;
        for (const auto& fio : sorted_io) {
            if (shown >= 15) break;
            std::string display_path = fio.path;
            if (display_path.size() > 38) {
                display_path = "..." + display_path.substr(display_path.size() - 35);
            }

            std::cout << std::left << std::setw(40) << display_path
                      << std::right << std::setw(10) << format_bytes(fio.read_bytes)
                      << std::setw(10) << format_bytes(fio.write_bytes)
                      << std::setw(12) << format_duration(fio.total_latency_ns) << "\n";
            shown++;
        }
    }

    // Network connections
    if (!data.network.empty()) {
        std::cout << "\n--- Network Connections ---\n";
        std::cout << std::left << std::setw(30) << "Remote"
                  << std::right << std::setw(12) << "Sent"
                  << std::setw(12) << "Received"
                  << std::setw(12) << "Latency" << "\n";
        std::cout << std::string(66, '-') << "\n";

        for (const auto& net : data.network) {
            std::cout << std::left << std::setw(30) << net.remote_addr
                      << std::right << std::setw(12) << format_bytes(net.bytes_sent)
                      << std::setw(12) << format_bytes(net.bytes_received)
                      << std::setw(12) << format_duration(net.connect_latency_ns) << "\n";
        }
    }

    // Memory summary
    std::cout << "\n--- Memory Usage ---\n";
    std::cout << "mmap calls:   " << data.memory.mmap_calls
              << " (" << format_bytes(data.memory.mmap_total_bytes) << " total)\n";
    std::cout << "munmap calls: " << data.memory.munmap_calls << "\n";
    std::cout << "brk calls:    " << data.memory.brk_calls << "\n";
    if (data.memory.peak_brk > 0) {
        std::cout << "Peak brk:     0x" << std::hex << data.memory.peak_brk << std::dec << "\n";
    }
    std::cout << "mprotect:     " << data.memory.mprotect_calls << "\n";

    // Signals
    if (!data.signals.empty()) {
        std::cout << "\n--- Signals ---\n";
        for (const auto& sig : data.signals) {
            std::cout << sig.signal_name << ": " << sig.count << " occurrence(s)\n";
        }
    }

    std::cout << "\n=== End Report ===\n";
}

Result<void, SLError> TraceReport::export_chrome_trace(const TraceData& data,
                                                         const std::string& output_path) {
    nlohmann::json trace;
    trace["traceEvents"] = nlohmann::json::array();

    auto& events = trace["traceEvents"];

    for (const auto& ev : data.events) {
        nlohmann::json entry;
        entry["name"] = ev.syscall_name;
        entry["cat"] = "syscall";
        entry["ph"] = "X";  // Complete event
        entry["ts"] = ev.timestamp_ns / 1000;  // Chrome trace uses microseconds
        entry["dur"] = ev.duration_ns / 1000;
        entry["pid"] = ev.pid;
        entry["tid"] = ev.tid;

        nlohmann::json args;
        args["syscall_nr"] = ev.syscall_nr;
        args["return"] = ev.return_value;
        if (!ev.args.empty()) {
            nlohmann::json a;
            for (size_t i = 0; i < ev.args.size(); ++i) {
                a["arg" + std::to_string(i)] = ev.args[i];
            }
            args["syscall_args"] = a;
        }
        entry["args"] = args;

        events.push_back(entry);
    }

    // Add metadata
    nlohmann::json meta;
    meta["name"] = "process_name";
    meta["ph"] = "M";
    meta["pid"] = data.traced_pid;
    meta["args"]["name"] = data.command;
    events.push_back(meta);

    trace["displayTimeUnit"] = "ms";

    std::ofstream f(output_path);
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + output_path + " for writing"});
    }

    f << trace.dump(2);

    return Result<void, SLError>::ok();
}

Result<void, SLError> TraceReport::export_json(const TraceData& data,
                                                 const std::string& output_path) {
    nlohmann::json j;
    j["command"] = data.command;
    j["pid"] = data.traced_pid;
    j["exit_code"] = data.exit_code;
    j["start_time_ns"] = data.start_time_ns;
    j["end_time_ns"] = data.end_time_ns;
    j["total_duration_ns"] = data.total_duration_ns;
    j["total_syscalls"] = data.total_syscalls;

    j["syscall_counts"] = data.syscall_counts;
    j["syscall_total_time_ns"] = data.syscall_total_time_ns;

    // File I/O
    nlohmann::json fio_arr = nlohmann::json::array();
    for (const auto& fio : data.file_io) {
        nlohmann::json f;
        f["path"] = fio.path;
        f["read_bytes"] = fio.read_bytes;
        f["write_bytes"] = fio.write_bytes;
        f["read_calls"] = fio.read_calls;
        f["write_calls"] = fio.write_calls;
        f["total_latency_ns"] = fio.total_latency_ns;
        f["open_count"] = fio.open_count;
        fio_arr.push_back(f);
    }
    j["file_io"] = fio_arr;

    // Network
    nlohmann::json net_arr = nlohmann::json::array();
    for (const auto& net : data.network) {
        nlohmann::json n;
        n["remote_addr"] = net.remote_addr;
        n["local_addr"] = net.local_addr;
        n["protocol"] = net.protocol;
        n["bytes_sent"] = net.bytes_sent;
        n["bytes_received"] = net.bytes_received;
        n["connect_latency_ns"] = net.connect_latency_ns;
        net_arr.push_back(n);
    }
    j["network"] = net_arr;

    // Memory
    j["memory"] = {
        {"mmap_calls", data.memory.mmap_calls},
        {"mmap_total_bytes", data.memory.mmap_total_bytes},
        {"munmap_calls", data.memory.munmap_calls},
        {"brk_calls", data.memory.brk_calls},
        {"peak_brk", data.memory.peak_brk},
        {"mprotect_calls", data.memory.mprotect_calls}
    };

    // Signals
    nlohmann::json sig_arr = nlohmann::json::array();
    for (const auto& sig : data.signals) {
        nlohmann::json s;
        s["signal_nr"] = sig.signal_nr;
        s["signal_name"] = sig.signal_name;
        s["count"] = sig.count;
        sig_arr.push_back(s);
    }
    j["signals"] = sig_arr;

    // Events (can be large)
    nlohmann::json ev_arr = nlohmann::json::array();
    for (const auto& ev : data.events) {
        nlohmann::json e;
        e["timestamp_ns"] = ev.timestamp_ns;
        e["pid"] = ev.pid;
        e["tid"] = ev.tid;
        e["syscall_nr"] = ev.syscall_nr;
        e["syscall_name"] = ev.syscall_name;
        e["return_value"] = ev.return_value;
        e["duration_ns"] = ev.duration_ns;
        e["args"] = ev.args;
        ev_arr.push_back(e);
    }
    j["events"] = ev_arr;

    std::ofstream f(output_path);
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + output_path + " for writing"});
    }

    f << j.dump(2);
    return Result<void, SLError>::ok();
}

Result<TraceData, SLError> TraceReport::import_json(const std::string& input_path) {
    std::ifstream f(input_path);
    if (!f.is_open()) {
        return Result<TraceData, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + input_path});
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::parse_error& e) {
        return Result<TraceData, SLError>::error(
            SLError{SLErrorCode::ParseError, std::string("JSON parse error: ") + e.what()});
    }

    TraceData data;
    data.command = j.value("command", "");
    data.traced_pid = j.value("pid", 0);
    data.exit_code = j.value("exit_code", -1);
    data.start_time_ns = j.value("start_time_ns", static_cast<uint64_t>(0));
    data.end_time_ns = j.value("end_time_ns", static_cast<uint64_t>(0));
    data.total_duration_ns = j.value("total_duration_ns", static_cast<uint64_t>(0));
    data.total_syscalls = j.value("total_syscalls", static_cast<uint64_t>(0));

    if (j.contains("syscall_counts") && j["syscall_counts"].is_object()) {
        for (auto& [k, v] : j["syscall_counts"].items()) {
            data.syscall_counts[k] = v.get<uint64_t>();
        }
    }

    if (j.contains("syscall_total_time_ns") && j["syscall_total_time_ns"].is_object()) {
        for (auto& [k, v] : j["syscall_total_time_ns"].items()) {
            data.syscall_total_time_ns[k] = v.get<uint64_t>();
        }
    }

    if (j.contains("file_io") && j["file_io"].is_array()) {
        for (const auto& fj : j["file_io"]) {
            FileIORecord fio;
            fio.path = fj.value("path", "");
            fio.read_bytes = fj.value("read_bytes", static_cast<uint64_t>(0));
            fio.write_bytes = fj.value("write_bytes", static_cast<uint64_t>(0));
            fio.read_calls = fj.value("read_calls", static_cast<uint64_t>(0));
            fio.write_calls = fj.value("write_calls", static_cast<uint64_t>(0));
            fio.total_latency_ns = fj.value("total_latency_ns", static_cast<uint64_t>(0));
            fio.open_count = fj.value("open_count", static_cast<uint64_t>(0));
            data.file_io.push_back(fio);
        }
    }

    if (j.contains("network") && j["network"].is_array()) {
        for (const auto& nj : j["network"]) {
            NetworkRecord net;
            net.remote_addr = nj.value("remote_addr", "");
            net.local_addr = nj.value("local_addr", "");
            net.protocol = nj.value("protocol", "");
            net.bytes_sent = nj.value("bytes_sent", static_cast<uint64_t>(0));
            net.bytes_received = nj.value("bytes_received", static_cast<uint64_t>(0));
            net.connect_latency_ns = nj.value("connect_latency_ns", static_cast<uint64_t>(0));
            data.network.push_back(net);
        }
    }

    if (j.contains("memory") && j["memory"].is_object()) {
        auto& m = j["memory"];
        data.memory.mmap_calls = m.value("mmap_calls", static_cast<uint64_t>(0));
        data.memory.mmap_total_bytes = m.value("mmap_total_bytes", static_cast<uint64_t>(0));
        data.memory.munmap_calls = m.value("munmap_calls", static_cast<uint64_t>(0));
        data.memory.brk_calls = m.value("brk_calls", static_cast<uint64_t>(0));
        data.memory.peak_brk = m.value("peak_brk", static_cast<uint64_t>(0));
        data.memory.mprotect_calls = m.value("mprotect_calls", static_cast<uint64_t>(0));
    }

    if (j.contains("signals") && j["signals"].is_array()) {
        for (const auto& sj : j["signals"]) {
            SignalRecord sig;
            sig.signal_nr = sj.value("signal_nr", 0);
            sig.signal_name = sj.value("signal_name", "");
            sig.count = sj.value("count", static_cast<uint64_t>(0));
            data.signals.push_back(sig);
        }
    }

    if (j.contains("events") && j["events"].is_array()) {
        for (const auto& ej : j["events"]) {
            SyscallEvent ev;
            ev.timestamp_ns = ej.value("timestamp_ns", static_cast<uint64_t>(0));
            ev.pid = ej.value("pid", 0);
            ev.tid = ej.value("tid", 0);
            ev.syscall_nr = ej.value("syscall_nr", 0);
            ev.syscall_name = ej.value("syscall_name", "");
            ev.return_value = ej.value("return_value", static_cast<int64_t>(0));
            ev.duration_ns = ej.value("duration_ns", static_cast<uint64_t>(0));
            if (ej.contains("args") && ej["args"].is_array()) {
                for (const auto& a : ej["args"]) {
                    ev.args.push_back(a.get<uint64_t>());
                }
            }
            data.events.push_back(ev);
        }
    }

    return Result<TraceData, SLError>::ok(std::move(data));
}

Result<void, SLError> TraceReport::export_flamegraph(const TraceData& data,
                                                       const std::string& output_path) {
    // Generate folded stacks format for FlameGraph tools.
    // Stack: command;syscall_name count
    std::ofstream f(output_path);
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + output_path + " for writing"});
    }

    // Group by syscall name, use time as weight
    std::vector<std::pair<std::string, uint64_t>> sorted_time(
        data.syscall_total_time_ns.begin(), data.syscall_total_time_ns.end());
    std::sort(sorted_time.begin(), sorted_time.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [name, total_ns] : sorted_time) {
        // Folded stack format: stack;frame weight
        // Weight in microseconds for reasonable flamegraph scale
        uint64_t us = total_ns / 1000;
        if (us == 0) us = 1;
        f << data.command << ";" << name << " " << us << "\n";
    }

    // Also group file I/O by path
    for (const auto& fio : data.file_io) {
        if (fio.total_latency_ns == 0) continue;
        uint64_t us = fio.total_latency_ns / 1000;
        if (us == 0) us = 1;

        std::string short_path = fio.path;
        if (short_path.size() > 40) {
            short_path = short_path.substr(short_path.size() - 40);
        }
        // Replace semicolons in path (flamegraph separator)
        for (char& c : short_path) {
            if (c == ';') c = ':';
        }

        f << data.command << ";file_io;" << short_path << " " << us << "\n";
    }

    return Result<void, SLError>::ok();
}

} // namespace straylight
