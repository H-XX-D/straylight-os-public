// apps/widgets/ml/training_dashboard.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct TrainingRun {
    std::string run_id;
    std::string model_name;
    int current_epoch = 0;
    int total_epochs = 0;
    int current_step = 0;
    int total_steps = 0;
    float learning_rate = 0.0f;
    float eta_seconds = 0.0f;
    // Loss history (up to 1024 points)
    static constexpr int kMaxPoints = 1024;
    std::vector<float> train_loss;
    std::vector<float> val_loss;
    float best_val_loss = 1e9f;
    int best_epoch = 0;
};

class TrainingDashboardWidget : public WidgetBase {
public:
    const char* name() const override { return "Training Dashboard"; }
    float poll_interval() const override { return 2.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<TrainingRun> runs_;
    int selected_run_ = 0;
    std::string error_msg_;

    void try_connect();
    void fetch_runs();
};

} // namespace straylight::widgets
