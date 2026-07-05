// apps/widgets/system/entropy_pool.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct DrbgInstance {
    std::string name;
    std::string algorithm;  // "ctr-aes-256", "hmac-sha256", etc.
    uint64_t bytes_generated = 0;
    uint64_t reseed_count = 0;
    bool health_ok = true;
};

class EntropyPoolWidget : public WidgetBase {
public:
    const char* name() const override { return "Entropy Pool"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    int entropy_avail_ = 0;
    int poolsize_ = 0;
    bool kernel_source_online_ = false;
    bool rdrand_available_ = false;
    bool rdseed_available_ = false;
    bool jitter_available_ = false;
    uint64_t kernel_bytes_generated_ = 0;
    uint64_t kernel_health_failures_ = 0;
    std::vector<DrbgInstance> drbgs_;
    IpcJsonClient ipc_;
    bool ipc_connected_ = false;
    std::string error_msg_;

    // History
    static constexpr int kHistLen = 120;
    std::array<float, kHistLen> entropy_hist_{};
    int hist_offset_ = 0;

    void read_kernel_entropy();
    void read_straylight_entropy();
    void read_drbg_stats();
};

} // namespace straylight::widgets
