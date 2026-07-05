// apps/hub/service_panel.h
// Service management panel — list daemons, show status, start/stop/restart, view logs.
#pragma once

#include <imgui.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight::hub {

struct ServiceInfo {
    std::string name;
    std::string display_name;
    std::string description;
    std::string module_name;
    std::string probe_path;
    bool kernel_surface = false;
    enum class Status { Running, Stopped, Error, Unknown } status = Status::Unknown;
    int pid = 0;
    std::string uptime;
    std::string log_tail;    // Last N lines of log
    bool log_expanded = false;
};

class ServicePanel {
public:
    ServicePanel();

    /// Refresh service status from the system.
    void refresh();

    /// Render the services tab.
    void render();

private:
    std::vector<ServiceInfo> services_;
    std::mutex services_mu_;
    float refresh_timer_ = 0.0f;
    float refresh_interval_ = 5.0f;

    void init_service_list();
    void query_service_status(ServiceInfo& svc);
    void read_service_log(ServiceInfo& svc, int lines);
    void service_action(const std::string& name, const std::string& action);

    ImVec4 status_color(ServiceInfo::Status s) const;
    const char* status_text(ServiceInfo::Status s) const;
};

} // namespace straylight::hub
