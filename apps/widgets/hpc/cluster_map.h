// apps/widgets/hpc/cluster_map.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <imgui.h>
#include <string>
#include <vector>

namespace straylight::widgets {

enum class NodeState : uint8_t { Online, Degraded, Offline, Draining, Unknown };

struct ClusterNode {
    std::string hostname;
    std::string ip;
    NodeState state = NodeState::Unknown;
    float cpu_pct = 0.0f;
    float gpu_pct = 0.0f;
    float mem_pct = 0.0f;
    int gpu_count = 0;
    int cpu_cores = 0;
    float mem_total_gb = 0.0f;
    int running_jobs = 0;
};

class ClusterMapWidget : public WidgetBase {
public:
    const char* name() const override { return "Cluster Map"; }
    float poll_interval() const override { return 3.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<ClusterNode> nodes_;
    int selected_node_ = -1;
    std::string error_msg_;
    int grid_cols_ = 8;

    void try_connect();
    void fetch_nodes();
    static ImVec4 state_color(NodeState s);
    static const char* state_str(NodeState s);
};

} // namespace straylight::widgets
