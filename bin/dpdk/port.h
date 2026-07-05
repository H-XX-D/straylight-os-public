// bin/dpdk/port.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::dpdk {

/// Per-port statistics snapshot.
struct PortStats {
    uint64_t rx_packets   = 0;
    uint64_t tx_packets   = 0;
    uint64_t rx_bytes     = 0;
    uint64_t tx_bytes     = 0;
    uint64_t rx_errors    = 0;
    uint64_t tx_errors    = 0;
    uint64_t rx_missed    = 0;
};

/// Manages DPDK EAL initialization and per-port lifecycle.
class PortManager {
public:
    PortManager() = default;
    ~PortManager();

    PortManager(const PortManager&) = delete;
    PortManager& operator=(const PortManager&) = delete;

    /// Initialize the DPDK EAL. Must be called before any other operation.
    Result<void, std::string> init(const std::vector<std::string>& eal_args);

    /// Configure a port with the given number of RX/TX queues.
    Result<void, std::string> configure_port(uint16_t port_id,
                                             uint16_t nb_rx_queues,
                                             uint16_t nb_tx_queues);

    /// Start a configured port.
    Result<void, std::string> start(uint16_t port_id);

    /// Stop a running port.
    void stop(uint16_t port_id);

    /// Retrieve current statistics for a port.
    PortStats stats(uint16_t port_id);

    /// Number of ports available after init.
    [[nodiscard]] uint16_t port_count() const noexcept;

    /// Whether EAL has been initialized.
    [[nodiscard]] bool initialized() const noexcept;

private:
    bool     eal_initialized_ = false;
    uint16_t nb_ports_        = 0;

    // Per-port mempool (one per port for simplicity)
    struct PortContext {
        void*    mempool   = nullptr; // rte_mempool*
        uint16_t nb_rx     = 0;
        uint16_t nb_tx     = 0;
        bool     configured = false;
        bool     started    = false;
    };

    static constexpr uint16_t MAX_PORTS = 32;
    PortContext ports_[MAX_PORTS] = {};

    static constexpr uint32_t NUM_MBUFS       = 8191;
    static constexpr uint32_t MBUF_CACHE_SIZE = 250;
    static constexpr uint16_t RX_RING_SIZE    = 1024;
    static constexpr uint16_t TX_RING_SIZE    = 1024;
};

} // namespace straylight::dpdk
