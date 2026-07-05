// services/lens/trace_collector.h
// Multi-source event collection for full-stack request tracing.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight {

/// Which layer of the stack generated the event.
enum class TraceLayer : uint8_t {
    Compositor = 0,
    Ipc        = 1,
    Vpu        = 2,
    Gpu        = 3,
    App        = 4,
    Kernel     = 5,
    Network    = 6,
};

inline const char* trace_layer_name(TraceLayer layer) {
    switch (layer) {
        case TraceLayer::Compositor: return "compositor";
        case TraceLayer::Ipc:        return "ipc";
        case TraceLayer::Vpu:        return "vpu";
        case TraceLayer::Gpu:        return "gpu";
        case TraceLayer::App:        return "app";
        case TraceLayer::Kernel:     return "kernel";
        case TraceLayer::Network:    return "network";
    }
    return "unknown";
}

/// A single trace event from any layer of the stack.
struct TraceEvent {
    uint64_t timestamp_ns{0};           // Nanosecond timestamp (CLOCK_MONOTONIC)
    TraceLayer layer{TraceLayer::App};
    std::string event_type;             // e.g., "mouse_click", "dispatch", "vpu_alloc"
    std::string correlation_id;         // Links related events across layers
    pid_t pid{0};
    uint64_t duration_ns{0};            // Duration of the event (0 for instant events)
    std::map<std::string, std::string> data;  // Arbitrary key-value pairs

    // Ordering by timestamp
    bool operator<(const TraceEvent& other) const {
        return timestamp_ns < other.timestamp_ns;
    }
};

/// State of a trace collection session.
enum class TraceState : uint8_t {
    Idle,
    Collecting,
    Complete,
};

/// A complete trace: collection of events from a single tracing session.
struct Trace {
    std::string trace_id;
    std::string correlation_id;
    TraceState state{TraceState::Idle};
    uint64_t start_ns{0};
    uint64_t end_ns{0};
    std::vector<TraceEvent> events;
};

/// Collector callback: invoked when a new event arrives.
using TraceEventCallback = std::function<void(TraceEvent)>;

/// Collects trace events from multiple sources across the StrayLight stack.
class TraceCollector {
public:
    TraceCollector();
    ~TraceCollector();

    /// Start all collector threads.
    Result<void, SLError> start(const std::string& correlation_id);

    /// Stop all collector threads and return collected events.
    Result<Trace, SLError> stop();

    /// Check if currently collecting.
    [[nodiscard]] bool is_collecting() const;

    /// Get the current trace in progress (snapshot).
    Trace current_snapshot() const;

    /// Manually inject an event (for testing or custom sources).
    void inject_event(TraceEvent event);

    /// Get current monotonic timestamp in nanoseconds.
    static uint64_t now_ns();

private:
    /// Compositor event collector: reads from compositor trace socket.
    void collect_compositor_events();

    /// Whisper IPC collector: taps into the IPC debug socket.
    void collect_ipc_events();

    /// VPU collector: reads allocation events from VPU sysfs + kernel tracepoints.
    void collect_vpu_events();

    /// GPU collector: reads GPU events from debugfs.
    void collect_gpu_events();

    /// Kernel tracepoint collector: reads ftrace events.
    void collect_kernel_events();

    /// Parse a ftrace line into a TraceEvent.
    static Result<TraceEvent, std::string> parse_ftrace_line(const std::string& line,
                                                              const std::string& correlation_id);

    mutable std::mutex mutex_;
    Trace current_trace_;
    bool collecting_{false};

    // Collector threads
    std::thread compositor_thread_;
    std::thread ipc_thread_;
    std::thread vpu_thread_;
    std::thread gpu_thread_;
    std::thread kernel_thread_;

    // Socket paths
    std::string compositor_trace_sock_{"/run/straylight/compositor-trace.sock"};
    std::string whisper_debug_sock_{"/run/straylight/whisper-debug.sock"};
    std::string ftrace_pipe_{"/sys/kernel/debug/tracing/trace_pipe"};
    std::string vpu_sysfs_{"/sys/class/straylight-vpu/trace"};
    std::string gpu_debugfs_{"/sys/kernel/debug/dri/0/trace"};
};

} // namespace straylight
