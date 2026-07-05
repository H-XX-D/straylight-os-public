// bin/rdma_bus/verbs.h
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>

struct ibv_context;
struct ibv_pd;
struct ibv_mr;
struct ibv_comp_channel;
struct ibv_cq;

namespace straylight::rdma {

/// Wraps libibverbs device/PD/MR lifecycle.
class VerbsContext {
public:
    VerbsContext() = default;
    ~VerbsContext();

    VerbsContext(const VerbsContext&) = delete;
    VerbsContext& operator=(const VerbsContext&) = delete;
    VerbsContext(VerbsContext&& other) noexcept;
    VerbsContext& operator=(VerbsContext&& other) noexcept;

    /// Open the named RDMA device (e.g. "mlx5_0"). Pass "" for first available.
    Result<void, std::string> open(const std::string& device_name);

    /// Create a protection domain on the opened device.
    Result<void, std::string> create_pd();

    /// Register a memory region. Returns the lkey for the region.
    Result<uint32_t, std::string> alloc_mr(void* buf, size_t size, int access);

    /// Close everything (MRs, PD, device).
    void close();

    /// Accessors
    [[nodiscard]] struct ibv_context* context() const noexcept { return ctx_; }
    [[nodiscard]] struct ibv_pd*      pd()      const noexcept { return pd_; }
    [[nodiscard]] bool                is_open() const noexcept { return ctx_ != nullptr; }

    /// Get the rkey of the most recently registered MR.
    [[nodiscard]] uint32_t last_rkey() const noexcept { return last_rkey_; }

    /// Create a completion queue.
    Result<void, std::string> create_cq(int cq_depth);

    /// Get the completion queue.
    [[nodiscard]] struct ibv_cq* cq() const noexcept { return cq_; }

private:
    struct ibv_context*      ctx_          = nullptr;
    struct ibv_pd*           pd_           = nullptr;
    struct ibv_comp_channel* comp_channel_ = nullptr;
    struct ibv_cq*           cq_           = nullptr;

    // Track registered MRs for cleanup
    struct MrEntry {
        struct ibv_mr* mr;
    };
    static constexpr size_t MAX_MRS = 64;
    MrEntry  mrs_[MAX_MRS] = {};
    size_t   mr_count_     = 0;
    uint32_t last_rkey_    = 0;

    void cleanup() noexcept;
};

} // namespace straylight::rdma
