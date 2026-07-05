// apps/widgets/system/io_latency.h
#pragma once

#include <straylight/widget.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct BlockDevice {
    std::string name;        // e.g. "nvme0n1", "sda"
    std::string model;
    std::string scheduler;
    uint64_t read_ios = 0;
    uint64_t write_ios = 0;
    uint64_t read_sectors = 0;
    uint64_t write_sectors = 0;
    uint64_t read_ticks_ms = 0;
    uint64_t write_ticks_ms = 0;
    uint64_t io_ticks_ms = 0;
    float read_avg_ms = 0.0f;
    float write_avg_ms = 0.0f;
    float util_pct = 0.0f;
    // Previous snapshot for delta
    uint64_t prev_read_ios = 0;
    uint64_t prev_write_ios = 0;
    uint64_t prev_read_ticks = 0;
    uint64_t prev_write_ticks = 0;
    uint64_t prev_io_ticks = 0;
    // Latency histogram buckets (from /sys/block/X/stat doesn't have these,
    // but we derive per-interval averages)
    static constexpr int kHistLen = 120;
    std::array<float, kHistLen> read_lat_hist{};
    std::array<float, kHistLen> write_lat_hist{};
    std::array<float, kHistLen> util_hist{};
    int hist_offset = 0;
};

class IoLatencyWidget : public WidgetBase {
public:
    const char* name() const override { return "I/O Latency"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    std::vector<BlockDevice> devices_;
    int selected_dev_ = 0;
    float sample_interval_ = 1.0f;

    void discover_devices();
    void read_stats();
    void push_history(BlockDevice& dev);
};

} // namespace straylight::widgets
