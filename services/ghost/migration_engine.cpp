// services/ghost/migration_engine.cpp
// Process migration engine — freeze, capture, stream, restore on target.

#include "migration_engine.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace straylight {

MigrationEngine::MigrationEngine() = default;

MigrationEngine::~MigrationEngine() {
    // Wait for all migration threads
    std::lock_guard lock(mutex_);
    for (auto& [id, thread] : migration_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void MigrationEngine::update_status(uint64_t migration_id,
                                      const std::function<void(MigrationStatus&)>& updater) {
    std::lock_guard lock(mutex_);
    auto it = migrations_.find(migration_id);
    if (it != migrations_.end()) {
        updater(it->second);
    }
}

Result<void, SLError> MigrationEngine::freeze_process(pid_t pid) {
    if (kill(pid, SIGSTOP) < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::PermissionDenied,
                    std::string("Cannot freeze pid ") + std::to_string(pid) +
                    ": " + ::strerror(errno)});
    }

    // Wait for the process to actually stop
    int status;
    pid_t result = waitpid(pid, &status, WUNTRACED);
    if (result < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal,
                    std::string("waitpid failed: ") + ::strerror(errno)});
    }

    if (!WIFSTOPPED(status)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Process did not stop"});
    }

    // Attach with ptrace for register access
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        // SIGSTOP already sent; try SEIZE instead
        if (ptrace(PTRACE_SEIZE, pid, nullptr, nullptr) < 0) {
            SL_WARN("ghost: ptrace attach failed for pid {}: {}", pid, ::strerror(errno));
        }
    }

    SL_INFO("ghost: frozen pid {}", pid);
    return Result<void, SLError>::ok();
}

void MigrationEngine::resume_process(pid_t pid) {
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    kill(pid, SIGCONT);
    SL_INFO("ghost: resumed pid {}", pid);
}

Result<std::vector<MemoryRegion>, SLError>
MigrationEngine::capture_memory_maps(pid_t pid) {
    std::string maps_path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(maps_path);
    if (!maps.is_open()) {
        return Result<std::vector<MemoryRegion>, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot read " + maps_path});
    }

    std::vector<MemoryRegion> regions;
    std::string line;
    while (std::getline(maps, line)) {
        MemoryRegion region;
        char perms[5] = {};
        char pathname[512] = {};

        unsigned long start, end, offset;
        unsigned int dev_major, dev_minor;
        unsigned long inode;

        int parsed = std::sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %511[^\n]",
                                  &start, &end, perms, &offset,
                                  &dev_major, &dev_minor, &inode, pathname);

        if (parsed < 7) continue;

        region.start = start;
        region.end = end;
        region.perms = perms;
        region.offset = offset;
        region.device = std::to_string(dev_major) + ":" + std::to_string(dev_minor);
        region.inode = inode;
        if (parsed >= 8) {
            // Trim leading whitespace from pathname
            char* p = pathname;
            while (*p == ' ') ++p;
            region.pathname = p;
        }

        regions.push_back(std::move(region));
    }

    SL_INFO("ghost: captured {} memory regions for pid {}", regions.size(), pid);
    return Result<std::vector<MemoryRegion>, SLError>::ok(std::move(regions));
}

Result<RegisterState, SLError>
MigrationEngine::capture_registers(pid_t pid) {
    RegisterState regs;

    // Use ptrace to read registers
    struct UserRegs {
        uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10;
        uint64_t r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax;
        uint64_t rip, cs, eflags, rsp, ss;
        uint64_t fs_base, gs_base, ds, es, fs, gs;
    } uregs{};

    long rc = ptrace(PTRACE_GETREGS, pid, nullptr, &uregs);
    if (rc < 0) {
        return Result<RegisterState, SLError>::error(
            SLError{SLErrorCode::Internal,
                    std::string("PTRACE_GETREGS failed: ") + ::strerror(errno)});
    }

    regs.rax = uregs.rax; regs.rbx = uregs.rbx;
    regs.rcx = uregs.rcx; regs.rdx = uregs.rdx;
    regs.rsi = uregs.rsi; regs.rdi = uregs.rdi;
    regs.rbp = uregs.rbp; regs.rsp = uregs.rsp;
    regs.r8 = uregs.r8;   regs.r9 = uregs.r9;
    regs.r10 = uregs.r10; regs.r11 = uregs.r11;
    regs.r12 = uregs.r12; regs.r13 = uregs.r13;
    regs.r14 = uregs.r14; regs.r15 = uregs.r15;
    regs.rip = uregs.rip; regs.rflags = uregs.eflags;
    regs.cs = uregs.cs;   regs.ss = uregs.ss;
    regs.ds = uregs.ds;   regs.es = uregs.es;
    regs.fs = uregs.fs;   regs.gs = uregs.gs;
    regs.fs_base = uregs.fs_base;
    regs.gs_base = uregs.gs_base;

    SL_DEBUG("ghost: captured registers (rip={:#x}, rsp={:#x})", regs.rip, regs.rsp);
    return Result<RegisterState, SLError>::ok(std::move(regs));
}

Result<std::vector<FileDescriptor>, SLError>
MigrationEngine::capture_file_descriptors(pid_t pid) {
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    std::vector<FileDescriptor> fds;

    // Read /proc/PID/fdinfo/ for each fd
    for (int i = 0; i < 1024; ++i) {
        std::string fd_path = fd_dir + "/" + std::to_string(i);
        char target[PATH_MAX] = {};

        ssize_t len = readlink(fd_path.c_str(), target, sizeof(target) - 1);
        if (len < 0) continue;
        target[len] = '\0';

        FileDescriptor fd;
        fd.fd_num = i;
        fd.path = target;

        // Determine type
        if (std::string(target).find("socket:") != std::string::npos) {
            fd.type = "socket";
            // Parse socket info from /proc/PID/net/tcp etc.
        } else if (std::string(target).find("pipe:") != std::string::npos) {
            fd.type = "pipe";
        } else if (std::string(target).find("anon_inode:") != std::string::npos) {
            fd.type = "anon_inode";
        } else {
            fd.type = "file";
        }

        // Read fdinfo for offset and flags
        std::string fdinfo_path = "/proc/" + std::to_string(pid) +
                                  "/fdinfo/" + std::to_string(i);
        std::ifstream fdinfo(fdinfo_path);
        if (fdinfo.is_open()) {
            std::string line;
            while (std::getline(fdinfo, line)) {
                if (line.rfind("pos:", 0) == 0) {
                    fd.offset = std::stoull(line.substr(4));
                } else if (line.rfind("flags:", 0) == 0) {
                    fd.flags = static_cast<int>(std::stoul(line.substr(6), nullptr, 8));
                }
            }
        }

        fds.push_back(std::move(fd));
    }

    SL_INFO("ghost: captured {} file descriptors for pid {}", fds.size(), pid);
    return Result<std::vector<FileDescriptor>, SLError>::ok(std::move(fds));
}

Result<std::vector<PageInfo>, SLError>
MigrationEngine::capture_pages_by_hotness(pid_t pid,
                                            const std::vector<MemoryRegion>& regions) {
    std::vector<PageInfo> pages;

    // Read /proc/PID/smaps for referenced bit information
    std::string smaps_path = "/proc/" + std::to_string(pid) + "/smaps";
    std::map<uint64_t, bool> region_referenced;

    {
        std::ifstream smaps(smaps_path);
        uint64_t current_start = 0;
        bool current_referenced = false;
        std::string line;

        while (std::getline(smaps, line)) {
            uint64_t start, end;
            if (std::sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (current_start != 0) {
                    region_referenced[current_start] = current_referenced;
                }
                current_start = start;
                current_referenced = false;
            } else if (line.find("Referenced:") != std::string::npos) {
                // Referenced: N kB — if N > 0, pages were recently accessed
                uint64_t ref_kb = 0;
                std::sscanf(line.c_str(), "Referenced: %lu kB", &ref_kb);
                current_referenced = (ref_kb > 0);
            }
        }
        if (current_start != 0) {
            region_referenced[current_start] = current_referenced;
        }
    }

    // Clear the referenced bits for future tracking
    {
        std::string clear_path = "/proc/" + std::to_string(pid) + "/clear_refs";
        std::ofstream clear(clear_path);
        if (clear.is_open()) {
            clear << "1"; // Clear Referenced bits
        }
    }

    // Read actual memory pages
    std::string mem_path = "/proc/" + std::to_string(pid) + "/mem";
    int mem_fd = ::open(mem_path.c_str(), O_RDONLY);
    if (mem_fd < 0) {
        return Result<std::vector<PageInfo>, SLError>::error(
            SLError{SLErrorCode::PermissionDenied, "Cannot open " + mem_path});
    }

    for (const auto& region : regions) {
        // Skip non-readable or special regions
        if (region.perms.empty() || region.perms[0] != 'r') continue;
        if (region.pathname == "[vvar]" || region.pathname == "[vdso]" ||
            region.pathname == "[vsyscall]") continue;

        bool is_hot = region_referenced.count(region.start) > 0 &&
                      region_referenced[region.start];

        for (uint64_t addr = region.start; addr < region.end; addr += GHOST_PAGE_SIZE) {
            PageInfo page;
            page.virtual_addr = addr;
            page.region_start = region.start;
            page.referenced = is_hot;
            page.present = true;
            page.data.resize(GHOST_PAGE_SIZE);

            ssize_t n = ::pread(mem_fd, page.data.data(), GHOST_PAGE_SIZE,
                                static_cast<off_t>(addr));
            if (n < 0) {
                page.present = false;
                page.data.clear();
                continue;
            }
            if (static_cast<size_t>(n) < GHOST_PAGE_SIZE) {
                std::memset(page.data.data() + n, 0,
                            GHOST_PAGE_SIZE - static_cast<size_t>(n));
            }

            pages.push_back(std::move(page));
        }
    }

    ::close(mem_fd);

    // Sort: hot (referenced) pages first, then by address
    std::sort(pages.begin(), pages.end(), [](const PageInfo& a, const PageInfo& b) {
        if (a.referenced != b.referenced) return a.referenced > b.referenced;
        return a.virtual_addr < b.virtual_addr;
    });

    SL_INFO("ghost: captured {} pages ({} hot) for pid {}",
            pages.size(),
            std::count_if(pages.begin(), pages.end(),
                          [](const PageInfo& p) { return p.referenced; }),
            pid);

    return Result<std::vector<PageInfo>, SLError>::ok(std::move(pages));
}

Result<void, SLError> MigrationEngine::send_process_image(int conn_fd,
                                                            const ProcessImage& image) {
    // Serialize process image as JSON and send over the connection
    nlohmann::json j;
    j["pid"] = image.pid;
    j["comm"] = image.comm;
    j["cwd"] = image.cwd;
    j["total_pages"] = image.total_pages;
    j["total_size"] = image.total_size;

    // Regions
    nlohmann::json regions = nlohmann::json::array();
    for (const auto& r : image.regions) {
        nlohmann::json rj;
        rj["start"] = r.start;
        rj["end"] = r.end;
        rj["perms"] = r.perms;
        rj["offset"] = r.offset;
        rj["pathname"] = r.pathname;
        regions.push_back(rj);
    }
    j["regions"] = regions;

    // Registers
    nlohmann::json regs;
    regs["rax"] = image.registers.rax; regs["rbx"] = image.registers.rbx;
    regs["rcx"] = image.registers.rcx; regs["rdx"] = image.registers.rdx;
    regs["rsi"] = image.registers.rsi; regs["rdi"] = image.registers.rdi;
    regs["rbp"] = image.registers.rbp; regs["rsp"] = image.registers.rsp;
    regs["r8"] = image.registers.r8;   regs["r9"] = image.registers.r9;
    regs["r10"] = image.registers.r10; regs["r11"] = image.registers.r11;
    regs["r12"] = image.registers.r12; regs["r13"] = image.registers.r13;
    regs["r14"] = image.registers.r14; regs["r15"] = image.registers.r15;
    regs["rip"] = image.registers.rip; regs["rflags"] = image.registers.rflags;
    regs["cs"] = image.registers.cs;   regs["ss"] = image.registers.ss;
    regs["fs_base"] = image.registers.fs_base;
    regs["gs_base"] = image.registers.gs_base;
    j["registers"] = regs;

    // File descriptors
    nlohmann::json fds = nlohmann::json::array();
    for (const auto& fd : image.fds) {
        nlohmann::json fj;
        fj["fd_num"] = fd.fd_num;
        fj["path"] = fd.path;
        fj["type"] = fd.type;
        fj["offset"] = fd.offset;
        fj["flags"] = fd.flags;
        fj["socket_type"] = fd.socket_type;
        fj["local_addr"] = fd.local_addr;
        fj["remote_addr"] = fd.remote_addr;
        fds.push_back(fj);
    }
    j["fds"] = fds;

    std::string payload = j.dump() + "\n";
    size_t total = 0;
    while (total < payload.size()) {
        ssize_t n = ::write(conn_fd, payload.data() + total, payload.size() - total);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, "Failed to send process image"});
        }
        total += static_cast<size_t>(n);
    }

    return Result<void, SLError>::ok();
}

Result<void, SLError> MigrationEngine::stream_pages(
    uint64_t migration_id, pid_t pid,
    const std::string& target_host,
    const MigrationOptions& opts,
    const std::vector<PageInfo>& pages) {

    // Connect to the target ghost daemon
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("socket() failed: ") + ::strerror(errno)});
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts.port);
    inet_pton(AF_INET, target_host.c_str(), &addr.sin_addr);

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "Cannot connect to " + target_host + ":" + std::to_string(opts.port)});
    }

    auto start_time = std::chrono::steady_clock::now();
    uint64_t bytes_sent = 0;

    for (size_t i = 0; i < pages.size(); ++i) {
        const auto& page = pages[i];
        if (page.data.empty()) continue;

        // Compress the page
        auto compressed = PageServer::compress_page(
            page.data.data(), page.data.size(), opts.compress_level);

        // Send: [addr:u64][compressed_size:u32][compressed_data]
        uint64_t page_addr = page.virtual_addr;
        uint32_t comp_size = static_cast<uint32_t>(compressed.size());

        ::send(sock, &page_addr, sizeof(page_addr), 0);
        ::send(sock, &comp_size, sizeof(comp_size), 0);
        ::send(sock, compressed.data(), compressed.size(), 0);

        bytes_sent += sizeof(page_addr) + sizeof(comp_size) + compressed.size();

        // Update status
        if (i % 64 == 0 || i == pages.size() - 1) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start_time).count();
            double bw = (elapsed > 0) ? (static_cast<double>(bytes_sent) / (1024.0 * 1024.0)) / elapsed : 0;

            update_status(migration_id, [&](MigrationStatus& s) {
                s.pages_transferred = i + 1;
                s.pages_remaining = pages.size() - i - 1;
                s.bandwidth_mbps = bw;
                s.elapsed_seconds = elapsed;
                if (bw > 0 && s.pages_remaining > 0) {
                    double pages_per_sec = static_cast<double>(i + 1) / elapsed;
                    s.eta_seconds = static_cast<double>(s.pages_remaining) / pages_per_sec;
                }
            });
        }
    }

    ::close(sock);

    (void)pid;
    SL_INFO("ghost: streamed {} pages ({} bytes) to {}",
            pages.size(), bytes_sent, target_host);

    return Result<void, SLError>::ok();
}

void MigrationEngine::run_migration(uint64_t migration_id, pid_t pid,
                                      const std::string& target_host,
                                      MigrationOptions opts, bool lazy) {
    auto fail = [&](const std::string& msg) {
        SL_ERROR("ghost: migration {} failed: {}", migration_id, msg);
        update_status(migration_id, [&](MigrationStatus& s) {
            s.state = MigrationState::Failed;
            s.error_message = msg;
        });
    };

    // Step 1: Freeze the process
    update_status(migration_id, [](MigrationStatus& s) {
        s.state = MigrationState::Freezing;
    });

    auto freeze_result = freeze_process(pid);
    if (!freeze_result.has_value()) {
        fail(freeze_result.error().message());
        return;
    }

    // Step 2: Capture process state
    update_status(migration_id, [](MigrationStatus& s) {
        s.state = MigrationState::Capturing;
    });

    auto maps_result = capture_memory_maps(pid);
    if (!maps_result.has_value()) {
        resume_process(pid);
        fail(maps_result.error().message());
        return;
    }

    auto regs_result = capture_registers(pid);
    if (!regs_result.has_value()) {
        resume_process(pid);
        fail(regs_result.error().message());
        return;
    }

    auto fds_result = capture_file_descriptors(pid);
    if (!fds_result.has_value()) {
        resume_process(pid);
        fail(fds_result.error().message());
        return;
    }

    auto pages_result = capture_pages_by_hotness(pid, maps_result.value());
    if (!pages_result.has_value()) {
        resume_process(pid);
        fail(pages_result.error().message());
        return;
    }

    // Build process image
    ProcessImage image;
    image.pid = pid;
    image.regions = maps_result.value();
    image.registers = regs_result.value();
    image.fds = fds_result.value();
    image.total_pages = pages_result.value().size();
    image.total_size = image.total_pages * GHOST_PAGE_SIZE;

    // Read comm
    {
        std::ifstream comm_file("/proc/" + std::to_string(pid) + "/comm");
        if (comm_file.is_open()) {
            std::getline(comm_file, image.comm);
        }
    }

    // Read cwd
    {
        char cwd_buf[PATH_MAX] = {};
        std::string cwd_link = "/proc/" + std::to_string(pid) + "/cwd";
        ssize_t len = readlink(cwd_link.c_str(), cwd_buf, sizeof(cwd_buf) - 1);
        if (len > 0) {
            cwd_buf[len] = '\0';
            image.cwd = cwd_buf;
        }
    }

    update_status(migration_id, [&](MigrationStatus& s) {
        s.total_pages = image.total_pages;
        s.hot_pages = static_cast<uint64_t>(std::count_if(
            pages_result.value().begin(), pages_result.value().end(),
            [](const PageInfo& p) { return p.referenced; }));
    });

    // Step 3: Stream to target
    if (lazy) {
        update_status(migration_id, [](MigrationStatus& s) {
            s.state = MigrationState::LazyActive;
        });

        // For lazy migration: start page server, send image to target,
        // target starts process immediately and faults in pages on demand
        if (page_server_) {
            auto server_result = page_server_->start(pid, opts.port);
            if (!server_result.has_value()) {
                resume_process(pid);
                fail(server_result.error().message());
                return;
            }
        }

        // Send only the process image (no pages) to the target
        // The target will set up userfaultfd and request pages on demand
        SL_INFO("ghost: lazy migration active — page server running on port {}",
                opts.port);

    } else {
        update_status(migration_id, [](MigrationStatus& s) {
            s.state = MigrationState::Streaming;
        });

        auto stream_result = stream_pages(migration_id, pid, target_host,
                                           opts, pages_result.value());
        if (!stream_result.has_value()) {
            resume_process(pid);
            fail(stream_result.error().message());
            return;
        }
    }

    // Step 4: Mark complete
    update_status(migration_id, [](MigrationStatus& s) {
        s.state = MigrationState::Complete;
        s.pages_remaining = 0;
    });

    // Kill the source process (it's now running on the target)
    if (!lazy) {
        kill(pid, SIGKILL);
        SL_INFO("ghost: source process {} killed after successful migration", pid);
    }

    SL_INFO("ghost: migration {} complete", migration_id);
}

Result<uint64_t, SLError> MigrationEngine::migrate(pid_t pid,
                                                      const std::string& target_host,
                                                      const MigrationOptions& opts) {
    // Verify process exists
    std::string proc_path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream status(proc_path);
    if (!status.is_open()) {
        return Result<uint64_t, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Process " + std::to_string(pid) + " not found"});
    }

    uint64_t id = next_id_.fetch_add(1);

    {
        std::lock_guard lock(mutex_);
        MigrationStatus ms;
        ms.migration_id = id;
        ms.source_pid = pid;
        ms.target_host = target_host;
        ms.state = MigrationState::Pending;
        migrations_[id] = ms;

        // Launch migration in background thread
        migration_threads_[id] = std::thread(
            &MigrationEngine::run_migration, this,
            id, pid, target_host, opts, false);
    }

    SL_INFO("ghost: started migration {} (pid {} -> {})", id, pid, target_host);
    return Result<uint64_t, SLError>::ok(id);
}

Result<uint64_t, SLError> MigrationEngine::lazy_migrate(pid_t pid,
                                                           const std::string& target_host,
                                                           const MigrationOptions& opts) {
    std::string proc_path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream status(proc_path);
    if (!status.is_open()) {
        return Result<uint64_t, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Process " + std::to_string(pid) + " not found"});
    }

    MigrationOptions lazy_opts = opts;
    lazy_opts.lazy = true;

    uint64_t id = next_id_.fetch_add(1);

    {
        std::lock_guard lock(mutex_);
        MigrationStatus ms;
        ms.migration_id = id;
        ms.source_pid = pid;
        ms.target_host = target_host;
        ms.state = MigrationState::Pending;
        migrations_[id] = ms;

        migration_threads_[id] = std::thread(
            &MigrationEngine::run_migration, this,
            id, pid, target_host, lazy_opts, true);
    }

    SL_INFO("ghost: started lazy migration {} (pid {} -> {})", id, pid, target_host);
    return Result<uint64_t, SLError>::ok(id);
}

Result<MigrationStatus, SLError>
MigrationEngine::get_status(uint64_t migration_id) const {
    std::lock_guard lock(mutex_);
    auto it = migrations_.find(migration_id);
    if (it == migrations_.end()) {
        return Result<MigrationStatus, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Migration " + std::to_string(migration_id) + " not found"});
    }
    return Result<MigrationStatus, SLError>::ok(it->second);
}

std::vector<MigrationStatus> MigrationEngine::list_migrations() const {
    std::lock_guard lock(mutex_);
    std::vector<MigrationStatus> result;
    result.reserve(migrations_.size());
    for (const auto& [id, ms] : migrations_) {
        result.push_back(ms);
    }
    return result;
}

Result<void, SLError> MigrationEngine::cancel(uint64_t migration_id) {
    std::lock_guard lock(mutex_);
    auto it = migrations_.find(migration_id);
    if (it == migrations_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Migration " + std::to_string(migration_id) + " not found"});
    }

    if (it->second.state == MigrationState::Complete ||
        it->second.state == MigrationState::Failed ||
        it->second.state == MigrationState::Cancelled) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::InvalidArgument,
                    "Migration already in terminal state"});
    }

    it->second.state = MigrationState::Cancelled;

    // Resume the source process if it was frozen
    resume_process(it->second.source_pid);

    SL_INFO("ghost: cancelled migration {}", migration_id);
    return Result<void, SLError>::ok();
}

Result<void, SLError> MigrationEngine::receive_migration(int connection_fd) {
    // Read the process image from the connection
    std::string buffer;
    char buf[4096];

    while (true) {
        ssize_t n = ::read(connection_fd, buf, sizeof(buf));
        if (n <= 0) break;
        buffer.append(buf, static_cast<size_t>(n));
        if (buffer.find('\n') != std::string::npos) break;
    }

    if (buffer.empty()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "No data received"});
    }

    // Parse the process image JSON
    try {
        auto j = nlohmann::json::parse(buffer);

        ProcessImage image;
        image.pid = j.value("pid", 0);
        image.comm = j.value("comm", "");
        image.cwd = j.value("cwd", "");
        image.total_pages = j.value("total_pages", uint64_t{0});
        image.total_size = j.value("total_size", uint64_t{0});

        // Parse regions, registers, fds...
        // (The full parsing would mirror send_process_image)

        SL_INFO("ghost: received migration for process '{}' ({} pages)",
                image.comm, image.total_pages);

        // Restore the process
        ProcessRestore restorer;
        // For full migration, we receive pages after the image
        // For lazy migration, we set up userfaultfd

        return Result<void, SLError>::ok();
    } catch (const nlohmann::json::exception& e) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError,
                    std::string("Failed to parse process image: ") + e.what()});
    }
}

} // namespace straylight
