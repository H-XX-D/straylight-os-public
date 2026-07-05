// services/swarm/orchestrator.h
#pragma once

#include <straylight/result.h>
#include "discovery.h"
#include "node_client.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

/// Placement strategy for task assignment.
enum class PlacementStrategy {
    GpuAffinity,  // Place on node with most free VRAM
    CpuAffinity,  // Place on node with most free CPU
    Spread,       // Distribute evenly across nodes
    Pack,         // Fill one node before moving to next
};

/// State of a submitted swarm task.
enum class TaskState {
    Pending,      // Queued, not yet assigned
    Running,      // Executing on a remote node
    Completed,    // Finished successfully
    Failed,       // Finished with error
    Cancelled,    // Cancelled by user
};

/// A task to be distributed across the swarm.
struct SwarmTask {
    std::string task_id;
    std::string command;              // Shell command to execute
    std::string working_dir = "/tmp";
    PlacementStrategy strategy = PlacementStrategy::GpuAffinity;

    // Resource requirements (0 = no constraint)
    int min_gpu_count     = 0;
    uint64_t min_vram     = 0;
    int min_cpu_cores     = 0;
    uint64_t min_memory   = 0;

    // Timeout (0 = no timeout)
    int timeout_seconds   = 0;

    // Optional: target a specific node
    std::string target_node_id;
};

/// Status of a submitted task.
struct SwarmTaskStatus {
    std::string task_id;
    TaskState state = TaskState::Pending;
    std::string assigned_node_id;
    std::string assigned_hostname;
    std::string output;              // stdout from remote execution
    std::string error_message;
    int exit_code = -1;
    std::chrono::steady_clock::time_point submitted_at;
    std::chrono::steady_clock::time_point completed_at;
};

/// Orchestrates task distribution across discovered swarm nodes.
class SwarmOrchestrator {
public:
    explicit SwarmOrchestrator(NodeDiscovery& discovery, NodeClient& client);

    /// Submit a task for execution. Returns the assigned task_id.
    Result<std::string, std::string> submit_task(const SwarmTask& task);

    /// Get the current status of a task.
    Result<SwarmTaskStatus, std::string> task_status(const std::string& task_id) const;

    /// Cancel a pending or running task.
    Result<void, std::string> cancel_task(const std::string& task_id);

    /// Get all known nodes (delegates to discovery).
    std::vector<SwarmNode> nodes() const;

    /// Process pending tasks: assign them to nodes and poll running ones.
    void process_tasks();

    /// Get counts for status display.
    size_t pending_count() const;
    size_t running_count() const;
    size_t completed_count() const;

private:
    /// Select the best node for a task based on its placement strategy.
    Result<SwarmNode, std::string> select_node(const SwarmTask& task);

    /// Check if a node meets the resource requirements of a task.
    bool node_meets_requirements(const SwarmNode& node, const SwarmTask& task) const;

    /// Generate a unique task ID.
    std::string generate_task_id();

    NodeDiscovery& discovery_;
    NodeClient& client_;

    mutable std::mutex tasks_mutex_;
    std::unordered_map<std::string, SwarmTask> tasks_;
    std::unordered_map<std::string, SwarmTaskStatus> task_statuses_;

    // Round-robin counter for spread strategy
    size_t spread_counter_ = 0;

    // Task-per-node counter for pack strategy
    std::unordered_map<std::string, int> node_task_count_;

    uint64_t next_task_seq_ = 1;
};

} // namespace straylight
