// services/ghost/process_restore.h
// Process restoration — reconstructs a process on the target machine.
#pragma once

#include "page_server.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace straylight {

/// Progress callback for restoration.
using RestoreProgressCallback = std::function<void(uint64_t pages_restored,
                                                    uint64_t total_pages)>;

/// Process restorer: recreates a process from a ProcessImage on the target machine.
class ProcessRestore {
public:
    ProcessRestore();
    ~ProcessRestore();

    /// Restore a process from a complete image.
    /// Creates a new process with matching memory layout, registers, and file descriptors.
    /// Returns the PID of the new process.
    Result<pid_t, SLError> restore(const ProcessImage& image,
                                    const std::vector<PageInfo>& pages);

    /// Restore a process for lazy migration.
    /// Creates the process and maps regions but installs a userfaultfd handler
    /// instead of populating memory. Returns the PID of the new process.
    Result<pid_t, SLError> restore_lazy(const ProcessImage& image,
                                         const std::string& source_host,
                                         uint16_t source_port);

    /// Set a callback for progress updates during restoration.
    void set_progress_callback(RestoreProgressCallback cb) {
        progress_cb_ = std::move(cb);
    }

private:
    /// Create a new process with matching clone flags.
    Result<pid_t, SLError> create_process(const ProcessImage& image);

    /// Restore memory regions via mmap.
    Result<void, SLError> restore_memory_regions(pid_t pid,
                                                  const std::vector<MemoryRegion>& regions);

    /// Write memory pages into the new process via /proc/PID/mem.
    Result<void, SLError> restore_pages(pid_t pid,
                                         const std::vector<PageInfo>& pages);

    /// Restore register state via ptrace.
    Result<void, SLError> restore_registers(pid_t pid,
                                             const RegisterState& regs);

    /// Restore file descriptors: re-open files and dup2 to correct fd numbers.
    Result<void, SLError> restore_file_descriptors(pid_t pid,
                                                    const std::vector<FileDescriptor>& fds);

    /// Restore network connections where possible.
    Result<void, SLError> restore_network_connections(pid_t pid,
                                                       const std::vector<FileDescriptor>& fds);

    /// Set up userfaultfd for lazy page migration.
    Result<void, SLError> setup_userfaultfd(pid_t pid,
                                             const std::vector<MemoryRegion>& regions,
                                             const std::string& source_host,
                                             uint16_t source_port);

    /// Background page fault handler thread for lazy migration.
    void fault_handler_loop(int uffd, const std::string& source_host,
                            uint16_t source_port);

    /// Parse /proc/PID/maps into protection flags.
    static int perms_to_prot(const std::string& perms);

    /// Parse /proc/PID/maps into mmap flags.
    static int perms_to_flags(const std::string& perms);

    RestoreProgressCallback progress_cb_;
};

} // namespace straylight
