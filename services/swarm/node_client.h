// services/swarm/node_client.h
#pragma once

#include <straylight/result.h>
#include "discovery.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace straylight {

/// Result of a remote command execution.
struct RemoteExecResult {
    std::string stdout_output;
    std::string stderr_output;
    int exit_code = -1;
};

/// Communicates with peer StrayLight nodes over the straylight-remote protocol.
///
/// Protocol: TLS 1.3 on port 7700. Wire format is a simple framed text protocol:
///   REQUEST\n
///   type=exec|cancel|status\n
///   task_id=<id>\n
///   command=<cmd>\n
///   working_dir=<dir>\n
///   timeout=<seconds>\n
///   \n
///
///   RESPONSE\n
///   status=ok|error\n
///   exit_code=<n>\n
///   output=<base64-encoded stdout>\n
///   error=<message>\n
///   \n
///
/// On macOS (no peer nodes), all operations log the intended action and return
/// a simulated response for development/testing.
class NodeClient {
public:
    NodeClient();
    ~NodeClient();

    /// Initialize with reference to discovery (for node address lookup).
    void init(NodeDiscovery& discovery);

    /// Execute a command on a remote node. Blocks until completion or timeout.
    Result<RemoteExecResult, std::string> execute_remote(
        const std::string& node_id,
        const std::string& command,
        const std::string& working_dir = "/tmp",
        int timeout_seconds = 0);

    /// Cancel a running task on a remote node.
    Result<void, std::string> cancel_remote_task(
        const std::string& node_id,
        const std::string& task_id);

    /// Check connectivity to a node (ping-like).
    Result<double, std::string> ping_node(const std::string& node_id);

    /// Get the remote protocol version from a node.
    Result<std::string, std::string> remote_version(const std::string& node_id);

private:
    /// Establish a TLS connection to a node. Returns the socket fd or error.
    Result<int, std::string> connect_to_node(const SwarmNode& node, int timeout_ms = 5000);

    /// Send a request and receive a response over an established connection.
    Result<std::string, std::string> send_request(int fd, const std::string& request);

    /// Close a connection.
    void close_connection(int fd);

    /// Build a request string for the wire protocol.
    std::string build_exec_request(const std::string& task_id,
                                    const std::string& command,
                                    const std::string& working_dir,
                                    int timeout_seconds);

    std::string build_cancel_request(const std::string& task_id);
    std::string build_ping_request();
    std::string build_version_request();

    /// Parse a response string into key-value pairs.
    std::unordered_map<std::string, std::string> parse_response(const std::string& data);

    NodeDiscovery* discovery_ = nullptr;
    mutable std::mutex mutex_;

    // Connection pool (node_id -> fd). In production this would be
    // a proper pool with keepalive and reconnection logic.
    std::unordered_map<std::string, int> conn_pool_;
};

} // namespace straylight
