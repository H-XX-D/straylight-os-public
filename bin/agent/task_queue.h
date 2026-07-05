// bin/agent/task_queue.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::agent {

enum class Priority : uint8_t { Low = 0, Normal = 1, High = 2, Critical = 3 };
enum class TaskType : uint8_t { Inference, Training, Preprocess, Custom };

struct Task {
    uint64_t id;
    Priority priority;
    TaskType type;
    std::string payload;
};

/// Thread-safe priority task queue.
/// Dequeues highest-priority tasks first; within the same priority level,
/// tasks are returned in FIFO order (lowest id first).
class TaskQueue {
public:
    explicit TaskQueue(size_t capacity);

    /// Push a task into the queue. Fails if at capacity.
    Result<void, std::string> push(const Task& t);

    /// Pop the highest-priority, earliest-inserted task. Fails if empty.
    Result<Task, std::string> pop();

    /// Peek at the next task without removing it. Fails if empty.
    Result<Task, std::string> peek() const;

    /// Cancel (remove) a task by id. Returns true if found and removed.
    bool cancel(uint64_t id);

    /// Current number of tasks in the queue.
    size_t size() const;

private:
    size_t capacity_;
    // Maintained as a max-heap: highest priority first, then lowest id first.
    std::vector<Task> heap_;
    mutable std::mutex mutex_;

    // Heap helpers (0-indexed)
    static size_t parent(size_t i) { return (i - 1) / 2; }
    static size_t left(size_t i) { return 2 * i + 1; }
    static size_t right(size_t i) { return 2 * i + 2; }

    /// Returns true if a should come before b in the heap (a has higher priority,
    /// or same priority but earlier insertion = lower id).
    static bool higher(const Task& a, const Task& b);

    void sift_up(size_t i);
    void sift_down(size_t i);
};

} // namespace straylight::agent
