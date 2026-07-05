// tools/bench/benchmarks.h
// Hardware benchmark suite for StrayLight OS.
// Real workload measurements, not synthetic scores.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// A single benchmark measurement.
struct BenchResult {
    std::string name;           ///< Benchmark identifier
    std::string category;       ///< "cpu", "memory", "storage", "gpu", "network", "ml"
    double value;               ///< Measured value
    std::string unit;           ///< "GB/s", "GFLOPS", "ms", "tok/s", "IOPS", "us"
    double duration_seconds;    ///< Wall clock time for this benchmark
    std::string description;    ///< Human-friendly explanation of what was measured

    /// Format the result as a one-line summary.
    std::string format() const;
};

/// Collection of all benchmark results from a run.
struct BenchReport {
    std::string hostname;
    std::string os_version;
    std::string kernel_version;
    std::string cpu_model;
    uint32_t cpu_cores;
    uint64_t memory_bytes;
    std::string gpu_model;
    std::string timestamp;      ///< ISO 8601
    std::vector<BenchResult> results;
};

/// Hardware benchmark suite.
/// Each method runs an independent benchmark and returns its result.
class BenchmarkSuite {
public:
    BenchmarkSuite();

    // ---- CPU benchmarks ----

    /// Single-threaded floating-point throughput (GFLOPS).
    /// Tight FMA loop: a = a * b + c, measured over 1 billion iterations.
    BenchResult cpu_single_thread();

    /// Multi-threaded floating-point throughput (GFLOPS).
    /// Same FMA loop across all available cores.
    BenchResult cpu_multi_thread(int threads = 0);

    /// AVX/SIMD memory throughput (GB/s).
    /// Measures sustained bandwidth of vectorized memory operations.
    BenchResult cpu_avx_bandwidth();

    /// Context switch latency (microseconds).
    /// Pipe ping-pong between parent and child process.
    BenchResult cpu_context_switch();

    // ---- Memory benchmarks ----

    /// Sequential read bandwidth (GB/s).
    /// Large buffer memcpy with sequential access pattern.
    BenchResult mem_sequential_read();

    /// Sequential write bandwidth (GB/s).
    /// Large buffer memset with sequential access pattern.
    BenchResult mem_sequential_write();

    /// Random access latency (nanoseconds).
    /// Pointer-chasing through a shuffled linked list in a large array.
    BenchResult mem_random_access();

    /// NUMA cross-socket bandwidth (GB/s).
    /// Measures memory throughput between NUMA nodes (single-node if only one).
    BenchResult mem_numa_bandwidth();

    // ---- Storage benchmarks ----

    /// Sequential read throughput (GB/s) using O_DIRECT.
    BenchResult storage_seq_read(const std::string& path);

    /// Sequential write throughput (GB/s) using O_DIRECT.
    BenchResult storage_seq_write(const std::string& path);

    /// Random 4K read IOPS using O_DIRECT.
    BenchResult storage_rand_read(const std::string& path);

    /// Random 4K write IOPS using O_DIRECT.
    BenchResult storage_rand_write(const std::string& path);

    /// fsync latency (microseconds).
    BenchResult storage_latency(const std::string& path);

    // ---- GPU benchmarks ----

    /// GPU memory bandwidth (GB/s) via VPU device or fallback.
    BenchResult gpu_memory_bandwidth(int device = 0);

    /// GPU compute throughput (GFLOPS) via SGEMM-like workload.
    BenchResult gpu_compute_flops(int device = 0);

    /// GPU-to-GPU P2P bandwidth (GB/s).
    BenchResult gpu_p2p_bandwidth(int dev_a = 0, int dev_b = 1);

    // ---- Network benchmarks ----

    /// TCP throughput (Gbps) to a target host.
    BenchResult net_throughput(const std::string& target);

    /// Network round-trip latency (milliseconds) via ICMP or TCP.
    BenchResult net_latency(const std::string& target);

    // ---- ML benchmarks ----

    /// ML inference throughput (tokens/second).
    /// Forks straylight-alice and measures generation speed.
    BenchResult ml_inference_throughput();

    // ---- Utilities ----

    /// Run all benchmarks and return the full report.
    BenchReport run_all(const std::string& storage_path = "/tmp",
                        const std::string& network_target = "");

    /// Populate system information in a report.
    void populate_system_info(BenchReport& report);

    /// Compare two reports and return human-readable diff.
    static std::string compare(const BenchReport& baseline,
                               const BenchReport& current);

private:
    int num_cores_;

    /// Measure wall-clock time of a callable.
    template <typename F>
    double measure_seconds(F&& fn) {
        auto start = std::chrono::high_resolution_clock::now();
        fn();
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double>(end - start).count();
    }
};

} // namespace straylight
