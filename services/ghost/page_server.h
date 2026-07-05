// services/ghost/page_server.h
// Page server — serves memory pages to remote ghost daemons on demand.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight {

static constexpr uint64_t GHOST_PAGE_SIZE = 4096;

/// A memory region from /proc/PID/maps.
struct MemoryRegion {
    uint64_t start{0};
    uint64_t end{0};
    std::string perms;        // e.g., "rw-p"
    uint64_t offset{0};
    std::string device;
    uint64_t inode{0};
    std::string pathname;

    [[nodiscard]] uint64_t size() const { return end - start; }
    [[nodiscard]] uint64_t page_count() const { return size() / GHOST_PAGE_SIZE; }
    [[nodiscard]] bool is_writable() const { return perms.size() >= 2 && perms[1] == 'w'; }
    [[nodiscard]] bool is_private() const { return perms.size() >= 4 && perms[3] == 'p'; }
    [[nodiscard]] bool is_anonymous() const { return pathname.empty() || pathname == "[heap]" || pathname == "[stack]"; }
};

/// Info about a single page.
struct PageInfo {
    uint64_t virtual_addr{0};
    uint64_t region_start{0};
    bool referenced{false};     // Recently accessed (hot page)
    bool dirty{false};          // Modified since last scan
    bool present{false};        // Currently in physical memory
    std::vector<uint8_t> data;  // Page contents (GHOST_PAGE_SIZE bytes)
};

/// Register state for a process (x86_64).
struct RegisterState {
    uint64_t rax{0}, rbx{0}, rcx{0}, rdx{0};
    uint64_t rsi{0}, rdi{0}, rbp{0}, rsp{0};
    uint64_t r8{0}, r9{0}, r10{0}, r11{0};
    uint64_t r12{0}, r13{0}, r14{0}, r15{0};
    uint64_t rip{0}, rflags{0};
    uint64_t cs{0}, ss{0}, ds{0}, es{0}, fs{0}, gs{0};
    uint64_t fs_base{0}, gs_base{0};
    // FPU/SSE state
    std::vector<uint8_t> fpu_state;
};

/// Open file descriptor info.
struct FileDescriptor {
    int fd_num{-1};
    std::string path;           // Target of /proc/PID/fd/N symlink
    std::string type;           // "file", "socket", "pipe", "anon_inode", etc.
    uint64_t offset{0};         // File offset (for regular files)
    int flags{0};               // Open flags (O_RDONLY, O_WRONLY, etc.)
    // Socket-specific
    std::string socket_type;    // "tcp", "udp", "unix"
    std::string local_addr;
    std::string remote_addr;
};

/// Complete process image for migration.
struct ProcessImage {
    pid_t pid{0};
    std::string comm;           // Process name
    std::vector<MemoryRegion> regions;
    RegisterState registers;
    std::vector<FileDescriptor> fds;
    std::vector<std::string> environ;
    std::string cwd;
    uint64_t total_pages{0};
    uint64_t total_size{0};
};

/// Page request from the remote side.
struct PageRequest {
    uint64_t virtual_addr{0};
    uint64_t count{1};          // Number of consecutive pages
};

/// Page server: serves memory pages to remote ghost daemons.
class PageServer {
public:
    PageServer();
    ~PageServer();

    /// Start serving pages for a process on the given port.
    Result<void, SLError> start(pid_t pid, uint16_t port);

    /// Stop the page server.
    void stop();

    /// Check if the server is running.
    [[nodiscard]] bool is_running() const { return running_.load(); }

    /// Get the number of pages served so far.
    [[nodiscard]] uint64_t pages_served() const { return pages_served_.load(); }

    /// Get the total bytes transferred.
    [[nodiscard]] uint64_t bytes_sent() const { return bytes_sent_.load(); }

    /// Read a page from the source process.
    Result<std::vector<uint8_t>, SLError> read_page(pid_t pid, uint64_t addr);

    /// Read multiple consecutive pages.
    Result<std::vector<uint8_t>, SLError> read_pages(pid_t pid, uint64_t addr,
                                                      uint64_t count);

    /// Compress a page with zstd.
    static std::vector<uint8_t> compress_page(const uint8_t* data, size_t size,
                                               int level = 3);

    /// Decompress a zstd-compressed page.
    static Result<std::vector<uint8_t>, std::string> decompress_page(
        const uint8_t* data, size_t size);

    /// Predict which pages will be needed next based on access pattern.
    std::vector<uint64_t> predict_prefetch(uint64_t current_addr,
                                            int lookahead = 16) const;

private:
    /// Accept loop: serves incoming page requests.
    void serve_loop();

    /// Handle a single client connection.
    void handle_client(int client_fd);

    std::atomic<bool> running_{false};
    std::thread server_thread_;
    int server_fd_{-1};
    pid_t source_pid_{0};

    std::atomic<uint64_t> pages_served_{0};
    std::atomic<uint64_t> bytes_sent_{0};

    // Access pattern tracking for prefetch prediction
    mutable std::mutex pattern_mutex_;
    std::vector<uint64_t> access_history_;
    static constexpr size_t MAX_HISTORY = 1024;
};

} // namespace straylight
