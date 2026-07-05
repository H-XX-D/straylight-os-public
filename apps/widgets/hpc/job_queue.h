// apps/widgets/hpc/job_queue.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <imgui.h>
#include <string>
#include <vector>

namespace straylight::widgets {

enum class JobState : uint8_t { Pending, Running, Completed, Failed, Cancelled };

struct JobEntry {
    std::string job_id;
    std::string name;
    std::string user;
    JobState state = JobState::Pending;
    int gpus_requested = 0;
    int cpus_requested = 0;
    float mem_gb_requested = 0.0f;
    std::string node;          // assigned node (if running/completed)
    float progress_pct = 0.0f;
    float elapsed_sec = 0.0f;
    float eta_sec = 0.0f;
    std::string submit_time;
    std::string start_time;
    int priority = 0;
};

class JobQueueWidget : public WidgetBase {
public:
    const char* name() const override { return "Job Queue"; }
    float poll_interval() const override { return 2.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<JobEntry> jobs_;
    int selected_job_ = -1;
    std::string error_msg_;
    int filter_state_ = -1; // -1 = all
    char filter_buf_[128]{};

    void try_connect();
    void fetch_jobs();
    static ImVec4 state_color(JobState s);
    static const char* state_str(JobState s);
    static std::string format_duration(float seconds);
};

} // namespace straylight::widgets
