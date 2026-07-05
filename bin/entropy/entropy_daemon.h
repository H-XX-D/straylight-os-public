#pragma once

#include <straylight/daemon.h>
#include <straylight/hw/entropy.h>
#include "drbg.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace straylight {

class EntropyDaemon : public DaemonBase {
public:
    /// Default constructor — creates a real hw::EntropySource on init().
    EntropyDaemon() = default;

    /// Test constructor with injected entropy source.
    explicit EntropyDaemon(std::unique_ptr<hw::EntropySource> source)
        : source_(std::move(source)) {}

    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    /// Run NIST SP 800-90B health check on hardware source.
    Result<void, SLError> run_health_check();

private:
    std::unique_ptr<hw::EntropySource> source_;
    CtrDrbg drbg_;
    int health_interval_s_ = 60;
    int reseed_interval_s_ = 3600;
    std::chrono::steady_clock::time_point last_health_{};
    std::chrono::steady_clock::time_point last_reseed_{};
    std::chrono::steady_clock::time_point started_at_{};
    std::string socket_path_ = "/run/straylight/entropy.sock";
    int server_fd_ = -1;
    bool hardware_rng_available_ = false;
    bool last_health_ok_ = false;
    uint64_t health_check_count_ = 0;
    uint64_t health_failure_count_ = 0;
    std::string last_health_error_;

    Result<void, SLError> setup_ipc();
    void teardown_ipc();
    void handle_ipc(int timeout_ms);
    void handle_client(int fd);
    nlohmann::json dispatch(const nlohmann::json& request) const;
    nlohmann::json build_drbg_stats() const;
    nlohmann::json build_pool_status() const;
    nlohmann::json build_kernel_source_status() const;
};

} // namespace straylight
