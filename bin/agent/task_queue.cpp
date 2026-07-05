// bin/agent/task_queue.cpp
#include "task_queue.h"

#include <algorithm>
#include <utility>

namespace straylight::agent {

TaskQueue::TaskQueue(size_t capacity) : capacity_(capacity) {
    heap_.reserve(capacity);
}

bool TaskQueue::higher(const Task& a, const Task& b) {
    if (static_cast<uint8_t>(a.priority) != static_cast<uint8_t>(b.priority)) {
        return static_cast<uint8_t>(a.priority) > static_cast<uint8_t>(b.priority);
    }
    // Same priority: earlier insertion (lower id) wins.
    return a.id < b.id;
}

void TaskQueue::sift_up(size_t i) {
    while (i > 0) {
        size_t p = parent(i);
        if (higher(heap_[i], heap_[p])) {
            std::swap(heap_[i], heap_[p]);
            i = p;
        } else {
            break;
        }
    }
}

void TaskQueue::sift_down(size_t i) {
    size_t n = heap_.size();
    while (true) {
        size_t best = i;
        size_t l = left(i);
        size_t r = right(i);

        if (l < n && higher(heap_[l], heap_[best])) {
            best = l;
        }
        if (r < n && higher(heap_[r], heap_[best])) {
            best = r;
        }
        if (best == i) break;
        std::swap(heap_[i], heap_[best]);
        i = best;
    }
}

Result<void, std::string> TaskQueue::push(const Task& t) {
    std::lock_guard lock(mutex_);
    if (heap_.size() >= capacity_) {
        return Result<void, std::string>::error("task queue at capacity");
    }
    heap_.push_back(t);
    sift_up(heap_.size() - 1);
    return Result<void, std::string>::ok();
}

Result<Task, std::string> TaskQueue::pop() {
    std::lock_guard lock(mutex_);
    if (heap_.empty()) {
        return Result<Task, std::string>::error("task queue is empty");
    }
    Task top = std::move(heap_[0]);
    heap_[0] = std::move(heap_.back());
    heap_.pop_back();
    if (!heap_.empty()) {
        sift_down(0);
    }
    return Result<Task, std::string>::ok(std::move(top));
}

Result<Task, std::string> TaskQueue::peek() const {
    std::lock_guard lock(mutex_);
    if (heap_.empty()) {
        return Result<Task, std::string>::error("task queue is empty");
    }
    return Result<Task, std::string>::ok(heap_[0]);
}

bool TaskQueue::cancel(uint64_t id) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(heap_.begin(), heap_.end(),
                           [id](const Task& t) { return t.id == id; });
    if (it == heap_.end()) {
        return false;
    }
    size_t idx = static_cast<size_t>(std::distance(heap_.begin(), it));

    // Replace with last element and remove last
    if (idx == heap_.size() - 1) {
        heap_.pop_back();
        return true;
    }

    heap_[idx] = std::move(heap_.back());
    heap_.pop_back();

    // Restore heap property: the replacement element might need to go up or down.
    if (idx > 0 && higher(heap_[idx], heap_[parent(idx)])) {
        sift_up(idx);
    } else {
        sift_down(idx);
    }

    return true;
}

size_t TaskQueue::size() const {
    std::lock_guard lock(mutex_);
    return heap_.size();
}

} // namespace straylight::agent
