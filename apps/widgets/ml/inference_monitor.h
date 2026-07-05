// apps/widgets/ml/inference_monitor.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct InferenceEndpoint {
    std::string model_name;
    std::string endpoint;
    float rps = 0.0f;          // requests per second
    float latency_p50_ms = 0;
    float latency_p95_ms = 0;
    float latency_p99_ms = 0;
    int batch_size = 1;
    int queue_depth = 0;
    uint64_t total_requests = 0;
    uint64_t total_errors = 0;
    // History
    static constexpr int kHistLen = 120;
    std::array<float, kHistLen> rps_history{};
    std::array<float, kHistLen> latency_history{};
    int hist_offset = 0;
};

class InferenceMonitorWidget : public WidgetBase {
public:
    const char* name() const override { return "Inference Monitor"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<InferenceEndpoint> endpoints_;
    int selected_ = 0;
    std::string error_msg_;

    void try_connect();
    void fetch_metrics();
    void push_history(InferenceEndpoint& ep);
};

} // namespace straylight::widgets
