// bin/agent/agent_daemon.h
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include "task_queue.h"
#include "event_loop.h"

#include <string>
#include <vector>

namespace straylight {

/// Agent daemon: manages an ML/inference task queue and accepts task
/// submissions over a Unix domain socket (/run/straylight/agent.sock).
class AgentDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    agent::TaskQueue& queue() { return queue_; }

private:
    agent::TaskQueue queue_{1024};
    agent::EventLoop loop_;
    int server_fd_ = -1;
    std::string socket_path_;
    uint64_t next_task_id_ = 1;

    /// Raw file descriptors of connected clients.
    std::vector<int> client_fds_;

    /// Accept a new client connection on the server socket.
    void handle_accept(uint32_t events);

    /// Handle data arriving on a client connection.
    void handle_client(int fd, uint32_t events);

    /// Remove and close a client connection.
    void remove_client(int fd);

    /// Read a length-prefixed message from a client fd.
    Result<std::string, std::string> recv_message(int fd);

    /// Send a length-prefixed message to a client fd.
    Result<void, std::string> send_message(int fd, std::string_view msg);

    /// Parse and enqueue a task from a raw IPC message payload.
    Result<void, std::string> process_submission(const std::string& message);

    /// Dequeue and execute up to `max_batch` tasks.
    void drain_queue(size_t max_batch);
};

} // namespace straylight
