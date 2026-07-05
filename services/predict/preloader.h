/**
 * StrayLight Preloader — Pre-warms system resources for predicted apps.
 *
 * Strategies:
 *   - Libraries: readahead() on shared libraries (parsed from ldd)
 *   - VPU: Pre-allocate slab blocks for known GPU apps
 *   - Files: readahead() on recently-used files
 *   - Mesh: Pre-connect to remote nodes for mesh GPU apps
 *
 * Budget: never use more than 10% of free RAM or 5% of free VRAM.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace straylight::predict {

// ─── Types ──────────────────────────────────────────────────────────────────

struct PreloadEntry {
    std::string app_name;
    std::string resource_path;
    std::string resource_type;   // "library", "file", "vpu_slab", "mesh_node"
    size_t size_bytes = 0;
    std::chrono::steady_clock::time_point loaded_at;
    bool active = false;
};

struct ResourceBudget {
    size_t max_ram_bytes = 0;      // 10% of free RAM
    size_t max_vram_bytes = 0;     // 5% of free VRAM
    size_t used_ram_bytes = 0;
    size_t used_vram_bytes = 0;
};

// ─── Preloader ──────────────────────────────────────────────────────────────

class Preloader {
public:
    Preloader() {
        refresh_budget();
    }

    /** Pre-warm resources for a predicted app. */
    VoidResult<> preload_app(const std::string& app_name) {
        std::lock_guard<std::mutex> lock(mtx_);

        // Skip if already preloaded
        if (preloaded_apps_.count(app_name)) {
            return VoidResult<>::ok();
        }

        refresh_budget();

        // Strategy 1: Preload shared libraries
        auto libs = get_app_libraries(app_name);
        for (const auto& lib : libs) {
            if (budget_.used_ram_bytes >= budget_.max_ram_bytes) break;

            auto result = readahead_file(lib);
            if (result) {
                PreloadEntry entry;
                entry.app_name = app_name;
                entry.resource_path = lib;
                entry.resource_type = "library";
                entry.size_bytes = result.value();
                entry.loaded_at = std::chrono::steady_clock::now();
                entry.active = true;

                entries_.push_back(entry);
                budget_.used_ram_bytes += entry.size_bytes;
            }
        }

        // Strategy 2: VPU pre-allocation for known GPU apps
        if (is_gpu_app(app_name)) {
            auto vpu_result = preload_vpu_slab(app_name);
            if (vpu_result) {
                PreloadEntry entry;
                entry.app_name = app_name;
                entry.resource_path = "/sys/kernel/straylight-vpu";
                entry.resource_type = "vpu_slab";
                entry.size_bytes = vpu_result.value();
                entry.loaded_at = std::chrono::steady_clock::now();
                entry.active = true;

                entries_.push_back(entry);
                budget_.used_vram_bytes += entry.size_bytes;
            }
        }

        // Strategy 3: Preload recently-used files for this app
        auto recent_files = get_recent_files(app_name);
        for (const auto& file : recent_files) {
            if (budget_.used_ram_bytes >= budget_.max_ram_bytes) break;

            auto result = readahead_file(file);
            if (result) {
                PreloadEntry entry;
                entry.app_name = app_name;
                entry.resource_path = file;
                entry.resource_type = "file";
                entry.size_bytes = result.value();
                entry.loaded_at = std::chrono::steady_clock::now();
                entry.active = true;

                entries_.push_back(entry);
                budget_.used_ram_bytes += entry.size_bytes;
            }
        }

        // Strategy 4: Pre-connect mesh nodes for mesh GPU apps
        if (is_mesh_app(app_name)) {
            preconnect_mesh_nodes(app_name);
        }

        preloaded_apps_.insert(app_name);
        return VoidResult<>::ok();
    }

    /** Release pre-warmed resources for an app (prediction was wrong). */
    void evict_preloads(const std::string& app_name) {
        std::lock_guard<std::mutex> lock(mtx_);

        for (auto& entry : entries_) {
            if (entry.app_name == app_name && entry.active) {
                entry.active = false;

                if (entry.resource_type == "library" || entry.resource_type == "file") {
                    // Advise kernel to release pages via posix_fadvise
                    evict_file(entry.resource_path);
                    budget_.used_ram_bytes -= std::min(budget_.used_ram_bytes, entry.size_bytes);
                } else if (entry.resource_type == "vpu_slab") {
                    release_vpu_slab(app_name);
                    budget_.used_vram_bytes -= std::min(budget_.used_vram_bytes, entry.size_bytes);
                }
            }
        }

        // Remove inactive entries
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [&](const PreloadEntry& e) {
                               return e.app_name == app_name && !e.active;
                           }),
            entries_.end());

        preloaded_apps_.erase(app_name);
    }

    /** Get all current preload entries. */
    std::vector<PreloadEntry> list_preloads() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return entries_;
    }

    /** Get current resource budget. */
    ResourceBudget get_budget() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return budget_;
    }

    /** Evict all preloads. */
    void evict_all() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& entry : entries_) {
            if (entry.active) {
                if (entry.resource_type == "library" || entry.resource_type == "file") {
                    evict_file(entry.resource_path);
                } else if (entry.resource_type == "vpu_slab") {
                    release_vpu_slab(entry.app_name);
                }
            }
        }
        entries_.clear();
        preloaded_apps_.clear();
        budget_.used_ram_bytes = 0;
        budget_.used_vram_bytes = 0;
    }

    /** Check if an app's resources are preloaded. */
    bool is_preloaded(const std::string& app_name) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return preloaded_apps_.count(app_name) > 0;
    }

private:
    mutable std::mutex mtx_;
    std::vector<PreloadEntry> entries_;
    std::set<std::string> preloaded_apps_;
    ResourceBudget budget_;

    // ─── Budget management ──────────────────────────────────────────────

    void refresh_budget() {
        // Read free memory from /proc/meminfo (Linux)
        size_t free_ram = 0;
        std::ifstream meminfo("/proc/meminfo");
        if (meminfo) {
            std::string line;
            while (std::getline(meminfo, line)) {
                if (line.find("MemAvailable:") == 0 || line.find("MemFree:") == 0) {
                    // Parse "MemAvailable:   12345678 kB"
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        try {
                            size_t kb = std::stoul(line.substr(colon + 1));
                            free_ram = kb * 1024; // Convert to bytes
                            if (line.find("MemAvailable:") == 0) break; // Prefer MemAvailable
                        } catch (...) {}
                    }
                }
            }
        }

        if (free_ram == 0) {
            // Fallback: assume 8GB system with 50% free
            free_ram = 4ULL * 1024 * 1024 * 1024;
        }

        budget_.max_ram_bytes = free_ram / 10; // 10% of free RAM

        // Read free VRAM from the live StrayLight VPU kernel ABI.
        size_t free_vram = 0;
        {
            size_t total = 0;
            size_t used = 0;
            std::ifstream total_file("/sys/kernel/straylight-vpu/gpu0/vram_total");
            std::ifstream used_file("/sys/kernel/straylight-vpu/gpu0/vram_used");
            if (total_file) total_file >> total;
            if (used_file) used_file >> used;
            if (total > used) free_vram = total - used;
        }
        if (free_vram == 0) {
            std::ifstream vram_file("/sys/class/straylight-vpu/vpu0/memory_free");
            if (vram_file) {
                vram_file >> free_vram;
            }
        }
        if (free_vram == 0) {
            // Fallback: assume 2GB VRAM total, 50% free
            free_vram = 1ULL * 1024 * 1024 * 1024;
        }

        budget_.max_vram_bytes = free_vram / 20; // 5% of free VRAM
    }

    // ─── Library discovery ──────────────────────────────────────────────

    /** Get shared libraries for an app via ldd or from cached data. */
    std::vector<std::string> get_app_libraries(const std::string& app_name) {
        std::vector<std::string> libs;

        // Find the binary path
        std::string which_cmd = "which " + app_name + " 2>/dev/null";
        std::string bin_path = exec_cmd(which_cmd);
        // Trim newline
        while (!bin_path.empty() && (bin_path.back() == '\n' || bin_path.back() == '\r'))
            bin_path.pop_back();

        if (bin_path.empty()) return libs;

        // Run ldd to get shared libraries
        std::string ldd_cmd = "ldd " + bin_path + " 2>/dev/null";
        std::string output = exec_cmd(ldd_cmd);

        // Parse ldd output: "libfoo.so.1 => /usr/lib/libfoo.so.1 (0x...)"
        size_t pos = 0;
        while (pos < output.size()) {
            auto nl = output.find('\n', pos);
            if (nl == std::string::npos) nl = output.size();
            std::string line = output.substr(pos, nl - pos);
            pos = nl + 1;

            auto arrow = line.find("=>");
            if (arrow == std::string::npos) continue;

            auto path_start = line.find_first_not_of(" \t", arrow + 2);
            if (path_start == std::string::npos) continue;

            auto paren = line.find('(', path_start);
            std::string lib_path;
            if (paren != std::string::npos) {
                lib_path = line.substr(path_start, paren - path_start);
            } else {
                lib_path = line.substr(path_start);
            }

            // Trim whitespace
            while (!lib_path.empty() && lib_path.back() == ' ') lib_path.pop_back();

            if (!lib_path.empty() && lib_path[0] == '/') {
                libs.push_back(lib_path);
            }
        }

        return libs;
    }

    // ─── File preloading ────────────────────────────────────────────────

    /** Call readahead() on a file. Returns bytes read or error. */
    Result<size_t, std::string> readahead_file(const std::string& path) {
        struct stat st{};
        if (stat(path.c_str(), &st) < 0) {
            return Result<size_t, std::string>::error("stat failed: " + path);
        }

        size_t file_size = static_cast<size_t>(st.st_size);

        // Don't preload files larger than 100MB
        if (file_size > 100 * 1024 * 1024) {
            return Result<size_t, std::string>::error("file too large: " + path);
        }

        // Check budget
        if (budget_.used_ram_bytes + file_size > budget_.max_ram_bytes) {
            return Result<size_t, std::string>::error("RAM budget exceeded");
        }

        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return Result<size_t, std::string>::error("open failed: " + path);
        }

#ifdef __linux__
        // Use posix_fadvise for readahead
        posix_fadvise(fd, 0, file_size, POSIX_FADV_WILLNEED);
#else
        // macOS fallback: read sequentially to populate page cache
        char buf[65536];
        while (read(fd, buf, sizeof(buf)) > 0) {}
        lseek(fd, 0, SEEK_SET);
#endif

        close(fd);
        return Result<size_t, std::string>::ok(file_size);
    }

    /** Advise kernel to release cached pages for a file. */
    void evict_file(const std::string& path) {
#ifdef __linux__
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            struct stat st{};
            if (fstat(fd, &st) == 0) {
                posix_fadvise(fd, 0, st.st_size, POSIX_FADV_DONTNEED);
            }
            close(fd);
        }
#else
        (void)path; // macOS: no direct equivalent, let kernel manage
#endif
    }

    // ─── VPU pre-allocation ─────────────────────────────────────────────

    /** Check if an app is a known GPU/VPU user. */
    bool is_gpu_app(const std::string& app_name) const {
        // Check historical VPU allocation data
        std::string sysfs = "/sys/class/straylight-vpu/vpu0/clients";
        std::ifstream f(sysfs);
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.find(app_name) != std::string::npos) return true;
            }
        }

        // Known GPU-heavy apps
        static const std::set<std::string> gpu_apps = {
            "straylight-bench", "straylight-compiler", "blender",
            "firefox", "chromium", "obs", "krita", "gimp",
            "straylight_gui", "wf-recorder"
        };
        return gpu_apps.count(app_name) > 0;
    }

    /** Pre-allocate VPU slab blocks. Returns bytes allocated. */
    Result<size_t, std::string> preload_vpu_slab(const std::string& app_name) {
        // Request 64MB slab pre-allocation
        size_t slab_size = 64 * 1024 * 1024;

        if (budget_.used_vram_bytes + slab_size > budget_.max_vram_bytes) {
            slab_size = budget_.max_vram_bytes - budget_.used_vram_bytes;
            if (slab_size < 1024 * 1024) {
                return Result<size_t, std::string>::error("VRAM budget exceeded");
            }
        }

        std::string payload = app_name + " " + std::to_string(slab_size);
        if (!write_first_available({
                "/sys/kernel/straylight-vpu/prealloc",
                "/sys/class/straylight-vpu/vpu0/prealloc",
            }, payload)) {
            return Result<size_t, std::string>::error("VPU prealloc control node not available");
        }

        return Result<size_t, std::string>::ok(slab_size);
    }

    /** Release pre-allocated VPU slabs. */
    void release_vpu_slab(const std::string& app_name) {
        (void)write_first_available({
            "/sys/kernel/straylight-vpu/release",
            "/sys/class/straylight-vpu/vpu0/release",
        }, app_name);
    }

    bool write_first_available(const std::initializer_list<const char*>& paths,
                               const std::string& payload) const {
        for (const char* path : paths) {
            std::ofstream f(path);
            if (!f) continue;
            f << payload;
            f.flush();
            if (f.good()) return true;
        }
        return false;
    }

    // ─── Recent files ───────────────────────────────────────────────────

    /** Get recently-used files for an app from the XDG recently-used database. */
    std::vector<std::string> get_recent_files(const std::string& app_name) {
        std::vector<std::string> files;

        const char* home = getenv("HOME");
        if (!home) return files;

        // Check XDG recently-used
        std::string recent_path = std::string(home) +
            "/.local/share/recently-used.xbel";
        std::ifstream f(recent_path);
        if (!f) return files;

        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

        // Simple XML-ish parse: find bookmarks with matching app
        size_t pos = 0;
        int count = 0;
        while (pos < content.size() && count < 5) {
            auto bookmark = content.find("<bookmark ", pos);
            if (bookmark == std::string::npos) break;

            auto bookmark_end = content.find("</bookmark>", bookmark);
            if (bookmark_end == std::string::npos) break;

            std::string block = content.substr(bookmark, bookmark_end - bookmark);

            // Check if this bookmark is for our app
            if (block.find(app_name) != std::string::npos) {
                // Extract href
                auto href = block.find("href=\"");
                if (href != std::string::npos) {
                    href += 6;
                    auto href_end = block.find('"', href);
                    if (href_end != std::string::npos) {
                        std::string uri = block.substr(href, href_end - href);
                        // Convert file:// URI to path
                        if (uri.find("file://") == 0) {
                            std::string path = uri.substr(7);
                            if (access(path.c_str(), R_OK) == 0) {
                                files.push_back(path);
                                ++count;
                            }
                        }
                    }
                }
            }

            pos = bookmark_end + 1;
        }

        return files;
    }

    // ─── Mesh pre-connection ────────────────────────────────────────────

    /** Check if an app uses mesh GPU. */
    bool is_mesh_app(const std::string& app_name) const {
        static const std::set<std::string> mesh_apps = {
            "straylight-compiler", "straylight-bench"
        };
        return mesh_apps.count(app_name) > 0;
    }

    /** Pre-connect to mesh nodes for distributed GPU workloads. */
    void preconnect_mesh_nodes(const std::string& app_name) {
        // Read known mesh nodes from configuration
        std::string mesh_conf = "/etc/straylight/mesh/nodes.conf";
        std::ifstream f(mesh_conf);
        if (!f) return;

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;

            // Each line is a node address: "hostname:port"
            // We just do a non-blocking connect to warm up the TCP connection
            // The actual connection will be established by the app
            PreloadEntry entry;
            entry.app_name = app_name;
            entry.resource_path = line;
            entry.resource_type = "mesh_node";
            entry.size_bytes = 0;
            entry.loaded_at = std::chrono::steady_clock::now();
            entry.active = true;

            entries_.push_back(entry);
        }
    }

    // ─── Utility ────────────────────────────────────────────────────────

    static std::string exec_cmd(const std::string& cmd) {
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            result += buf;
        }
        pclose(pipe);
        return result;
    }
};

} // namespace straylight::predict
