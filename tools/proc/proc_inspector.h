// tools/proc/proc_inspector.h
// Process inspector for StrayLight OS — deep /proc/PID reader.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace straylight {

/// Basic process info from /proc/PID/status.
struct ProcInfo {
    int         pid = 0;
    int         ppid = 0;
    std::string name;
    std::string state;          // "running", "sleeping", "stopped", "zombie"
    std::string state_char;
    int         threads = 0;
    int         uid = 0;
    int         gid = 0;
    std::string user;
    uint64_t    vm_size = 0;    // virtual memory
    uint64_t    vm_rss = 0;     // resident set size
    uint64_t    vm_shared = 0;
    std::string cmdline;
    std::string exe;
    std::string cwd;
    std::string cgroup;
    int         nice = 0;
    uint64_t    start_time = 0;
    std::string start_time_str;
};

/// Memory map region from /proc/PID/maps.
struct MemRegion {
    std::string address;
    std::string perms;          // "rwxp"
    uint64_t    offset = 0;
    std::string device;
    uint64_t    inode = 0;
    std::string pathname;
    uint64_t    size = 0;
    uint64_t    rss = 0;
    uint64_t    pss = 0;        // from smaps
};

/// Open file descriptor.
struct OpenFile {
    int         fd = 0;
    std::string type;           // "regular", "socket", "pipe", "anon_inode", "eventfd"
    std::string path;
    std::string mode;           // "r", "w", "rw"
};

/// IO stats from /proc/PID/io.
struct ProcIO {
    uint64_t    read_bytes = 0;
    uint64_t    write_bytes = 0;
    uint64_t    read_syscalls = 0;
    uint64_t    write_syscalls = 0;
    uint64_t    cancelled_write_bytes = 0;
};

/// Network connection for a process.
struct ProcNetConn {
    std::string protocol;       // "tcp", "tcp6", "udp"
    std::string local_addr;
    uint16_t    local_port = 0;
    std::string remote_addr;
    uint16_t    remote_port = 0;
    std::string state;
};

/// Resource limit from /proc/PID/limits.
struct ProcLimit {
    std::string name;
    std::string soft;
    std::string hard;
    std::string units;
};

/// Process tree node.
struct ProcTreeNode {
    int         pid = 0;
    std::string name;
    std::string state_char;
    std::vector<ProcTreeNode> children;
};

class ProcInspector {
public:
    ProcInspector();
    ~ProcInspector();

    /// Get detailed process info.
    Result<ProcInfo, std::string> info(int pid) const;

    /// Get process tree.
    Result<ProcTreeNode, std::string> tree() const;

    /// List open files for a process.
    Result<std::vector<OpenFile>, std::string> files(int pid) const;

    /// Get memory map info.
    Result<std::vector<MemRegion>, std::string> mem(int pid) const;

    /// Get network connections for a process.
    Result<std::vector<ProcNetConn>, std::string> net(int pid) const;

    /// Get environment variables.
    Result<std::map<std::string, std::string>, std::string> env(int pid) const;

    /// Send a signal to a process.
    Result<void, std::string> signal(int pid, int sig) const;

    /// Adjust nice/ionice for a process.
    Result<void, std::string> renice(int pid, int nice_level) const;

    /// Find processes by name.
    Result<std::vector<ProcInfo>, std::string> find(const std::string& name) const;

    /// Get IO stats.
    Result<ProcIO, std::string> io(int pid) const;

    /// Get resource limits.
    Result<std::vector<ProcLimit>, std::string> limits(int pid) const;

private:
    Result<std::string, std::string> read_proc_file(int pid, const std::string& file) const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    std::string resolve_state(char state_char) const;
    std::string hex_to_ip(const std::string& hex_ip) const;
    uint16_t hex_to_port(const std::string& hex_port) const;
    ProcTreeNode build_tree(int pid, const std::map<int, std::vector<int>>& children_map,
                             const std::map<int, std::string>& names,
                             const std::map<int, std::string>& states) const;
};

} // namespace straylight
