// services/replay/analyzer.h
// Event analysis and timeline construction for the flight recorder.
#pragma once

#include "recorder.h"

#include <straylight/result.h>

#include <chrono>
#include <string>
#include <vector>

namespace straylight {

/// A segment of a timeline grouping related events.
struct TimelineSegment {
    uint64_t start_ns;
    uint64_t end_ns;
    std::string label;
    std::vector<SystemEvent> events;
};

/// A complete timeline of system events over a period.
struct Timeline {
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t total_events;
    std::vector<TimelineSegment> segments;
    std::vector<SystemEvent> all_events;
};

/// A crash analysis report.
struct CrashReport {
    uint64_t crash_timestamp_ns;
    EventType crash_type;         ///< OomKill, Panic, ServiceFail, etc.
    std::string crash_detail;
    std::string crash_process;
    std::vector<SystemEvent> preceding_events;  ///< Events in the 30s before crash
    std::string summary;          ///< Human-readable analysis
};

/// Analyzes recorded events, builds timelines, and generates reports.
class EventAnalyzer {
public:
    explicit EventAnalyzer(const EventRecorder& recorder);

    /// Build a timeline between two time points (monotonic nanoseconds).
    Result<Timeline, std::string> build_timeline(uint64_t start_ns, uint64_t end_ns);

    /// Build a timeline for the last N seconds.
    Result<Timeline, std::string> build_timeline_last(uint64_t seconds);

    /// Analyze the most recent crash, OOM, panic, or service failure.
    Result<CrashReport, std::string> analyze_crash();

    /// Filter events by type and optionally by PID.
    Result<std::vector<SystemEvent>, std::string> filter(EventType type, int pid = -1);

    /// Search event details for a pattern (substring match).
    Result<std::vector<SystemEvent>, std::string> search(const std::string& pattern);

    /// Format a timeline as a human-readable string.
    Result<std::string, std::string> format_timeline(const Timeline& t);

    /// Export a timeline as a JSON string.
    Result<std::string, std::string> export_json(const Timeline& t);

    /// Export a crash report as a JSON string.
    Result<std::string, std::string> export_crash_json(const CrashReport& report);

private:
    const EventRecorder& recorder_;

    /// Group events into timeline segments by time windows.
    std::vector<TimelineSegment> segment_events(
        const std::vector<SystemEvent>& events,
        uint64_t window_ns);

    /// Generate a human-readable summary of a crash.
    std::string summarize_crash(const CrashReport& report);
};

} // namespace straylight
