// services/replay/analyzer.cpp
// Event analysis and timeline construction implementation.
#include "analyzer.h"

#include <straylight/log.h>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

namespace straylight {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static uint64_t monotonic_ns_now() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static std::string ns_to_relative(uint64_t ns, uint64_t ref_ns) {
    if (ns >= ref_ns) return "+0.000s";
    double delta = static_cast<double>(ref_ns - ns) / 1'000'000'000.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "-%.3fs", delta);
    return buf;
}

static std::string ns_to_duration(uint64_t ns) {
    double secs = static_cast<double>(ns) / 1'000'000'000.0;
    if (secs < 60.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", secs);
        return buf;
    } else if (secs < 3600.0) {
        int m = static_cast<int>(secs) / 60;
        int s = static_cast<int>(secs) % 60;
        return std::to_string(m) + "m" + std::to_string(s) + "s";
    } else {
        int h = static_cast<int>(secs) / 3600;
        int m = (static_cast<int>(secs) % 3600) / 60;
        return std::to_string(h) + "h" + std::to_string(m) + "m";
    }
}

static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// --------------------------------------------------------------------------
// EventAnalyzer
// --------------------------------------------------------------------------

EventAnalyzer::EventAnalyzer(const EventRecorder& recorder)
    : recorder_(recorder) {}

Result<Timeline, std::string> EventAnalyzer::build_timeline(
    uint64_t start_ns, uint64_t end_ns) {

    if (start_ns >= end_ns) {
        return Result<Timeline, std::string>::error(
            "Invalid time range: start must be before end");
    }

    auto events = recorder_.events_in_range(start_ns, end_ns);

    Timeline tl;
    tl.start_ns = start_ns;
    tl.end_ns = end_ns;
    tl.total_events = events.size();
    tl.all_events = events;

    // Segment into 10-second windows
    uint64_t window = 10'000'000'000ULL;  // 10 seconds
    tl.segments = segment_events(events, window);

    return Result<Timeline, std::string>::ok(std::move(tl));
}

Result<Timeline, std::string> EventAnalyzer::build_timeline_last(uint64_t seconds) {
    uint64_t now = monotonic_ns_now();
    uint64_t duration = seconds * 1'000'000'000ULL;
    uint64_t start = (now > duration) ? (now - duration) : 0;
    return build_timeline(start, now);
}

Result<CrashReport, std::string> EventAnalyzer::analyze_crash() {
    auto all = recorder_.snapshot();
    if (all.empty()) {
        return Result<CrashReport, std::string>::error(
            "No events recorded — nothing to analyze");
    }

    // Find the most recent crash-related event
    const SystemEvent* crash_event = nullptr;
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (it->type == EventType::OomKill ||
            it->type == EventType::Panic ||
            it->type == EventType::ServiceFail) {
            crash_event = &(*it);
            break;
        }
    }

    if (!crash_event) {
        // Also check for unexpected process exits with non-zero codes
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->type == EventType::ProcessExit &&
                it->detail.find("\"exit_code\":0") == std::string::npos) {
                crash_event = &(*it);
                break;
            }
        }
    }

    if (!crash_event) {
        return Result<CrashReport, std::string>::error(
            "No crash, OOM, panic, or service failure found in recorded events");
    }

    CrashReport report;
    report.crash_timestamp_ns = crash_event->timestamp_ns;
    report.crash_type = crash_event->type;
    report.crash_detail = crash_event->detail;
    report.crash_process = crash_event->process_name;

    // Gather events from the 30 seconds before the crash
    uint64_t window = 30'000'000'000ULL;  // 30 seconds
    uint64_t window_start = (crash_event->timestamp_ns > window)
                                ? (crash_event->timestamp_ns - window)
                                : 0;

    for (const auto& ev : all) {
        if (ev.timestamp_ns >= window_start &&
            ev.timestamp_ns <= crash_event->timestamp_ns) {
            report.preceding_events.push_back(ev);
        }
    }

    report.summary = summarize_crash(report);

    return Result<CrashReport, std::string>::ok(std::move(report));
}

Result<std::vector<SystemEvent>, std::string> EventAnalyzer::filter(
    EventType type, int pid) {

    auto events = recorder_.query([type, pid](const SystemEvent& ev) {
        if (ev.type != type) return false;
        if (pid >= 0 && static_cast<int>(ev.pid) != pid) return false;
        return true;
    });

    return Result<std::vector<SystemEvent>, std::string>::ok(std::move(events));
}

Result<std::vector<SystemEvent>, std::string> EventAnalyzer::search(
    const std::string& pattern) {

    if (pattern.empty()) {
        return Result<std::vector<SystemEvent>, std::string>::error(
            "Search pattern cannot be empty");
    }

    auto events = recorder_.query([&pattern](const SystemEvent& ev) {
        if (ev.detail.find(pattern) != std::string::npos) return true;
        if (ev.process_name.find(pattern) != std::string::npos) return true;
        return false;
    });

    return Result<std::vector<SystemEvent>, std::string>::ok(std::move(events));
}

Result<std::string, std::string> EventAnalyzer::format_timeline(const Timeline& t) {
    std::ostringstream out;

    uint64_t duration_ns = t.end_ns - t.start_ns;
    out << "=== StrayLight Event Timeline ===\n";
    out << "Duration: " << ns_to_duration(duration_ns) << "\n";
    out << "Events:   " << t.total_events << "\n";
    out << "================================\n\n";

    // Count by type
    std::map<EventType, size_t> type_counts;
    for (const auto& ev : t.all_events) {
        type_counts[ev.type]++;
    }

    out << "Event breakdown:\n";
    for (const auto& [type, count] : type_counts) {
        out << "  " << event_type_name(type) << ": " << count << "\n";
    }
    out << "\n";

    // Print each event
    out << "Timeline:\n";
    for (const auto& ev : t.all_events) {
        out << "  [" << ns_to_relative(t.end_ns - (t.end_ns - ev.timestamp_ns), t.end_ns)
            << "] " << event_type_name(ev.type);

        if (!ev.process_name.empty()) {
            out << " (" << ev.process_name;
            if (ev.pid > 0) out << ":" << ev.pid;
            out << ")";
        }

        if (!ev.detail.empty()) {
            // Print compact detail
            std::string d = ev.detail;
            if (d.size() > 80) d = d.substr(0, 77) + "...";
            out << " " << d;
        }

        out << "\n";
    }

    return Result<std::string, std::string>::ok(out.str());
}

Result<std::string, std::string> EventAnalyzer::export_json(const Timeline& t) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"start_ns\": " << t.start_ns << ",\n";
    out << "  \"end_ns\": " << t.end_ns << ",\n";
    out << "  \"total_events\": " << t.total_events << ",\n";
    out << "  \"duration_seconds\": "
        << static_cast<double>(t.end_ns - t.start_ns) / 1'000'000'000.0 << ",\n";
    out << "  \"events\": [\n";

    for (size_t i = 0; i < t.all_events.size(); ++i) {
        const auto& ev = t.all_events[i];
        out << "    {\n";
        out << "      \"timestamp_ns\": " << ev.timestamp_ns << ",\n";
        out << "      \"type\": \"" << event_type_name(ev.type) << "\",\n";
        out << "      \"pid\": " << ev.pid << ",\n";
        out << "      \"uid\": " << ev.uid << ",\n";
        out << "      \"process_name\": \"" << escape_json(ev.process_name) << "\",\n";
        out << "      \"detail\": \"" << escape_json(ev.detail) << "\"\n";
        out << "    }";
        if (i + 1 < t.all_events.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return Result<std::string, std::string>::ok(out.str());
}

Result<std::string, std::string> EventAnalyzer::export_crash_json(
    const CrashReport& report) {

    std::ostringstream out;

    out << "{\n";
    out << "  \"crash_timestamp_ns\": " << report.crash_timestamp_ns << ",\n";
    out << "  \"crash_type\": \"" << event_type_name(report.crash_type) << "\",\n";
    out << "  \"crash_process\": \"" << escape_json(report.crash_process) << "\",\n";
    out << "  \"crash_detail\": \"" << escape_json(report.crash_detail) << "\",\n";
    out << "  \"summary\": \"" << escape_json(report.summary) << "\",\n";
    out << "  \"preceding_events\": [\n";

    for (size_t i = 0; i < report.preceding_events.size(); ++i) {
        const auto& ev = report.preceding_events[i];
        out << "    {\n";
        out << "      \"timestamp_ns\": " << ev.timestamp_ns << ",\n";
        out << "      \"type\": \"" << event_type_name(ev.type) << "\",\n";
        out << "      \"pid\": " << ev.pid << ",\n";
        out << "      \"process_name\": \"" << escape_json(ev.process_name) << "\",\n";
        out << "      \"detail\": \"" << escape_json(ev.detail) << "\"\n";
        out << "    }";
        if (i + 1 < report.preceding_events.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return Result<std::string, std::string>::ok(out.str());
}

// --------------------------------------------------------------------------
// Private helpers
// --------------------------------------------------------------------------

std::vector<TimelineSegment> EventAnalyzer::segment_events(
    const std::vector<SystemEvent>& events, uint64_t window_ns) {

    std::vector<TimelineSegment> segments;
    if (events.empty()) return segments;

    uint64_t seg_start = events.front().timestamp_ns;
    TimelineSegment current;
    current.start_ns = seg_start;
    current.end_ns = seg_start + window_ns;

    // Count dominant event type for label
    std::map<EventType, size_t> type_counts;

    for (const auto& ev : events) {
        if (ev.timestamp_ns > current.end_ns) {
            // Finalize current segment
            if (!current.events.empty()) {
                // Label from dominant type
                EventType dominant = current.events.front().type;
                size_t max_count = 0;
                for (const auto& [t, c] : type_counts) {
                    if (c > max_count) {
                        max_count = c;
                        dominant = t;
                    }
                }
                current.label = std::string(event_type_name(dominant)) + " (" +
                                std::to_string(current.events.size()) + " events)";
                segments.push_back(std::move(current));
            }

            // Start new segment
            type_counts.clear();
            current = TimelineSegment{};
            current.start_ns = ev.timestamp_ns;
            current.end_ns = ev.timestamp_ns + window_ns;
        }

        current.events.push_back(ev);
        type_counts[ev.type]++;
    }

    // Finalize last segment
    if (!current.events.empty()) {
        EventType dominant = current.events.front().type;
        size_t max_count = 0;
        for (const auto& [t, c] : type_counts) {
            if (c > max_count) {
                max_count = c;
                dominant = t;
            }
        }
        current.label = std::string(event_type_name(dominant)) + " (" +
                        std::to_string(current.events.size()) + " events)";
        segments.push_back(std::move(current));
    }

    return segments;
}

std::string EventAnalyzer::summarize_crash(const CrashReport& report) {
    std::ostringstream out;

    out << "Crash Analysis Report\n";
    out << "=====================\n\n";

    // Crash description
    switch (report.crash_type) {
        case EventType::OomKill:
            out << "TYPE: Out-of-Memory Kill\n";
            out << "The kernel OOM killer terminated process '"
                << report.crash_process << "'.\n";
            break;
        case EventType::Panic:
            out << "TYPE: Kernel Panic\n";
            out << "A kernel panic occurred. System state before panic:\n";
            break;
        case EventType::ServiceFail:
            out << "TYPE: Service Failure\n";
            out << "Service '" << report.crash_process << "' entered failed state.\n";
            break;
        default:
            out << "TYPE: Abnormal Process Exit\n";
            out << "Process '" << report.crash_process
                << "' exited abnormally.\n";
            break;
    }

    out << "\nDetail: " << report.crash_detail << "\n";

    // Analyze preceding events for patterns
    size_t proc_starts = 0;
    size_t proc_exits = 0;
    size_t mem_events = 0;
    size_t net_events = 0;
    size_t svc_fails = 0;

    for (const auto& ev : report.preceding_events) {
        switch (ev.type) {
            case EventType::ProcessStart: proc_starts++; break;
            case EventType::ProcessExit:  proc_exits++; break;
            case EventType::GpuAlloc:
            case EventType::GpuFree:      mem_events++; break;
            case EventType::NetworkConnect:
            case EventType::NetworkListen: net_events++; break;
            case EventType::ServiceFail:  svc_fails++; break;
            default: break;
        }
    }

    out << "\nPreceding Activity (30s window):\n";
    out << "  Total events: " << report.preceding_events.size() << "\n";
    out << "  Process starts: " << proc_starts << "\n";
    out << "  Process exits: " << proc_exits << "\n";
    out << "  GPU memory events: " << mem_events << "\n";
    out << "  Network events: " << net_events << "\n";
    out << "  Service failures: " << svc_fails << "\n";

    // Pattern detection
    out << "\nPossible Contributing Factors:\n";

    if (report.crash_type == EventType::OomKill && mem_events > 5) {
        out << "  - High GPU memory churn detected before OOM. "
            << "GPU memory fragmentation may have contributed.\n";
    }

    if (proc_starts > 20) {
        out << "  - Rapid process spawning detected (" << proc_starts
            << " starts in 30s). Possible fork bomb or runaway service.\n";
    }

    if (svc_fails > 0) {
        out << "  - " << svc_fails << " service failure(s) preceded the crash. "
            << "Cascading failure suspected.\n";
    }

    if (proc_exits > proc_starts + 5) {
        out << "  - More exits than starts — processes may be crash-looping.\n";
    }

    if (report.preceding_events.size() < 3) {
        out << "  - Very few events before crash. This may have been sudden "
            << "(hardware fault, external signal).\n";
    }

    return out.str();
}

} // namespace straylight
