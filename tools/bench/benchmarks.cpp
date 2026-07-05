// tools/bench/benchmarks.cpp
// Hardware benchmark implementations for StrayLight OS.
#include "benchmarks.h"

#include <straylight/log.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Networking
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace straylight {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static std::string read_proc_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f.is_open()) std::getline(f, line);
    return line;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static std::string run_command(const std::string& cmd) {
    std::string output;
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return output;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp)) {
        output += buf;
    }
    pclose(fp);
    return trim(output);
}

static std::string iso_timestamp() {
    time_t now = time(nullptr);
    struct tm tm{};
    gmtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// --------------------------------------------------------------------------
// BenchResult
// --------------------------------------------------------------------------

std::string BenchResult::format() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%-30s %12.2f %-8s (%.1fs) %s",
                  name.c_str(), value, unit.c_str(), duration_seconds,
                  description.c_str());
    return buf;
}

// --------------------------------------------------------------------------
// BenchmarkSuite
// --------------------------------------------------------------------------

BenchmarkSuite::BenchmarkSuite()
    : num_cores_(static_cast<int>(std::thread::hardware_concurrency())) {
    if (num_cores_ < 1) num_cores_ = 1;
}

// ---- CPU: Single-threaded FMA ----

BenchResult BenchmarkSuite::cpu_single_thread() {
    BenchResult r;
    r.name = "cpu_single_thread";
    r.category = "cpu";
    r.unit = "GFLOPS";
    r.description = "Single-core FP throughput (FMA loop)";

    const uint64_t iterations = 500'000'000ULL;
    volatile double a = 1.0, b = 2.0, c = 3.0;

    double elapsed = measure_seconds([&] {
        double va = a, vb = b, vc = c;
        for (uint64_t i = 0; i < iterations; ++i) {
            va = va * vb + vc;  // 2 FLOPs per iteration (multiply + add)
        }
        a = va;  // Prevent optimization
    });

    // 2 FLOPs per iteration
    double flops = static_cast<double>(iterations) * 2.0;
    r.value = flops / elapsed / 1e9;
    r.duration_seconds = elapsed;

    return r;
}

// ---- CPU: Multi-threaded FMA ----

BenchResult BenchmarkSuite::cpu_multi_thread(int threads) {
    if (threads <= 0) threads = num_cores_;

    BenchResult r;
    r.name = "cpu_multi_thread";
    r.category = "cpu";
    r.unit = "GFLOPS";
    r.description = "All-core FP throughput (" + std::to_string(threads) + " threads)";

    const uint64_t iterations_per_thread = 500'000'000ULL;
    std::atomic<double> total_flops{0.0};

    double elapsed = measure_seconds([&] {
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                volatile double a = 1.0 + t * 0.001;
                double b = 2.0, c = 3.0;
                double va = a;
                for (uint64_t i = 0; i < iterations_per_thread; ++i) {
                    va = va * b + c;
                }
                a = va;
                // Each thread: iterations * 2 FLOPs
                double tf = static_cast<double>(iterations_per_thread) * 2.0;
                double old = total_flops.load(std::memory_order_relaxed);
                while (!total_flops.compare_exchange_weak(old, old + tf,
                           std::memory_order_relaxed)) {}
            });
        }

        for (auto& w : workers) w.join();
    });

    r.value = total_flops.load() / elapsed / 1e9;
    r.duration_seconds = elapsed;

    return r;
}

// ---- CPU: AVX/SIMD bandwidth ----

BenchResult BenchmarkSuite::cpu_avx_bandwidth() {
    BenchResult r;
    r.name = "cpu_avx_bandwidth";
    r.category = "cpu";
    r.unit = "GB/s";
    r.description = "SIMD memory read bandwidth";

    const size_t buffer_size = 256 * 1024 * 1024;  // 256MB
    std::vector<char> buffer(buffer_size);

    // Fill with data to ensure pages are mapped
    std::memset(buffer.data(), 0xAB, buffer_size);

    const int iterations = 10;
    volatile uint64_t sink = 0;

    double elapsed = measure_seconds([&] {
        for (int iter = 0; iter < iterations; ++iter) {
            // Read through the buffer, summing values to prevent optimization
            const uint64_t* ptr = reinterpret_cast<const uint64_t*>(buffer.data());
            size_t count = buffer_size / sizeof(uint64_t);
            uint64_t sum = 0;
            for (size_t i = 0; i < count; ++i) {
                sum += ptr[i];
            }
            sink = sum;
        }
    });

    double bytes_read = static_cast<double>(buffer_size) * iterations;
    r.value = bytes_read / elapsed / 1e9;
    r.duration_seconds = elapsed;

    return r;
}

// ---- CPU: Context switch latency ----

BenchResult BenchmarkSuite::cpu_context_switch() {
    BenchResult r;
    r.name = "cpu_context_switch";
    r.category = "cpu";
    r.unit = "us";
    r.description = "Context switch latency (pipe ping-pong)";

    int pipe_ab[2];  // Parent writes, child reads
    int pipe_ba[2];  // Child writes, parent reads

    if (::pipe(pipe_ab) < 0 || ::pipe(pipe_ba) < 0) {
        r.value = -1;
        r.duration_seconds = 0;
        r.description = "Failed to create pipes";
        return r;
    }

    const int rounds = 100'000;
    char byte = 'x';

    pid_t pid = ::fork();
    if (pid == 0) {
        // Child: echo back
        ::close(pipe_ab[1]);
        ::close(pipe_ba[0]);
        for (int i = 0; i < rounds; ++i) {
            if (::read(pipe_ab[0], &byte, 1) != 1) break;
            if (::write(pipe_ba[1], &byte, 1) != 1) break;
        }
        ::close(pipe_ab[0]);
        ::close(pipe_ba[1]);
        _exit(0);
    }

    // Parent
    ::close(pipe_ab[0]);
    ::close(pipe_ba[1]);

    double elapsed = measure_seconds([&] {
        for (int i = 0; i < rounds; ++i) {
            ::write(pipe_ab[1], &byte, 1);
            ::read(pipe_ba[0], &byte, 1);
        }
    });

    ::close(pipe_ab[1]);
    ::close(pipe_ba[0]);
    int status;
    ::waitpid(pid, &status, 0);

    // Each round involves 2 context switches (parent->child, child->parent)
    double total_switches = static_cast<double>(rounds) * 2.0;
    double latency_us = (elapsed / total_switches) * 1e6;

    r.value = latency_us;
    r.duration_seconds = elapsed;

    return r;
}

// ---- Memory: Sequential read ----

BenchResult BenchmarkSuite::mem_sequential_read() {
    BenchResult r;
    r.name = "mem_sequential_read";
    r.category = "memory";
    r.unit = "GB/s";
    r.description = "Sequential memory read bandwidth (memcpy)";

    const size_t size = 512 * 1024 * 1024;  // 512MB
    void* src = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* dst = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (src == MAP_FAILED || dst == MAP_FAILED) {
        if (src != MAP_FAILED) ::munmap(src, size);
        if (dst != MAP_FAILED) ::munmap(dst, size);
        r.value = -1;
        r.duration_seconds = 0;
        r.description = "mmap failed";
        return r;
    }

    // Touch pages
    std::memset(src, 0xCD, size);
    std::memset(dst, 0, size);

#ifdef __linux__
    ::madvise(src, size, MADV_SEQUENTIAL);
    ::madvise(dst, size, MADV_SEQUENTIAL);
#endif

    const int iterations = 5;

    double elapsed = measure_seconds([&] {
        for (int i = 0; i < iterations; ++i) {
            std::memcpy(dst, src, size);
        }
    });

    double bytes = static_cast<double>(size) * iterations;
    r.value = bytes / elapsed / 1e9;
    r.duration_seconds = elapsed;

    ::munmap(src, size);
    ::munmap(dst, size);

    return r;
}

// ---- Memory: Sequential write ----

BenchResult BenchmarkSuite::mem_sequential_write() {
    BenchResult r;
    r.name = "mem_sequential_write";
    r.category = "memory";
    r.unit = "GB/s";
    r.description = "Sequential memory write bandwidth (memset)";

    const size_t size = 512 * 1024 * 1024;
    void* buf = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (buf == MAP_FAILED) {
        r.value = -1;
        r.duration_seconds = 0;
        return r;
    }

    // Touch pages first
    std::memset(buf, 0, size);

    const int iterations = 5;

    double elapsed = measure_seconds([&] {
        for (int i = 0; i < iterations; ++i) {
            std::memset(buf, static_cast<int>(i & 0xFF), size);
        }
    });

    double bytes = static_cast<double>(size) * iterations;
    r.value = bytes / elapsed / 1e9;
    r.duration_seconds = elapsed;

    ::munmap(buf, size);

    return r;
}

// ---- Memory: Random access latency ----

BenchResult BenchmarkSuite::mem_random_access() {
    BenchResult r;
    r.name = "mem_random_access";
    r.category = "memory";
    r.unit = "ns";
    r.description = "Random memory access latency (pointer chase)";

    // Create a shuffled linked list in a large array
    const size_t num_entries = 16 * 1024 * 1024;  // ~128MB of pointers
    std::vector<size_t> indices(num_entries);

    // Fisher-Yates shuffle to create a random cycle
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937_64 rng(42);
    for (size_t i = num_entries - 1; i > 0; --i) {
        std::uniform_int_distribution<size_t> dist(0, i);
        std::swap(indices[i], indices[dist(rng)]);
    }

    // Build pointer-chasing array: next[i] = indices[i]
    std::vector<size_t> next(num_entries);
    for (size_t i = 0; i < num_entries; ++i) {
        next[i] = indices[i];
    }

    const size_t chase_steps = 10'000'000;
    volatile size_t pos = 0;

    double elapsed = measure_seconds([&] {
        size_t p = 0;
        for (size_t i = 0; i < chase_steps; ++i) {
            p = next[p];
        }
        pos = p;
    });

    double latency_ns = (elapsed / static_cast<double>(chase_steps)) * 1e9;
    r.value = latency_ns;
    r.duration_seconds = elapsed;

    return r;
}

// ---- Memory: NUMA bandwidth ----

BenchResult BenchmarkSuite::mem_numa_bandwidth() {
    BenchResult r;
    r.name = "mem_numa_bandwidth";
    r.category = "memory";
    r.unit = "GB/s";
    r.description = "Cross-NUMA memory bandwidth (or single-node fallback)";

    // Detect NUMA nodes
    int numa_nodes = 1;
#ifdef __linux__
    std::string node_str = read_proc_line("/sys/devices/system/node/online");
    if (!node_str.empty()) {
        auto dash = node_str.find('-');
        if (dash != std::string::npos) {
            numa_nodes = std::atoi(node_str.substr(dash + 1).c_str()) + 1;
        }
    }
#endif

    // Regardless of NUMA topology, measure cross-thread memory bandwidth
    const size_t size = 256 * 1024 * 1024;
    void* buf = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (buf == MAP_FAILED) {
        r.value = -1;
        r.duration_seconds = 0;
        return r;
    }

    std::memset(buf, 0xAA, size);

    // Multi-threaded read
    int threads = std::min(num_cores_, 8);
    size_t chunk = size / static_cast<size_t>(threads);
    std::atomic<double> total_bytes{0.0};

    double elapsed = measure_seconds([&] {
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                volatile uint64_t sum = 0;
                const uint64_t* ptr = reinterpret_cast<const uint64_t*>(
                    static_cast<uint8_t*>(buf) + t * chunk);
                size_t count = chunk / sizeof(uint64_t);
                for (int iter = 0; iter < 3; ++iter) {
                    for (size_t i = 0; i < count; ++i) {
                        sum += ptr[i];
                    }
                }
                double old = total_bytes.load(std::memory_order_relaxed);
                double tb = static_cast<double>(chunk) * 3.0;
                while (!total_bytes.compare_exchange_weak(old, old + tb,
                           std::memory_order_relaxed)) {}
            });
        }
        for (auto& w : workers) w.join();
    });

    r.value = total_bytes.load() / elapsed / 1e9;
    r.duration_seconds = elapsed;
    if (numa_nodes > 1) {
        r.description = "Cross-NUMA bandwidth (" + std::to_string(numa_nodes) + " nodes)";
    }

    ::munmap(buf, size);

    return r;
}

// ---- Storage: Sequential read ----

BenchResult BenchmarkSuite::storage_seq_read(const std::string& path) {
    BenchResult r;
    r.name = "storage_seq_read";
    r.category = "storage";
    r.unit = "GB/s";
    r.description = "Sequential read throughput (O_DIRECT)";

    // Create a temporary test file
    std::string test_file = path + "/straylight_bench_seq.tmp";
    const size_t file_size = 256 * 1024 * 1024;  // 256MB
    const size_t block_size = 1024 * 1024;        // 1MB blocks

    // Write the test file first
    {
        int fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            r.value = -1;
            r.description = "Cannot create test file: " + std::string(::strerror(errno));
            return r;
        }

        // Use aligned buffer for O_DIRECT compatibility
        void* wbuf = nullptr;
        ::posix_memalign(&wbuf, 4096, block_size);
        std::memset(wbuf, 0xAB, block_size);

        for (size_t written = 0; written < file_size; written += block_size) {
            ::write(fd, wbuf, block_size);
        }
        ::fsync(fd);
        ::close(fd);
        ::free(wbuf);
    }

    // Read with O_DIRECT if possible
    int flags = O_RDONLY;
#ifdef __linux__
    flags |= O_DIRECT;
#endif
    int fd = ::open(test_file.c_str(), flags);
    if (fd < 0) {
        // Fallback without O_DIRECT
        fd = ::open(test_file.c_str(), O_RDONLY);
        if (fd < 0) {
            ::unlink(test_file.c_str());
            r.value = -1;
            r.description = "Cannot open test file";
            return r;
        }
    }

    void* rbuf = nullptr;
    ::posix_memalign(&rbuf, 4096, block_size);

    double elapsed = measure_seconds([&] {
        ::lseek(fd, 0, SEEK_SET);
        size_t total_read = 0;
        while (total_read < file_size) {
            ssize_t n = ::read(fd, rbuf, block_size);
            if (n <= 0) break;
            total_read += static_cast<size_t>(n);
        }
    });

    r.value = static_cast<double>(file_size) / elapsed / 1e9;
    r.duration_seconds = elapsed;

    ::close(fd);
    ::free(rbuf);
    ::unlink(test_file.c_str());

    return r;
}

// ---- Storage: Sequential write ----

BenchResult BenchmarkSuite::storage_seq_write(const std::string& path) {
    BenchResult r;
    r.name = "storage_seq_write";
    r.category = "storage";
    r.unit = "GB/s";
    r.description = "Sequential write throughput (O_DIRECT)";

    std::string test_file = path + "/straylight_bench_seqw.tmp";
    const size_t file_size = 256 * 1024 * 1024;
    const size_t block_size = 1024 * 1024;

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef __linux__
    flags |= O_DIRECT;
#endif
    int fd = ::open(test_file.c_str(), flags, 0644);
    if (fd < 0) {
        fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            r.value = -1;
            r.description = "Cannot create test file";
            return r;
        }
    }

    void* wbuf = nullptr;
    ::posix_memalign(&wbuf, 4096, block_size);
    std::memset(wbuf, 0xCD, block_size);

    double elapsed = measure_seconds([&] {
        size_t total_written = 0;
        while (total_written < file_size) {
            ssize_t n = ::write(fd, wbuf, block_size);
            if (n <= 0) break;
            total_written += static_cast<size_t>(n);
        }
        ::fsync(fd);
    });

    r.value = static_cast<double>(file_size) / elapsed / 1e9;
    r.duration_seconds = elapsed;

    ::close(fd);
    ::free(wbuf);
    ::unlink(test_file.c_str());

    return r;
}

// ---- Storage: Random 4K read IOPS ----

BenchResult BenchmarkSuite::storage_rand_read(const std::string& path) {
    BenchResult r;
    r.name = "storage_rand_read_4k";
    r.category = "storage";
    r.unit = "IOPS";
    r.description = "Random 4K read IOPS";

    std::string test_file = path + "/straylight_bench_rr.tmp";
    const size_t file_size = 128 * 1024 * 1024;  // 128MB
    const size_t block_size = 4096;
    const size_t num_blocks = file_size / block_size;

    // Create test file
    {
        int fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            r.value = -1;
            r.description = "Cannot create test file";
            return r;
        }
        void* buf = nullptr;
        ::posix_memalign(&buf, 4096, block_size);
        std::memset(buf, 0xEF, block_size);
        for (size_t i = 0; i < num_blocks; ++i) {
            ::write(fd, buf, block_size);
        }
        ::fsync(fd);
        ::close(fd);
        ::free(buf);
    }

    // Generate random offsets
    std::mt19937_64 rng(12345);
    const int io_count = 50000;
    std::vector<off_t> offsets(io_count);
    for (int i = 0; i < io_count; ++i) {
        offsets[i] = static_cast<off_t>((rng() % num_blocks) * block_size);
    }

    int flags = O_RDONLY;
#ifdef __linux__
    flags |= O_DIRECT;
#endif
    int fd = ::open(test_file.c_str(), flags);
    if (fd < 0) {
        fd = ::open(test_file.c_str(), O_RDONLY);
    }

    if (fd < 0) {
        ::unlink(test_file.c_str());
        r.value = -1;
        r.description = "Cannot open test file";
        return r;
    }

    void* rbuf = nullptr;
    ::posix_memalign(&rbuf, 4096, block_size);

    double elapsed = measure_seconds([&] {
        for (int i = 0; i < io_count; ++i) {
            ::lseek(fd, offsets[i], SEEK_SET);
            ::read(fd, rbuf, block_size);
        }
    });

    r.value = static_cast<double>(io_count) / elapsed;
    r.duration_seconds = elapsed;

    ::close(fd);
    ::free(rbuf);
    ::unlink(test_file.c_str());

    return r;
}

// ---- Storage: Random 4K write IOPS ----

BenchResult BenchmarkSuite::storage_rand_write(const std::string& path) {
    BenchResult r;
    r.name = "storage_rand_write_4k";
    r.category = "storage";
    r.unit = "IOPS";
    r.description = "Random 4K write IOPS";

    std::string test_file = path + "/straylight_bench_rw.tmp";
    const size_t file_size = 128 * 1024 * 1024;
    const size_t block_size = 4096;
    const size_t num_blocks = file_size / block_size;

    // Pre-allocate file
    {
        int fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            r.value = -1;
            r.description = "Cannot create test file";
            return r;
        }
        if (::ftruncate(fd, static_cast<off_t>(file_size)) != 0) {
            ::close(fd);
            ::unlink(test_file.c_str());
            r.value = -1;
            r.description = "ftruncate failed";
            return r;
        }
        ::close(fd);
    }

    std::mt19937_64 rng(67890);
    const int io_count = 20000;
    std::vector<off_t> offsets(io_count);
    for (int i = 0; i < io_count; ++i) {
        offsets[i] = static_cast<off_t>((rng() % num_blocks) * block_size);
    }

    int flags = O_WRONLY;
#ifdef __linux__
    flags |= O_DIRECT;
#endif
    int fd = ::open(test_file.c_str(), flags);
    if (fd < 0) {
        fd = ::open(test_file.c_str(), O_WRONLY);
    }

    if (fd < 0) {
        ::unlink(test_file.c_str());
        r.value = -1;
        r.description = "Cannot open test file";
        return r;
    }

    void* wbuf = nullptr;
    ::posix_memalign(&wbuf, 4096, block_size);
    std::memset(wbuf, 0xBE, block_size);

    double elapsed = measure_seconds([&] {
        for (int i = 0; i < io_count; ++i) {
            ::lseek(fd, offsets[i], SEEK_SET);
            ::write(fd, wbuf, block_size);
        }
        ::fsync(fd);
    });

    r.value = static_cast<double>(io_count) / elapsed;
    r.duration_seconds = elapsed;

    ::close(fd);
    ::free(wbuf);
    ::unlink(test_file.c_str());

    return r;
}

// ---- Storage: fsync latency ----

BenchResult BenchmarkSuite::storage_latency(const std::string& path) {
    BenchResult r;
    r.name = "storage_fsync_latency";
    r.category = "storage";
    r.unit = "us";
    r.description = "fsync latency per 4K write";

    std::string test_file = path + "/straylight_bench_lat.tmp";
    int fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        r.value = -1;
        r.description = "Cannot create test file";
        return r;
    }

    char buf[4096];
    std::memset(buf, 0xFF, sizeof(buf));
    const int rounds = 1000;

    double elapsed = measure_seconds([&] {
        for (int i = 0; i < rounds; ++i) {
            ::write(fd, buf, sizeof(buf));
            ::fsync(fd);
        }
    });

    r.value = (elapsed / rounds) * 1e6;  // microseconds per fsync
    r.duration_seconds = elapsed;

    ::close(fd);
    ::unlink(test_file.c_str());

    return r;
}

// ---- GPU: Memory bandwidth ----

BenchResult BenchmarkSuite::gpu_memory_bandwidth(int device) {
    BenchResult r;
    r.name = "gpu_memory_bandwidth";
    r.category = "gpu";
    r.unit = "GB/s";
    r.description = "GPU memory bandwidth (VPU device " + std::to_string(device) + ")";

    // Try StrayLight VPU device
    std::string vpu_path = "/dev/straylight-vpu" + std::to_string(device);
    int fd = ::open(vpu_path.c_str(), O_RDWR);

    if (fd >= 0) {
        // VPU device is available — use ioctl-based benchmark
        // Allocate GPU memory and measure transfer throughput
        const size_t alloc_size = 128 * 1024 * 1024;  // 128MB

        // VPU ioctl definitions (from straylight-vpu driver)
        struct vpu_alloc_req {
            uint64_t size;
            uint64_t flags;
            uint64_t handle;  // output
        };

        struct vpu_transfer_req {
            uint64_t handle;
            uint64_t offset;
            uint64_t size;
            uint64_t user_ptr;
            uint32_t direction;  // 0=H2D, 1=D2H
        };

        // Ioctl numbers — defined by straylight-vpu driver
        constexpr unsigned long VPU_IOCTL_ALLOC = 0xC0185600;
        constexpr unsigned long VPU_IOCTL_TRANSFER = 0xC0285601;
        constexpr unsigned long VPU_IOCTL_FREE = 0x40085602;

        vpu_alloc_req alloc_req{};
        alloc_req.size = alloc_size;
        alloc_req.flags = 0;

        if (::ioctl(fd, VPU_IOCTL_ALLOC, &alloc_req) == 0) {
            void* host_buf = nullptr;
            ::posix_memalign(&host_buf, 4096, alloc_size);
            std::memset(host_buf, 0xAB, alloc_size);

            const int transfers = 10;

            // Measure H2D + D2H
            double elapsed = measure_seconds([&] {
                for (int i = 0; i < transfers; ++i) {
                    // Host to Device
                    vpu_transfer_req tx{};
                    tx.handle = alloc_req.handle;
                    tx.offset = 0;
                    tx.size = alloc_size;
                    tx.user_ptr = reinterpret_cast<uint64_t>(host_buf);
                    tx.direction = 0;
                    ::ioctl(fd, VPU_IOCTL_TRANSFER, &tx);

                    // Device to Host
                    tx.direction = 1;
                    ::ioctl(fd, VPU_IOCTL_TRANSFER, &tx);
                }
            });

            // Total bytes: transfers * 2 directions * alloc_size
            double bytes = static_cast<double>(transfers) * 2.0 *
                           static_cast<double>(alloc_size);
            r.value = bytes / elapsed / 1e9;
            r.duration_seconds = elapsed;

            ::ioctl(fd, VPU_IOCTL_FREE, &alloc_req.handle);
            ::free(host_buf);
        } else {
            r.value = -1;
            r.description = "VPU allocation failed";
        }

        ::close(fd);
    } else {
        // No VPU device — try nvidia-smi for info, then simulate bandwidth test
        // using host memory as a proxy measurement
        std::string gpu_info = run_command("nvidia-smi --query-gpu=name,memory.total "
                                           "--format=csv,noheader 2>/dev/null");
        if (gpu_info.empty()) {
            r.value = 0;
            r.description = "No GPU device detected";
            r.duration_seconds = 0;
            return r;
        }

        r.description = "GPU memory bandwidth (nvidia: " + trim(gpu_info) + ")";

        // Measure PCIe-limited host-side throughput as proxy
        const size_t size = 128 * 1024 * 1024;
        void* src = nullptr;
        void* dst = nullptr;
        ::posix_memalign(&src, 4096, size);
        ::posix_memalign(&dst, 4096, size);
        std::memset(src, 0xCD, size);

        double elapsed = measure_seconds([&] {
            for (int i = 0; i < 20; ++i) {
                std::memcpy(dst, src, size);
            }
        });

        r.value = static_cast<double>(size) * 20.0 / elapsed / 1e9;
        r.duration_seconds = elapsed;
        r.description += " (host-side proxy)";

        ::free(src);
        ::free(dst);
    }

    return r;
}

// ---- GPU: Compute FLOPS ----

BenchResult BenchmarkSuite::gpu_compute_flops(int device) {
    BenchResult r;
    r.name = "gpu_compute_flops";
    r.category = "gpu";
    r.unit = "GFLOPS";
    r.description = "GPU compute throughput (VPU device " + std::to_string(device) + ")";

    std::string vpu_path = "/dev/straylight-vpu" + std::to_string(device);
    int fd = ::open(vpu_path.c_str(), O_RDWR);

    if (fd >= 0) {
        // Submit a compute workload via VPU ioctl
        struct vpu_compute_req {
            uint32_t workgroup_x;
            uint32_t workgroup_y;
            uint32_t workgroup_z;
            uint64_t shader_handle;
            uint64_t input_handle;
            uint64_t output_handle;
        };

        constexpr unsigned long VPU_IOCTL_COMPUTE = 0xC0305603;

        // Measure SGEMM-like workload: 1024x1024 matrix multiply
        const uint32_t n = 1024;
        double flops_per_matmul = 2.0 * n * n * n;  // 2N^3 for matmul
        const int iterations = 100;

        double elapsed = measure_seconds([&] {
            vpu_compute_req req{};
            req.workgroup_x = n / 16;
            req.workgroup_y = n / 16;
            req.workgroup_z = 1;
            for (int i = 0; i < iterations; ++i) {
                ::ioctl(fd, VPU_IOCTL_COMPUTE, &req);
            }
        });

        r.value = (flops_per_matmul * iterations) / elapsed / 1e9;
        r.duration_seconds = elapsed;

        ::close(fd);
    } else {
        // Fallback: report CPU-based SGEMM proxy
        const int n = 512;
        std::vector<float> a(n * n, 1.0f);
        std::vector<float> b(n * n, 1.0f);
        std::vector<float> c(n * n, 0.0f);

        double elapsed = measure_seconds([&] {
            for (int i = 0; i < n; ++i) {
                for (int k = 0; k < n; ++k) {
                    float aik = a[i * n + k];
                    for (int j = 0; j < n; ++j) {
                        c[i * n + j] += aik * b[k * n + j];
                    }
                }
            }
        });

        double flops = 2.0 * n * n * n;
        r.value = flops / elapsed / 1e9;
        r.duration_seconds = elapsed;
        r.description = "GPU compute (CPU SGEMM fallback, no GPU detected)";
    }

    return r;
}

// ---- GPU: P2P bandwidth ----

BenchResult BenchmarkSuite::gpu_p2p_bandwidth(int dev_a, int dev_b) {
    BenchResult r;
    r.name = "gpu_p2p_bandwidth";
    r.category = "gpu";
    r.unit = "GB/s";
    r.description = "GPU-to-GPU P2P bandwidth (dev" + std::to_string(dev_a) +
                    " -> dev" + std::to_string(dev_b) + ")";

    std::string vpu_a = "/dev/straylight-vpu" + std::to_string(dev_a);
    std::string vpu_b = "/dev/straylight-vpu" + std::to_string(dev_b);

    int fd_a = ::open(vpu_a.c_str(), O_RDWR);
    int fd_b = ::open(vpu_b.c_str(), O_RDWR);

    if (fd_a >= 0 && fd_b >= 0) {
        struct vpu_p2p_req {
            uint64_t src_handle;
            uint64_t dst_handle;
            uint64_t size;
            uint32_t src_device;
            uint32_t dst_device;
        };

        constexpr unsigned long VPU_IOCTL_P2P = 0xC0285604;

        const size_t size = 64 * 1024 * 1024;  // 64MB
        const int transfers = 20;

        double elapsed = measure_seconds([&] {
            vpu_p2p_req req{};
            req.size = size;
            req.src_device = static_cast<uint32_t>(dev_a);
            req.dst_device = static_cast<uint32_t>(dev_b);
            for (int i = 0; i < transfers; ++i) {
                ::ioctl(fd_a, VPU_IOCTL_P2P, &req);
            }
        });

        r.value = static_cast<double>(size) * transfers / elapsed / 1e9;
        r.duration_seconds = elapsed;
    } else {
        r.value = 0;
        r.duration_seconds = 0;
        r.description = "P2P not available (need 2+ VPU devices)";
    }

    if (fd_a >= 0) ::close(fd_a);
    if (fd_b >= 0) ::close(fd_b);

    return r;
}

// ---- Network: Throughput ----

BenchResult BenchmarkSuite::net_throughput(const std::string& target) {
    BenchResult r;
    r.name = "net_throughput";
    r.category = "network";
    r.unit = "Gbps";
    r.description = "TCP throughput to " + target;

    if (target.empty()) {
        r.value = 0;
        r.description = "No network target specified";
        r.duration_seconds = 0;
        return r;
    }

    // Parse host:port (default port 5201 for iperf-like behavior)
    std::string host = target;
    int port = 5201;
    auto colon = target.rfind(':');
    if (colon != std::string::npos) {
        host = target.substr(0, colon);
        port = std::atoi(target.substr(colon + 1).c_str());
    }

    // Resolve hostname
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);

    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        r.value = 0;
        r.description = "Cannot resolve " + host;
        r.duration_seconds = 0;
        return r;
    }

    int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ::freeaddrinfo(res);
        r.value = 0;
        r.description = "Cannot create socket";
        return r;
    }

    // Set TCP_NODELAY for throughput test
    int flag = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (::connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(sock);
        ::freeaddrinfo(res);

        // Fallback: measure loopback throughput
        r.description = "TCP throughput (loopback fallback, target unreachable)";

        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            r.value = 0;
            return r;
        }

        const size_t buf_size = 1024 * 1024;  // 1MB blocks
        std::vector<char> buf(buf_size, 'A');
        const size_t total_send = 512 * 1024 * 1024;  // 512MB

        pid_t pid = ::fork();
        if (pid == 0) {
            ::close(sv[0]);
            std::vector<char> rbuf(buf_size);
            size_t total = 0;
            while (total < total_send) {
                ssize_t n = ::read(sv[1], rbuf.data(), buf_size);
                if (n <= 0) break;
                total += static_cast<size_t>(n);
            }
            ::close(sv[1]);
            _exit(0);
        }

        ::close(sv[1]);

        double elapsed = measure_seconds([&] {
            size_t total = 0;
            while (total < total_send) {
                ssize_t n = ::write(sv[0], buf.data(), buf_size);
                if (n <= 0) break;
                total += static_cast<size_t>(n);
            }
        });

        ::close(sv[0]);
        int status;
        ::waitpid(pid, &status, 0);

        r.value = static_cast<double>(total_send) * 8.0 / elapsed / 1e9;
        r.duration_seconds = elapsed;
        return r;
    }

    ::freeaddrinfo(res);

    // Send data and measure throughput
    const size_t buf_size = 1024 * 1024;
    std::vector<char> buf(buf_size, 'X');
    const double test_duration = 5.0;  // 5 seconds

    size_t total_sent = 0;
    double elapsed = measure_seconds([&] {
        auto start = std::chrono::high_resolution_clock::now();
        while (true) {
            auto now = std::chrono::high_resolution_clock::now();
            double dt = std::chrono::duration<double>(now - start).count();
            if (dt >= test_duration) break;

            ssize_t n = ::send(sock, buf.data(), buf_size, 0);
            if (n <= 0) break;
            total_sent += static_cast<size_t>(n);
        }
    });

    ::close(sock);

    r.value = static_cast<double>(total_sent) * 8.0 / elapsed / 1e9;
    r.duration_seconds = elapsed;

    return r;
}

// ---- Network: Latency ----

BenchResult BenchmarkSuite::net_latency(const std::string& target) {
    BenchResult r;
    r.name = "net_latency";
    r.category = "network";
    r.unit = "ms";
    r.description = "Network round-trip latency to " + target;

    if (target.empty()) {
        r.value = 0;
        r.description = "No network target specified";
        return r;
    }

    std::string host = target;
    auto colon = target.rfind(':');
    if (colon != std::string::npos) {
        host = target.substr(0, colon);
    }

    // Use TCP connect latency (doesn't require raw sockets/root)
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (::getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || !res) {
        // Try ICMP via ping command
        std::string ping_output = run_command(
            "ping -c 5 -q " + host + " 2>/dev/null | tail -1");
        if (!ping_output.empty()) {
            // Parse "rtt min/avg/max/mdev = X/Y/Z/W ms"
            auto eq = ping_output.find('=');
            if (eq != std::string::npos) {
                std::string vals = ping_output.substr(eq + 2);
                auto slash1 = vals.find('/');
                if (slash1 != std::string::npos) {
                    auto slash2 = vals.find('/', slash1 + 1);
                    if (slash2 != std::string::npos) {
                        r.value = std::atof(vals.substr(slash1 + 1,
                                                         slash2 - slash1 - 1).c_str());
                        r.duration_seconds = 5.0;
                        return r;
                    }
                }
            }
        }
        r.value = -1;
        r.description = "Cannot resolve " + host;
        return r;
    }

    // Measure TCP connect latency (SYN->SYN-ACK)
    const int samples = 10;
    std::vector<double> latencies;
    latencies.reserve(samples);

    double total_elapsed = 0;
    for (int i = 0; i < samples; ++i) {
        int sock = ::socket(res->ai_family, SOCK_STREAM, 0);
        if (sock < 0) continue;

        auto start = std::chrono::high_resolution_clock::now();
        int rc = ::connect(sock, res->ai_addr, res->ai_addrlen);
        auto end = std::chrono::high_resolution_clock::now();

        if (rc == 0) {
            double lat = std::chrono::duration<double, std::milli>(end - start).count();
            latencies.push_back(lat);
            total_elapsed += lat / 1000.0;
        }
        ::close(sock);
    }

    ::freeaddrinfo(res);

    if (latencies.empty()) {
        r.value = -1;
        r.description = "All connection attempts failed";
        return r;
    }

    // Report median latency
    std::sort(latencies.begin(), latencies.end());
    r.value = latencies[latencies.size() / 2];
    r.duration_seconds = total_elapsed;
    r.description = "TCP connect latency to " + host + " (median of " +
                    std::to_string(latencies.size()) + " samples)";

    return r;
}

// ---- ML: Inference throughput ----

BenchResult BenchmarkSuite::ml_inference_throughput() {
    BenchResult r;
    r.name = "ml_inference_throughput";
    r.category = "ml";
    r.unit = "tok/s";
    r.description = "ML inference throughput (straylight-alice)";

    // Test prompt — request a moderate-length response
    std::string test_prompt = "Explain the concept of a ring buffer in 50 words.";

    // Time the Alice CLI
    auto start = std::chrono::high_resolution_clock::now();

    std::string output = run_command(
        "timeout 30 straylight-alice ask \"" + test_prompt + "\" 2>/dev/null");

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    if (output.empty()) {
        // Try direct llama-cli if available
        output = run_command(
            "timeout 30 llama-cli -m /var/lib/straylight/models/*.gguf "
            "-p \"" + test_prompt + "\" -n 100 2>/dev/null");
        end = std::chrono::high_resolution_clock::now();
        elapsed = std::chrono::duration<double>(end - start).count();
    }

    if (output.empty()) {
        r.value = 0;
        r.duration_seconds = 0;
        r.description = "ML inference not available (no model loaded)";
        return r;
    }

    // Estimate token count (rough: ~0.75 tokens per word)
    size_t word_count = 0;
    bool in_word = false;
    for (char c : output) {
        if (c == ' ' || c == '\n' || c == '\t') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            word_count++;
        }
    }
    double estimated_tokens = static_cast<double>(word_count) * 1.33;

    r.value = estimated_tokens / elapsed;
    r.duration_seconds = elapsed;
    r.description = "ML inference (~" + std::to_string(static_cast<int>(estimated_tokens)) +
                    " tokens in " + std::to_string(static_cast<int>(elapsed)) + "s)";

    return r;
}

// --------------------------------------------------------------------------
// Full suite & utilities
// --------------------------------------------------------------------------

BenchReport BenchmarkSuite::run_all(const std::string& storage_path,
                                     const std::string& network_target) {
    BenchReport report;
    populate_system_info(report);
    report.timestamp = iso_timestamp();

    SL_INFO("bench: starting full benchmark suite");

    // CPU
    SL_INFO("bench: running CPU benchmarks...");
    report.results.push_back(cpu_single_thread());
    report.results.push_back(cpu_multi_thread());
    report.results.push_back(cpu_avx_bandwidth());
    report.results.push_back(cpu_context_switch());

    // Memory
    SL_INFO("bench: running memory benchmarks...");
    report.results.push_back(mem_sequential_read());
    report.results.push_back(mem_sequential_write());
    report.results.push_back(mem_random_access());
    report.results.push_back(mem_numa_bandwidth());

    // Storage
    SL_INFO("bench: running storage benchmarks...");
    report.results.push_back(storage_seq_read(storage_path));
    report.results.push_back(storage_seq_write(storage_path));
    report.results.push_back(storage_rand_read(storage_path));
    report.results.push_back(storage_rand_write(storage_path));
    report.results.push_back(storage_latency(storage_path));

    // GPU
    SL_INFO("bench: running GPU benchmarks...");
    report.results.push_back(gpu_memory_bandwidth());
    report.results.push_back(gpu_compute_flops());
    report.results.push_back(gpu_p2p_bandwidth());

    // Network
    if (!network_target.empty()) {
        SL_INFO("bench: running network benchmarks...");
        report.results.push_back(net_throughput(network_target));
        report.results.push_back(net_latency(network_target));
    }

    // ML
    SL_INFO("bench: running ML benchmarks...");
    report.results.push_back(ml_inference_throughput());

    SL_INFO("bench: complete ({} benchmarks)", report.results.size());

    return report;
}

void BenchmarkSuite::populate_system_info(BenchReport& report) {
    // Hostname
    char hostname[256];
    if (::gethostname(hostname, sizeof(hostname)) == 0) {
        report.hostname = hostname;
    }

    // OS version
#ifdef __linux__
    report.os_version = run_command("cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | "
                                     "cut -d= -f2 | tr -d '\"'");
    if (report.os_version.empty()) {
        report.os_version = "StrayLight OS";
    }
    report.kernel_version = run_command("uname -r");
#else
    report.os_version = run_command("sw_vers -productName 2>/dev/null") + " " +
                        run_command("sw_vers -productVersion 2>/dev/null");
    report.kernel_version = run_command("uname -r");
#endif

    // CPU model
#ifdef __linux__
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                report.cpu_model = trim(line.substr(colon + 1));
            }
            break;
        }
    }
#else
    report.cpu_model = run_command("sysctl -n machdep.cpu.brand_string 2>/dev/null");
#endif

    report.cpu_cores = static_cast<uint32_t>(num_cores_);

    // Memory
#ifdef __linux__
    std::string meminfo = read_proc_line("/proc/meminfo");
    if (meminfo.find("MemTotal") != std::string::npos) {
        auto colon = meminfo.find(':');
        if (colon != std::string::npos) {
            report.memory_bytes = std::strtoull(
                trim(meminfo.substr(colon + 1)).c_str(), nullptr, 10) * 1024;
        }
    }
#else
    std::string mem_str = run_command("sysctl -n hw.memsize 2>/dev/null");
    if (!mem_str.empty()) {
        report.memory_bytes = std::strtoull(mem_str.c_str(), nullptr, 10);
    }
#endif

    // GPU
    report.gpu_model = run_command(
        "cat /sys/kernel/straylight-vpu/model 2>/dev/null || "
        "nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1");
    if (report.gpu_model.empty()) {
        report.gpu_model = "none detected";
    }
}

std::string BenchmarkSuite::compare(const BenchReport& baseline,
                                     const BenchReport& current) {
    std::ostringstream out;

    out << "=== StrayLight Benchmark Comparison ===\n\n";
    out << "Baseline: " << baseline.timestamp << " (" << baseline.hostname << ")\n";
    out << "Current:  " << current.timestamp << " (" << current.hostname << ")\n\n";

    out << std::left << std::setw(32) << "Benchmark"
        << std::right << std::setw(12) << "Baseline"
        << std::setw(12) << "Current"
        << std::setw(10) << "Change"
        << "\n";
    out << std::string(66, '-') << "\n";

    for (const auto& cur : current.results) {
        // Find matching baseline result
        const BenchResult* base = nullptr;
        for (const auto& b : baseline.results) {
            if (b.name == cur.name) {
                base = &b;
                break;
            }
        }

        out << std::left << std::setw(32) << cur.name;

        if (base) {
            char base_buf[16], cur_buf[16], change_buf[16];
            std::snprintf(base_buf, sizeof(base_buf), "%.2f", base->value);
            std::snprintf(cur_buf, sizeof(cur_buf), "%.2f", cur.value);

            double pct = 0;
            if (base->value != 0) {
                pct = ((cur.value - base->value) / base->value) * 100.0;
            }
            std::snprintf(change_buf, sizeof(change_buf), "%+.1f%%", pct);

            out << std::right << std::setw(12) << base_buf
                << std::setw(12) << cur_buf
                << std::setw(10) << change_buf;

            // Indicate if change is good or bad
            // For latency metrics (us, ms, ns), lower is better
            bool lower_is_better = (cur.unit == "us" || cur.unit == "ms" ||
                                    cur.unit == "ns");
            if (lower_is_better) {
                if (pct < -5) out << "  BETTER";
                else if (pct > 5) out << "  WORSE";
            } else {
                if (pct > 5) out << "  BETTER";
                else if (pct < -5) out << "  WORSE";
            }
        } else {
            char cur_buf[16];
            std::snprintf(cur_buf, sizeof(cur_buf), "%.2f", cur.value);
            out << std::right << std::setw(12) << "n/a"
                << std::setw(12) << cur_buf
                << std::setw(10) << "new";
        }

        out << " " << cur.unit << "\n";
    }

    return out.str();
}

} // namespace straylight
