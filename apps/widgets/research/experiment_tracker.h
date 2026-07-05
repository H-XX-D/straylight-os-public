// apps/widgets/research/experiment_tracker.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <map>

namespace straylight::widgets {

struct HyperParam {
    std::string key;
    std::string value;
};

struct MetricSeries {
    std::string name;
    std::vector<float> values;
};

struct Experiment {
    std::string id;
    std::string name;
    std::string status;  // "running", "completed", "failed"
    std::string start_time;
    std::string end_time;
    float duration_sec = 0.0f;
    std::vector<HyperParam> hyperparams;
    std::vector<MetricSeries> metrics;
    std::string notes;
    std::string tags;
};

class ExperimentTrackerWidget : public WidgetBase {
public:
    const char* name() const override { return "Experiment Tracker"; }
    float poll_interval() const override { return 5.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<Experiment> experiments_;
    int selected_exp_ = -1;
    int compare_exp_ = -1;
    std::string error_msg_;
    char filter_buf_[128]{};
    bool show_comparison_ = false;

    void try_connect();
    void fetch_experiments();
    static ImVec4 status_color(const std::string& s);
};

} // namespace straylight::widgets
