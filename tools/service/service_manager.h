// tools/service/service_manager.h
// Unified service manager — wraps systemd with StrayLight extensions.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

struct ServiceInfo {
    std::string name;
    std::string description;
    std::string state;
    std::string sub_state;
    bool is_straylight = false;
    uint64_t memory_bytes = 0;
    double cpu_percent = 0.0;
    std::string main_pid;
    std::string started_at;
};

struct ServiceResources {
    std::string name;
    uint64_t memory_current = 0;
    uint64_t memory_peak = 0;
    uint64_t memory_limit = 0;
    double cpu_usage_seconds = 0.0;
    uint64_t io_read_bytes = 0;
    uint64_t io_write_bytes = 0;
    int task_count = 0;
};

struct ServiceDep {
    std::string name;
    std::string type;
};

class ServiceManager {
public:
    ServiceManager() = default;
    std::vector<ServiceInfo> list(const std::string& filter = "all") const;
    Result<ServiceInfo, std::string> status(const std::string& name) const;
    Result<void, std::string> start(const std::string& name);
    Result<void, std::string> stop(const std::string& name);
    Result<void, std::string> restart(const std::string& name);
    Result<std::string, std::string> logs(const std::string& name, int lines = 50, bool follow = false) const;
    std::vector<ServiceDep> dependencies(const std::string& name) const;
    Result<ServiceResources, std::string> resources(const std::string& name) const;
    Result<void, std::string> create_service(const std::string& name, const std::string& description = "");
    Result<void, std::string> enable(const std::string& name);
    Result<void, std::string> disable(const std::string& name);

private:
    std::string systemctl(const std::string& args) const;
    std::string journalctl(const std::string& args) const;
    bool is_straylight_service(const std::string& name) const;
    uint64_t parse_memory(const std::string& s) const;
};

} // namespace straylight
