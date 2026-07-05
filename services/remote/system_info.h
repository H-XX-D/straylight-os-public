// services/remote/system_info.h
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace straylight {

/// Gathers comprehensive system information from the local machine.
/// Reads from /proc, /sys, and various system utilities.
class SystemInfo {
public:
    SystemInfo();
    ~SystemInfo();

    /// Gather all system information and return as JSON.
    nlohmann::json gather();

private:
    nlohmann::json gather_cpu();
    nlohmann::json gather_ram();
    nlohmann::json gather_gpus();
    nlohmann::json gather_disks();
    nlohmann::json gather_network();
    nlohmann::json gather_os();
    nlohmann::json gather_services();

    // Helpers
    static std::string read_file_contents(const std::string& path);
    static std::string run_command(const std::string& cmd);
    static std::string trim(const std::string& s);
};

} // namespace straylight
