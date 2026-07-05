// services/swarm/orchestrator.cpp
#include "orchestrator.h"
#include <straylight/log.h>

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

namespace straylight {

SwarmOrchestrator::SwarmOrchestrator(NodeDiscovery& discovery, NodeClient& client)
    : discovery_(discovery), client_(client) {}

// ---------------------------------------------------------------------------
// Task ID generation
// ---------------------------------------------------------------------------

std::string SwarmOrchestrator::generate_task_id() {
    // Format: "task-<seq>-<random8hex>"
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    std::ostringstream oss;
    oss << "task-" << next_task_seq_++ << "-"
        << std::hex << std::setfill('0') << std::setw(8) << dist(rng);
    return oss.str();
}

// ---------------------------------------------------------------------------
// Submit
// ---------------------------------------------------------------------------

Result<std::string, std::string> SwarmOrchestrator::submit_task(const SwarmTask& task) {
    std::lock_guard lock(tasks_mutex_);

    SwarmTask t = task;
    if (t.task_id.empty()) {
        t.task_id = generate_task_id();
    }

    SwarmTaskStatus status;
    status.task_id = t.task_id;
    status.state = TaskState::Pending;
    status.submitted_at = std::chrono::steady_clock::now();

    tasks_[t.task_id] = std::move(t);
    task_statuses_[status.task_id] = std::move(status);

    SL_INFO("swarm: task {} submitted (command='{}', strategy={})",
            tasks_[task.task_id].task_id, tasks_[task.task_id].command,
            static_cast<int>(tasks_[task.task_id].strategy));

    return Result<std::string, std::string>::ok(tasks_[task.task_id].task_id);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

Result<SwarmTaskStatus, std::string> SwarmOrchestrator::task_status(const std::string& task_id) const {
    std::lock_guard lock(tasks_mutex_);
    auto it = task_statuses_.find(task_id);
    if (it == task_statuses_.end()) {
        return Result<SwarmTaskStatus, std::string>::error("task not found: " + task_id);
    }
    return Result<SwarmTaskStatus, std::string>::ok(it->second);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

Result<void, std::string> SwarmOrchestrator::cancel_task(const std::string& task_id) {
    std::lock_guard lock(tasks_mutex_);
    auto it = task_statuses_.find(task_id);
    if (it == task_statuses_.end()) {
        return Result<void, std::string>::error("task not found: " + task_id);
    }

    if (it->second.state == TaskState::Completed || it->second.state == TaskState::Failed) {
        return Result<void, std::string>::error("task already finished");
    }

    if (it->second.state == TaskState::Running && !it->second.assigned_node_id.empty()) {
        // Send cancel request to remote node
        auto r = client_.cancel_remote_task(it->second.assigned_node_id, task_id);
        if (!r.has_value()) {
            SL_WARN("swarm: failed to cancel remote task {}: {}", task_id, r.error());
        }
    }

    it->second.state = TaskState::Cancelled;
    it->second.completed_at = std::chrono::steady_clock::now();
    SL_INFO("swarm: task {} cancelled", task_id);

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

std::vector<SwarmNode> SwarmOrchestrator::nodes() const {
    return discovery_.nodes();
}

// ---------------------------------------------------------------------------
// Node selection
// ---------------------------------------------------------------------------

bool SwarmOrchestrator::node_meets_requirements(const SwarmNode& node, const SwarmTask& task) const {
    if (task.min_gpu_count > 0 && node.gpu_count < task.min_gpu_count) return false;
    if (task.min_vram > 0 && node.vram_free < task.min_vram) return false;
    if (task.min_cpu_cores > 0 && node.cpu_cores < task.min_cpu_cores) return false;
    if (task.min_memory > 0 && node.mem_free < task.min_memory) return false;
    return true;
}

Result<SwarmNode, std::string> SwarmOrchestrator::select_node(const SwarmTask& task) {
    // If a specific node is targeted, use it
    if (!task.target_node_id.empty()) {
        const auto* node = discovery_.find_node(task.target_node_id);
        if (!node) {
            return Result<SwarmNode, std::string>::error("target node not found: " + task.target_node_id);
        }
        if (!node_meets_requirements(*node, task)) {
            return Result<SwarmNode, std::string>::error("target node does not meet resource requirements");
        }
        return Result<SwarmNode, std::string>::ok(*node);
    }

    auto all_nodes = discovery_.nodes();

    // Filter to eligible nodes (not self, meets requirements)
    std::vector<SwarmNode> eligible;
    for (const auto& n : all_nodes) {
        if (!n.is_self && node_meets_requirements(n, task)) {
            eligible.push_back(n);
        }
    }

    // Fall back to self if no remote nodes available
    if (eligible.empty()) {
        for (const auto& n : all_nodes) {
            if (n.is_self && node_meets_requirements(n, task)) {
                eligible.push_back(n);
                break;
            }
        }
    }

    if (eligible.empty()) {
        return Result<SwarmNode, std::string>::error("no eligible nodes for task requirements");
    }

    switch (task.strategy) {
    case PlacementStrategy::GpuAffinity: {
        // Sort by free VRAM descending
        std::sort(eligible.begin(), eligible.end(),
                  [](const SwarmNode& a, const SwarmNode& b) { return a.vram_free > b.vram_free; });
        return Result<SwarmNode, std::string>::ok(eligible.front());
    }

    case PlacementStrategy::CpuAffinity: {
        // Sort by inverse load (lower load = more free CPU)
        std::sort(eligible.begin(), eligible.end(),
                  [](const SwarmNode& a, const SwarmNode& b) { return a.load_1m < b.load_1m; });
        return Result<SwarmNode, std::string>::ok(eligible.front());
    }

    case PlacementStrategy::Spread: {
        // Round-robin across eligible nodes
        size_t idx = spread_counter_ % eligible.size();
        return Result<SwarmNode, std::string>::ok(eligible[idx]);
    }

    case PlacementStrategy::Pack: {
        // Sort by current task count descending (fill most-used first)
        std::sort(eligible.begin(), eligible.end(),
                  [this](const SwarmNode& a, const SwarmNode& b) {
                      auto ca = node_task_count_.count(a.node_id) ? node_task_count_.at(a.node_id) : 0;
                      auto cb = node_task_count_.count(b.node_id) ? node_task_count_.at(b.node_id) : 0;
                      return ca > cb;
                  });
        return Result<SwarmNode, std::string>::ok(eligible.front());
    }
    }

    return Result<SwarmNode, std::string>::error("unknown placement strategy");
}

// ---------------------------------------------------------------------------
// Process tasks
// ---------------------------------------------------------------------------

void SwarmOrchestrator::process_tasks() {
    std::lock_guard lock(tasks_mutex_);

    for (auto& [task_id, status] : task_statuses_) {
        if (status.state == TaskState::Pending) {
            auto& task = tasks_[task_id];
            auto node_result = select_node(task);
            if (!node_result.has_value()) {
                SL_WARN("swarm: no node available for task {}: {}", task_id, node_result.error());
                continue;
            }

            auto selected = node_result.value();
            status.assigned_node_id = selected.node_id;
            status.assigned_hostname = selected.hostname;

            SL_INFO("swarm: assigning task {} to node '{}' ({})",
                    task_id, selected.hostname, selected.ip_address);

            // Execute on remote node
            auto exec_result = client_.execute_remote(
                selected.node_id, task.command, task.working_dir, task.timeout_seconds);

            if (!exec_result.has_value()) {
                status.state = TaskState::Failed;
                status.error_message = exec_result.error();
                status.completed_at = std::chrono::steady_clock::now();
                SL_WARN("swarm: task {} failed on node '{}': {}",
                        task_id, selected.hostname, exec_result.error());
            } else {
                auto result = exec_result.value();
                status.output = result.stdout_output;
                status.exit_code = result.exit_code;

                if (result.exit_code == 0) {
                    status.state = TaskState::Completed;
                    SL_INFO("swarm: task {} completed on '{}' (exit=0)", task_id, selected.hostname);
                } else {
                    status.state = TaskState::Failed;
                    status.error_message = "exit code " + std::to_string(result.exit_code);
                    SL_WARN("swarm: task {} failed on '{}' (exit={})",
                            task_id, selected.hostname, result.exit_code);
                }
                status.completed_at = std::chrono::steady_clock::now();
            }

            // Update task count for pack strategy
            node_task_count_[selected.node_id]++;
            if (task.strategy == PlacementStrategy::Spread) {
                spread_counter_++;
            }

        } else if (status.state == TaskState::Running) {
            // Check for timeout
            if (tasks_[task_id].timeout_seconds > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - status.submitted_at).count();
                if (elapsed > tasks_[task_id].timeout_seconds) {
                    SL_WARN("swarm: task {} timed out after {}s", task_id, elapsed);
                    auto r = client_.cancel_remote_task(status.assigned_node_id, task_id);
                    if (!r.has_value()) {
                        SL_WARN("swarm: cancel of timed-out task {} failed: {}", task_id, r.error());
                    }
                    status.state = TaskState::Failed;
                    status.error_message = "timeout after " + std::to_string(elapsed) + "s";
                    status.completed_at = std::chrono::steady_clock::now();
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

size_t SwarmOrchestrator::pending_count() const {
    std::lock_guard lock(tasks_mutex_);
    size_t count = 0;
    for (const auto& [_, s] : task_statuses_) {
        if (s.state == TaskState::Pending) count++;
    }
    return count;
}

size_t SwarmOrchestrator::running_count() const {
    std::lock_guard lock(tasks_mutex_);
    size_t count = 0;
    for (const auto& [_, s] : task_statuses_) {
        if (s.state == TaskState::Running) count++;
    }
    return count;
}

size_t SwarmOrchestrator::completed_count() const {
    std::lock_guard lock(tasks_mutex_);
    size_t count = 0;
    for (const auto& [_, s] : task_statuses_) {
        if (s.state == TaskState::Completed || s.state == TaskState::Failed || s.state == TaskState::Cancelled) count++;
    }
    return count;
}

} // namespace straylight
