// tools/port/port_manager.h
// Port manager for StrayLight OS — listening ports, process mapping, reservations.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace straylight {

/// A listening port entry.
struct PortEntry {
    uint16_t    port = 0;
    std::string protocol;       // "tcp", "tcp6", "udp", "udp6"
    std::string local_addr;
    std::string remote_addr;
    std::string state;          // "LISTEN", "ESTABLISHED", etc.
    int         pid = 0;
    std::string process_name;
    std::string user;
};

/// Port reservation for a StrayLight service.
struct PortReservation {
    uint16_t    port = 0;
    std::string service_name;
    std::string protocol;       // "tcp", "udp"
    std::string description;
};

/// Port conflict — two things want the same port.
struct PortConflict {
    uint16_t    port = 0;
    std::string reserved_for;
    std::string actual_process;
    int         actual_pid = 0;
};

class PortManager {
public:
    PortManager();
    ~PortManager();

    /// List all listening ports.
    Result<std::vector<PortEntry>, std::string> list(bool listening_only = false) const;

    /// Find who is using a specific port.
    Result<PortEntry, std::string> who(uint16_t port) const;

    /// Kill the process occupying a port.
    Result<void, std::string> kill(uint16_t port, int signal = 15) const;

    /// Reserve a port for a StrayLight service.
    Result<void, std::string> reserve(uint16_t port, const std::string& service,
                                       const std::string& protocol = "tcp",
                                       const std::string& description = "");

    /// Unreserve a port.
    Result<void, std::string> unreserve(uint16_t port);

    /// Check for conflicts between reservations and actual usage.
    Result<std::vector<PortConflict>, std::string> conflicts() const;

    /// Find free ports in a range.
    Result<std::vector<uint16_t>, std::string> free_ports(uint16_t start, uint16_t end) const;

    /// List all reservations.
    Result<std::vector<PortReservation>, std::string> reservations() const;

private:
    std::string config_path() const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    std::vector<PortEntry> parse_proc_net(const std::string& proto) const;
    std::string pid_to_name(int pid) const;
    std::string pid_to_user(int pid) const;
    std::string hex_to_ip(const std::string& hex_ip) const;
    uint16_t hex_to_port(const std::string& hex_port) const;
    std::map<uint16_t, PortReservation> load_reservations() const;
    void save_reservations(const std::map<uint16_t, PortReservation>& reservations) const;
};

} // namespace straylight
