// bin/dpdk/port.cpp
#include "port.h"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_version.h>

#include <cstring>
#include <string>

namespace straylight::dpdk {

PortManager::~PortManager() {
    if (eal_initialized_) {
        for (uint16_t i = 0; i < nb_ports_; ++i) {
            if (ports_[i].started) {
                rte_eth_dev_stop(i);
                ports_[i].started = false;
            }
            if (ports_[i].configured) {
                rte_eth_dev_close(i);
                ports_[i].configured = false;
            }
        }
        rte_eal_cleanup();
    }
}

Result<void, std::string> PortManager::init(const std::vector<std::string>& eal_args) {
    if (eal_initialized_) {
        return Result<void, std::string>::error("EAL already initialized");
    }

    // Build argc/argv for rte_eal_init
    std::vector<char*> argv;
    // EAL expects argv[0] to be the program name
    std::string prog = "straylight-dpdk";
    argv.push_back(const_cast<char*>(prog.c_str()));
    std::vector<std::string> arg_storage(eal_args.begin(), eal_args.end());
    for (auto& a : arg_storage) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }

    int ret = rte_eal_init(static_cast<int>(argv.size()), argv.data());
    if (ret < 0) {
        return Result<void, std::string>::error(
            "rte_eal_init failed with error " + std::to_string(rte_errno));
    }

    nb_ports_ = rte_eth_dev_count_avail();
    if (nb_ports_ == 0) {
        return Result<void, std::string>::error("No DPDK-capable ports found");
    }
    if (nb_ports_ > MAX_PORTS) {
        nb_ports_ = MAX_PORTS;
    }

    eal_initialized_ = true;
    return Result<void, std::string>::ok();
}

Result<void, std::string> PortManager::configure_port(uint16_t port_id,
                                                      uint16_t nb_rx_queues,
                                                      uint16_t nb_tx_queues) {
    if (!eal_initialized_) {
        return Result<void, std::string>::error("EAL not initialized");
    }
    if (port_id >= nb_ports_) {
        return Result<void, std::string>::error(
            "Invalid port_id " + std::to_string(port_id));
    }
    if (ports_[port_id].configured) {
        return Result<void, std::string>::error(
            "Port " + std::to_string(port_id) + " already configured");
    }

    // Get device info for limits
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        return Result<void, std::string>::error(
            "rte_eth_dev_info_get failed for port " + std::to_string(port_id));
    }

    // Port configuration
    struct rte_eth_conf port_conf = {};
    // DPDK < 21.11 uses ETH_MQ_RX_NONE; >= 21.11 renamed to RTE_ETH_MQ_RX_NONE
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
#else
    port_conf.rxmode.mq_mode = ETH_MQ_RX_NONE;
    port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;
#endif

    ret = rte_eth_dev_configure(port_id, nb_rx_queues, nb_tx_queues, &port_conf);
    if (ret != 0) {
        return Result<void, std::string>::error(
            "rte_eth_dev_configure failed: " + std::to_string(ret));
    }

    // Adjust ring sizes
    uint16_t rx_ring = RX_RING_SIZE;
    uint16_t tx_ring = TX_RING_SIZE;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_ring, &tx_ring);
    if (ret != 0) {
        return Result<void, std::string>::error(
            "rte_eth_dev_adjust_nb_rx_tx_desc failed: " + std::to_string(ret));
    }

    // Create a mempool for this port
    std::string pool_name = "mbuf_pool_" + std::to_string(port_id);
    struct rte_mempool* mbuf_pool = rte_pktmbuf_pool_create(
        pool_name.c_str(), NUM_MBUFS, MBUF_CACHE_SIZE, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_eth_dev_socket_id(port_id));
    if (!mbuf_pool) {
        return Result<void, std::string>::error(
            "Failed to create mbuf pool for port " + std::to_string(port_id));
    }

    // Setup RX queues
    for (uint16_t q = 0; q < nb_rx_queues; ++q) {
        ret = rte_eth_rx_queue_setup(port_id, q, rx_ring,
                                     rte_eth_dev_socket_id(port_id),
                                     nullptr, mbuf_pool);
        if (ret < 0) {
            rte_mempool_free(mbuf_pool);
            return Result<void, std::string>::error(
                "rte_eth_rx_queue_setup failed for queue " + std::to_string(q));
        }
    }

    // Setup TX queues
    for (uint16_t q = 0; q < nb_tx_queues; ++q) {
        ret = rte_eth_tx_queue_setup(port_id, q, tx_ring,
                                     rte_eth_dev_socket_id(port_id),
                                     nullptr);
        if (ret < 0) {
            rte_mempool_free(mbuf_pool);
            return Result<void, std::string>::error(
                "rte_eth_tx_queue_setup failed for queue " + std::to_string(q));
        }
    }

    ports_[port_id].mempool    = mbuf_pool;
    ports_[port_id].nb_rx      = nb_rx_queues;
    ports_[port_id].nb_tx      = nb_tx_queues;
    ports_[port_id].configured = true;

    return Result<void, std::string>::ok();
}

Result<void, std::string> PortManager::start(uint16_t port_id) {
    if (!eal_initialized_) {
        return Result<void, std::string>::error("EAL not initialized");
    }
    if (port_id >= nb_ports_ || !ports_[port_id].configured) {
        return Result<void, std::string>::error(
            "Port " + std::to_string(port_id) + " not configured");
    }
    if (ports_[port_id].started) {
        return Result<void, std::string>::error(
            "Port " + std::to_string(port_id) + " already started");
    }

    int ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        return Result<void, std::string>::error(
            "rte_eth_dev_start failed: " + std::to_string(ret));
    }

    // Enable promiscuous mode for packet capture use-cases
    rte_eth_promiscuous_enable(port_id);
    ports_[port_id].started = true;

    return Result<void, std::string>::ok();
}

void PortManager::stop(uint16_t port_id) {
    if (port_id < nb_ports_ && ports_[port_id].started) {
        rte_eth_dev_stop(port_id);
        ports_[port_id].started = false;
    }
}

PortStats PortManager::stats(uint16_t port_id) {
    PortStats ps;
    if (port_id >= nb_ports_ || !ports_[port_id].configured) {
        return ps;
    }

    struct rte_eth_stats eth_stats;
    if (rte_eth_stats_get(port_id, &eth_stats) == 0) {
        ps.rx_packets = eth_stats.ipackets;
        ps.tx_packets = eth_stats.opackets;
        ps.rx_bytes   = eth_stats.ibytes;
        ps.tx_bytes   = eth_stats.obytes;
        ps.rx_errors  = eth_stats.ierrors;
        ps.tx_errors  = eth_stats.oerrors;
        ps.rx_missed  = eth_stats.imissed;
    }

    return ps;
}

uint16_t PortManager::port_count() const noexcept {
    return nb_ports_;
}

bool PortManager::initialized() const noexcept {
    return eal_initialized_;
}

} // namespace straylight::dpdk
