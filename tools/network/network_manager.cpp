// tools/network/network_manager.cpp
// Unified network management implementation for StrayLight OS.
// Merges: NetworkManager, WifiManager, NetworkScanner, ProbeDiagnostics.

#include "network_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>

// POSIX / Linux socket headers — conditionally included so the file parses
// cleanly on macOS developer machines during diagnostics review; the actual
// target platform is Linux (Debian-based).
#ifdef __linux__
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/ip.h>
#  include <netinet/ip_icmp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#else
// macOS — include what we can; raw ICMP paths are gated by __linux__ below.
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace straylight {

// ===========================================================================
// Construction / helpers
// ===========================================================================

NetworkManager::NetworkManager() {
    fs::create_directories(config_dir());
}

NetworkManager::~NetworkManager() = default;

std::string NetworkManager::config_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/networks";
}

Result<std::string, std::string> NetworkManager::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd +
            "\noutput: " + output);
    }
    return Result<std::string, std::string>::ok(output);
}

bool NetworkManager::has_nmcli() const {
    return run_cmd("which nmcli 2>/dev/null").has_value();
}

bool NetworkManager::has_iw() const {
    return run_cmd("which iw 2>/dev/null").has_value();
}

// ===========================================================================
// Interface status
// ===========================================================================

InterfaceStatus NetworkManager::read_sysfs_interface(const std::string& name) const {
    InterfaceStatus iface;
    iface.name = name;
    std::string base = "/sys/class/net/" + name;

    std::ifstream type_f(base + "/type");
    if (type_f.is_open()) {
        int type_num = 0;
        type_f >> type_num;
        if (type_num == 1) {
            iface.type = fs::exists(base + "/wireless") ? "wifi" : "ethernet";
        } else if (type_num == 772) {
            iface.type = "loopback";
        }
    }

    if (fs::exists(base + "/bridge")) iface.type = "bridge";
    if (fs::exists(base + "/bonding")) iface.type = "bond";

    std::ifstream state_f(base + "/operstate");
    if (state_f.is_open()) std::getline(state_f, iface.state);

    std::ifstream mac_f(base + "/address");
    if (mac_f.is_open()) std::getline(mac_f, iface.mac);

    std::ifstream mtu_f(base + "/mtu");
    if (mtu_f.is_open()) mtu_f >> iface.mtu;

    std::ifstream rx_f(base + "/statistics/rx_bytes");
    if (rx_f.is_open()) rx_f >> iface.rx_bytes;
    std::ifstream tx_f(base + "/statistics/tx_bytes");
    if (tx_f.is_open()) tx_f >> iface.tx_bytes;

    return iface;
}

std::vector<InterfaceStatus> NetworkManager::parse_ip_addr(const std::string& output) const {
    std::vector<InterfaceStatus> interfaces;
    std::istringstream stream(output);
    std::string line;
    InterfaceStatus* current = nullptr;

    while (std::getline(stream, line)) {
        std::regex iface_re(R"(^\d+:\s+(\S+):\s+<([^>]*)>)");
        std::smatch m;
        if (std::regex_search(line, m, iface_re)) {
            std::string name = m[1].str();
            auto at = name.find('@');
            if (at != std::string::npos) name = name.substr(0, at);

            interfaces.push_back(read_sysfs_interface(name));
            current = &interfaces.back();

            std::string flags = m[2].str();
            if (flags.find("UP") != std::string::npos) {
                if (current->state.empty()) current->state = "up";
            }

            std::regex mtu_re(R"(mtu\s+(\d+))");
            if (std::regex_search(line, m, mtu_re)) {
                current->mtu = std::stoi(m[1].str());
            }
        } else if (current) {
            std::regex inet_re(R"(inet\s+(\S+))");
            std::smatch m2;
            if (std::regex_search(line, m2, inet_re)) current->ipv4 = m2[1].str();

            std::regex inet6_re(R"(inet6\s+(\S+))");
            if (std::regex_search(line, m2, inet6_re)) current->ipv6 = m2[1].str();
        }
    }

    // Default gateway
    auto gw_res = run_cmd("ip route show default 2>/dev/null");
    if (gw_res.has_value()) {
        std::regex gw_re(R"(default via (\S+) dev (\S+))");
        std::smatch m;
        std::string gw_out = gw_res.value();
        if (std::regex_search(gw_out, m, gw_re)) {
            std::string gw = m[1].str();
            std::string dev = m[2].str();
            for (auto& iface : interfaces) {
                if (iface.name == dev) { iface.gateway = gw; break; }
            }
        }
    }

    return interfaces;
}

Result<std::vector<InterfaceStatus>, std::string> NetworkManager::status() const {
    auto res = run_cmd("ip addr show 2>/dev/null");
    if (!res.has_value()) {
        return Result<std::vector<InterfaceStatus>, std::string>::error(
            "failed to get interface status: " + res.error());
    }

    auto interfaces = parse_ip_addr(res.value());

    for (auto& iface : interfaces) {
        if (iface.type == "wifi") {
            auto iw_res = run_cmd("iw dev " + iface.name + " link 2>/dev/null");
            if (iw_res.has_value()) {
                std::string info = iw_res.value();
                std::regex ssid_re(R"(SSID:\s+(.+))");
                std::regex signal_re(R"(signal:\s+(-?\d+)\s+dBm)");
                std::smatch m;
                if (std::regex_search(info, m, ssid_re)) iface.ssid = m[1].str();
                if (std::regex_search(info, m, signal_re))
                    iface.signal_dbm = std::stoi(m[1].str());
            }
        }
    }

    return Result<std::vector<InterfaceStatus>, std::string>::ok(interfaces);
}

// ===========================================================================
// WiFi — scan
// ===========================================================================

std::vector<WifiNetwork> NetworkManager::parse_iw_scan(const std::string& output) const {
    std::vector<WifiNetwork> networks;
    std::istringstream stream(output);
    std::string line;
    WifiNetwork* current = nullptr;

    while (std::getline(stream, line)) {
        auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos) line = line.substr(pos);

        if (line.rfind("BSS ", 0) == 0) {
            networks.emplace_back();
            current = &networks.back();
            std::regex bss_re(R"(BSS\s+([0-9a-fA-F:]+))");
            std::smatch m;
            if (std::regex_search(line, m, bss_re)) current->bssid = m[1].str();
            current->connected = (line.find("associated") != std::string::npos);
        } else if (current) {
            if (line.rfind("SSID:", 0) == 0) {
                current->ssid = line.substr(6);
            } else if (line.rfind("signal:", 0) == 0) {
                std::regex sig_re(R"((-?\d+\.?\d*)\s+dBm)");
                std::smatch m;
                if (std::regex_search(line, m, sig_re))
                    current->signal_strength = static_cast<int>(std::stod(m[1].str()));
            } else if (line.rfind("freq:", 0) == 0) {
                std::regex freq_re(R"(\d+)");
                std::smatch m;
                if (std::regex_search(line, m, freq_re)) {
                    current->frequency_mhz = std::stoi(m[0].str());
                    if (current->frequency_mhz >= 2412 && current->frequency_mhz <= 2484)
                        current->channel = (current->frequency_mhz - 2407) / 5;
                    else if (current->frequency_mhz >= 5180)
                        current->channel = (current->frequency_mhz - 5000) / 5;
                }
            } else if (line.find("WPA") != std::string::npos) {
                if (line.find("Version: 2") != std::string::npos)
                    current->security = "WPA2";
                else if (current->security.empty())
                    current->security = "WPA";
            } else if (line.find("RSN") != std::string::npos) {
                if (current->security != "WPA3") current->security = "WPA2";
            } else if (line.find("SAE") != std::string::npos) {
                current->security = "WPA3";
            }
        }
    }

    for (auto& net : networks) {
        if (net.security.empty()) net.security = "Open";
    }

    std::sort(networks.begin(), networks.end(),
              [](const auto& a, const auto& b) {
                  return a.signal_strength > b.signal_strength;
              });

    // Deduplicate — keep strongest signal per SSID
    std::vector<WifiNetwork> unique;
    for (const auto& net : networks) {
        bool found = false;
        for (const auto& u : unique) {
            if (u.ssid == net.ssid && !net.ssid.empty()) { found = true; break; }
        }
        if (!found) unique.push_back(net);
    }

    return unique;
}

Result<std::vector<WifiNetwork>, std::string> NetworkManager::scan_wifi() const {
    if (has_nmcli()) {
        auto res = run_cmd(
            "nmcli -t -f SSID,BSSID,SIGNAL,FREQ,SECURITY,ACTIVE "
            "device wifi list 2>/dev/null");
        if (res.has_value()) {
            std::vector<WifiNetwork> networks;
            std::istringstream stream(res.value());
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty()) continue;
                WifiNetwork net;
                std::vector<std::string> fields;
                size_t pos2 = 0;
                while (pos2 < line.size()) {
                    auto next = line.find(':', pos2);
                    if (next != std::string::npos && next + 1 < line.size() &&
                        line[next + 1] != ':' && (fields.size() != 1 || next - pos2 < 2)) {
                        fields.push_back(line.substr(pos2, next - pos2));
                        pos2 = next + 1;
                    } else if (next != std::string::npos) {
                        if (fields.size() == 1 && pos2 + 17 <= line.size()) {
                            fields.push_back(line.substr(pos2, 17));
                            pos2 += 18;
                        } else {
                            fields.push_back(line.substr(pos2, next - pos2));
                            pos2 = next + 1;
                        }
                    } else {
                        fields.push_back(line.substr(pos2));
                        break;
                    }
                }
                if (fields.size() >= 5) {
                    net.ssid = fields[0];
                    net.bssid = fields[1];
                    if (fields.size() > 2) {
                        try { net.signal_strength = std::stoi(fields[2]); } catch (...) {}
                    }
                    if (fields.size() > 3) {
                        try { net.frequency_mhz = std::stoi(fields[3]); } catch (...) {}
                    }
                    if (fields.size() > 4) net.security = fields[4];
                    if (fields.size() > 5) net.connected = (fields[5] == "yes");
                    networks.push_back(net);
                }
            }
            return Result<std::vector<WifiNetwork>, std::string>::ok(networks);
        }
    }

    std::string wifi_iface;
    auto ifaces_res = run_cmd("iw dev 2>/dev/null");
    if (ifaces_res.has_value()) {
        std::regex iface_re(R"(Interface\s+(\S+))");
        std::smatch m;
        std::string ifaces = ifaces_res.value();
        if (std::regex_search(ifaces, m, iface_re)) wifi_iface = m[1].str();
    }

    if (wifi_iface.empty()) {
        return Result<std::vector<WifiNetwork>, std::string>::error(
            "no wireless interface found");
    }

    auto scan_res = run_cmd("iw dev " + wifi_iface + " scan 2>/dev/null");
    if (!scan_res.has_value()) {
        return Result<std::vector<WifiNetwork>, std::string>::error(
            "WiFi scan failed: " + scan_res.error());
    }

    return Result<std::vector<WifiNetwork>, std::string>::ok(
        parse_iw_scan(scan_res.value()));
}

Result<void, std::string> NetworkManager::connect_wifi(const std::string& ssid,
                                                        const std::string& password,
                                                        bool hidden) {
    if (has_nmcli()) {
        std::string cmd = "nmcli device wifi connect '" + ssid + "'";
        if (!password.empty()) cmd += " password '" + password + "'";
        if (hidden) cmd += " hidden yes";
        cmd += " 2>/dev/null";
        auto res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<void, std::string>::error("WiFi connection failed: " + res.error());
        }

        std::ostringstream json;
        json << "{\n"
             << "  \"type\": \"wifi\",\n"
             << "  \"ssid\": \"" << ssid << "\",\n"
             << "  \"auto_connect\": true\n"
             << "}\n";
        std::string path = config_dir() + "/" + ssid + ".json";
        std::ofstream out(path);
        if (out.is_open()) out << json.str();

        return Result<void, std::string>::ok();
    }

    if (password.empty()) {
        auto res = run_cmd("iw dev wlan0 connect '" + ssid + "' 2>/dev/null");
        if (!res.has_value()) {
            return Result<void, std::string>::error("WiFi connection failed: " + res.error());
        }
        return Result<void, std::string>::ok();
    }

    auto psk_res = run_cmd("wpa_passphrase '" + ssid + "' '" + password + "' 2>/dev/null");
    if (!psk_res.has_value()) {
        return Result<void, std::string>::error("failed to generate PSK: " + psk_res.error());
    }

    std::string conf_path = "/tmp/straylight_wpa_" + ssid + ".conf";
    std::ofstream conf(conf_path);
    if (!conf.is_open()) {
        return Result<void, std::string>::error("cannot write wpa_supplicant config");
    }
    conf << psk_res.value();
    conf.close();

    auto connect_res = run_cmd(
        "wpa_supplicant -B -i wlan0 -c " + conf_path + " 2>/dev/null");
    if (!connect_res.has_value()) {
        return Result<void, std::string>::error(
            "wpa_supplicant failed: " + connect_res.error());
    }

    run_cmd("dhclient wlan0 2>/dev/null");
    return Result<void, std::string>::ok();
}

Result<void, std::string> NetworkManager::disconnect(const std::string& interface) {
    std::string iface = interface;

    if (has_nmcli()) {
        std::string cmd = "nmcli device disconnect";
        if (!iface.empty()) cmd += " " + iface;
        cmd += " 2>/dev/null";
        auto res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<void, std::string>::error("disconnect failed: " + res.error());
        }
        return Result<void, std::string>::ok();
    }

    if (iface.empty()) iface = "wlan0";
    run_cmd("ip link set " + iface + " down 2>/dev/null");
    return Result<void, std::string>::ok();
}

// ===========================================================================
// VPN
// ===========================================================================

Result<void, std::string> NetworkManager::vpn_add(const VpnConfig& config) {
    if (config.type == "wireguard") {
        if (config.config_file.empty()) {
            return Result<void, std::string>::error("WireGuard requires a config file");
        }
        std::string dest = "/etc/wireguard/" + config.name + ".conf";
        auto res = run_cmd("cp '" + config.config_file + "' '" + dest + "' 2>/dev/null");
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "failed to install WireGuard config: " + res.error());
        }
        run_cmd("chmod 600 '" + dest + "' 2>/dev/null");
    } else if (config.type == "openvpn") {
        if (config.config_file.empty()) {
            return Result<void, std::string>::error("OpenVPN requires a config file");
        }
        if (has_nmcli()) {
            auto res = run_cmd("nmcli connection import type openvpn file '" +
                               config.config_file + "' 2>/dev/null");
            if (!res.has_value()) {
                return Result<void, std::string>::error(
                    "failed to import OpenVPN config: " + res.error());
            }
        } else {
            std::string dest = "/etc/openvpn/client/" + config.name + ".conf";
            auto res = run_cmd("cp '" + config.config_file + "' '" + dest + "' 2>/dev/null");
            if (!res.has_value()) {
                return Result<void, std::string>::error(
                    "failed to install OpenVPN config: " + res.error());
            }
        }
    } else {
        return Result<void, std::string>::error("unsupported VPN type: " + config.type);
    }

    std::ostringstream json;
    json << "{\n"
         << "  \"name\": \"" << config.name << "\",\n"
         << "  \"type\": \"" << config.type << "\",\n"
         << "  \"server\": \"" << config.server << "\",\n"
         << "  \"port\": " << config.port << "\n"
         << "}\n";
    std::string path = config_dir() + "/vpn-" + config.name + ".json";
    std::ofstream out(path);
    if (out.is_open()) out << json.str();

    return Result<void, std::string>::ok();
}

Result<void, std::string> NetworkManager::vpn_connect(const std::string& name) {
    auto wg_res = run_cmd("wg-quick up " + name + " 2>/dev/null");
    if (wg_res.has_value()) return Result<void, std::string>::ok();

    if (has_nmcli()) {
        auto res = run_cmd("nmcli connection up '" + name + "' 2>/dev/null");
        if (res.has_value()) return Result<void, std::string>::ok();
    }

    auto ovpn_res = run_cmd("systemctl start openvpn-client@" + name + " 2>/dev/null");
    if (ovpn_res.has_value()) return Result<void, std::string>::ok();

    return Result<void, std::string>::error("failed to connect VPN '" + name + "'");
}

Result<void, std::string> NetworkManager::vpn_disconnect(const std::string& name) {
    run_cmd("wg-quick down " + name + " 2>/dev/null");
    if (has_nmcli()) run_cmd("nmcli connection down '" + name + "' 2>/dev/null");
    run_cmd("systemctl stop openvpn-client@" + name + " 2>/dev/null");
    return Result<void, std::string>::ok();
}

Result<std::vector<VpnConfig>, std::string> NetworkManager::vpn_list() const {
    std::vector<VpnConfig> vpns;

    auto wg_res = run_cmd("wg show interfaces 2>/dev/null");
    if (wg_res.has_value() && !wg_res.value().empty()) {
        std::istringstream stream(wg_res.value());
        std::string name;
        while (stream >> name) {
            VpnConfig vpn;
            vpn.name = name;
            vpn.type = "wireguard";
            vpn.connected = true;
            auto detail = run_cmd("wg show " + name + " 2>/dev/null");
            if (detail.has_value()) {
                std::regex endpoint_re(R"(endpoint:\s+(\S+))");
                std::regex pk_re(R"(public key:\s+(\S+))");
                std::smatch m;
                std::string info = detail.value();
                if (std::regex_search(info, m, endpoint_re)) vpn.server = m[1].str();
                if (std::regex_search(info, m, pk_re)) vpn.public_key = m[1].str();
            }
            vpns.push_back(vpn);
        }
    }

    if (fs::exists("/etc/wireguard")) {
        for (const auto& entry : fs::directory_iterator("/etc/wireguard")) {
            std::string fname = entry.path().filename().string();
            if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".conf") {
                std::string name = fname.substr(0, fname.size() - 5);
                bool already = false;
                for (const auto& v : vpns) {
                    if (v.name == name) { already = true; break; }
                }
                if (!already) {
                    VpnConfig vpn;
                    vpn.name = name;
                    vpn.type = "wireguard";
                    vpn.connected = false;
                    vpns.push_back(vpn);
                }
            }
        }
    }

    if (has_nmcli()) {
        auto res = run_cmd("nmcli -t -f NAME,TYPE,ACTIVE connection show 2>/dev/null");
        if (res.has_value()) {
            std::istringstream stream(res.value());
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("vpn") != std::string::npos) {
                    auto fc = line.find(':');
                    auto sc = line.find(':', fc + 1);
                    if (fc != std::string::npos) {
                        VpnConfig vpn;
                        vpn.name = line.substr(0, fc);
                        vpn.type = "openvpn";
                        if (sc != std::string::npos)
                            vpn.connected = (line.substr(sc + 1) == "yes");
                        vpns.push_back(vpn);
                    }
                }
            }
        }
    }

    return Result<std::vector<VpnConfig>, std::string>::ok(vpns);
}

// ===========================================================================
// Firewall
// ===========================================================================

Result<void, std::string> NetworkManager::firewall_add(const FirewallRule& rule) {
    std::ostringstream cmd;
    auto nft_check = run_cmd("which nft 2>/dev/null");
    if (nft_check.has_value()) {
        cmd << "nft add rule inet filter " << rule.chain;
        if (!rule.protocol.empty() && rule.protocol != "any") cmd << " " << rule.protocol;
        if (!rule.source.empty()) cmd << " ip saddr " << rule.source;
        if (!rule.destination.empty()) cmd << " ip daddr " << rule.destination;
        if (rule.port > 0) cmd << " dport " << rule.port;
        if (!rule.interface.empty()) {
            if (rule.chain == "input") cmd << " iifname \"" << rule.interface << "\"";
            else                       cmd << " oifname \"" << rule.interface << "\"";
        }
        cmd << " " << rule.action;
        if (!rule.comment.empty()) cmd << " comment \"" << rule.comment << "\"";
        cmd << " 2>/dev/null";
    } else {
        cmd << "ufw ";
        if (rule.action == "accept") cmd << "allow";
        else if (rule.action == "drop") cmd << "deny";
        else cmd << rule.action;
        if (!rule.source.empty()) cmd << " from " << rule.source;
        if (!rule.destination.empty()) cmd << " to " << rule.destination;
        if (rule.port > 0) cmd << " port " << rule.port;
        if (!rule.protocol.empty() && rule.protocol != "any")
            cmd << " proto " << rule.protocol;
        if (!rule.comment.empty()) cmd << " comment '" << rule.comment << "'";
        cmd << " 2>/dev/null";
    }

    auto res = run_cmd(cmd.str());
    if (!res.has_value()) {
        return Result<void, std::string>::error(
            "failed to add firewall rule: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> NetworkManager::firewall_remove(uint32_t rule_id) {
    auto nft_check = run_cmd("which nft 2>/dev/null");
    if (nft_check.has_value()) {
        for (const auto& chain : {"input", "output", "forward"}) {
            std::string cmd = "nft delete rule inet filter " +
                              std::string(chain) + " handle " +
                              std::to_string(rule_id) + " 2>/dev/null";
            if (run_cmd(cmd).has_value()) return Result<void, std::string>::ok();
        }
        return Result<void, std::string>::error(
            "failed to remove rule " + std::to_string(rule_id));
    }

    std::string cmd = "ufw delete " + std::to_string(rule_id) + " 2>/dev/null";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to remove rule: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<std::vector<FirewallRule>, std::string> NetworkManager::firewall_list() const {
    std::vector<FirewallRule> rules;

    auto nft_check = run_cmd("which nft 2>/dev/null");
    if (nft_check.has_value()) {
        auto res = run_cmd("nft -a list ruleset 2>/dev/null");
        if (res.has_value()) {
            std::istringstream stream(res.value());
            std::string line;
            std::string current_chain;

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                std::regex chain_re(R"(chain\s+(\S+)\s*\{)");
                std::smatch m;
                if (std::regex_search(line, m, chain_re)) {
                    current_chain = m[1].str();
                    continue;
                }

                std::regex handle_re(R"(#\s*handle\s+(\d+))");
                if (std::regex_search(line, m, handle_re)) {
                    FirewallRule rule;
                    rule.id = std::stoul(m[1].str());
                    rule.chain = current_chain;

                    if (line.find(" accept") != std::string::npos) rule.action = "accept";
                    else if (line.find(" drop") != std::string::npos) rule.action = "drop";
                    else if (line.find(" reject") != std::string::npos) rule.action = "reject";

                    std::regex proto_re(R"(\b(tcp|udp|icmp)\b)");
                    if (std::regex_search(line, m, proto_re)) rule.protocol = m[1].str();

                    std::regex saddr_re(R"(ip saddr (\S+))");
                    if (std::regex_search(line, m, saddr_re)) rule.source = m[1].str();

                    std::regex daddr_re(R"(ip daddr (\S+))");
                    if (std::regex_search(line, m, daddr_re)) rule.destination = m[1].str();

                    std::regex port_re(R"(dport (\d+))");
                    if (std::regex_search(line, m, port_re)) rule.port = std::stoi(m[1].str());

                    std::regex comment_re(R"(comment\s+"([^"]*)")");
                    if (std::regex_search(line, m, comment_re)) rule.comment = m[1].str();

                    rules.push_back(rule);
                }
            }
        }
    } else {
        auto res = run_cmd("ufw status numbered 2>/dev/null");
        if (res.has_value()) {
            std::istringstream stream(res.value());
            std::string line;
            while (std::getline(stream, line)) {
                std::regex rule_re(R"(\[\s*(\d+)\]\s+(.+))");
                std::smatch m;
                if (std::regex_search(line, m, rule_re)) {
                    FirewallRule rule;
                    rule.id = std::stoul(m[1].str());
                    std::string desc = m[2].str();
                    if (desc.find("ALLOW") != std::string::npos) rule.action = "accept";
                    else if (desc.find("DENY") != std::string::npos) rule.action = "drop";
                    else if (desc.find("REJECT") != std::string::npos) rule.action = "reject";
                    rule.comment = desc;
                    rules.push_back(rule);
                }
            }
        }
    }

    return Result<std::vector<FirewallRule>, std::string>::ok(rules);
}

// ===========================================================================
// DNS configuration
// ===========================================================================

Result<void, std::string> NetworkManager::dns_set(const DnsConfig& config) {
    if (fs::exists("/etc/systemd/resolved.conf")) {
        std::string servers_str;
        for (size_t i = 0; i < config.servers.size(); ++i) {
            if (i > 0) servers_str += " ";
            servers_str += config.servers[i];
        }
        std::string domains_str;
        for (size_t i = 0; i < config.search_domains.size(); ++i) {
            if (i > 0) domains_str += " ";
            domains_str += config.search_domains[i];
        }

        std::ofstream out("/etc/systemd/resolved.conf");
        if (!out.is_open()) {
            for (const auto& server : config.servers)
                run_cmd("resolvectl dns 1 " + server + " 2>/dev/null");
            return Result<void, std::string>::ok();
        }

        out << "[Resolve]\n"
            << "DNS=" << servers_str << "\n";
        if (!domains_str.empty()) out << "Domains=" << domains_str << "\n";
        out << "DNSSEC=" << (config.dnssec ? "yes" : "no") << "\n"
            << "DNSOverTLS=" << (config.dns_over_tls ? "yes" : "no") << "\n";
        out.close();
        run_cmd("systemctl restart systemd-resolved 2>/dev/null");
        return Result<void, std::string>::ok();
    }

    std::ofstream out("/etc/resolv.conf");
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write to /etc/resolv.conf");
    }
    out << "# Generated by straylight-network\n";
    for (const auto& domain : config.search_domains) out << "search " << domain << "\n";
    for (const auto& server : config.servers) out << "nameserver " << server << "\n";
    return Result<void, std::string>::ok();
}

Result<DnsConfig, std::string> NetworkManager::dns_get() const {
    DnsConfig config;

    auto res = run_cmd("resolvectl status 2>/dev/null");
    if (res.has_value()) {
        config.mode = "systemd-resolved";
        std::istringstream stream(res.value());
        std::string line;
        while (std::getline(stream, line)) {
            auto pos = line.find_first_not_of(" \t");
            if (pos != std::string::npos) line = line.substr(pos);

            if (line.rfind("DNS Servers:", 0) == 0 ||
                line.rfind("Current DNS Server:", 0) == 0) {
                std::regex ip_re(R"((\d+\.\d+\.\d+\.\d+|[0-9a-fA-F:]+))");
                auto it = std::sregex_iterator(line.begin(), line.end(), ip_re);
                for (; it != std::sregex_iterator(); ++it) {
                    std::string server = (*it)[1].str();
                    bool found = false;
                    for (const auto& s : config.servers)
                        if (s == server) { found = true; break; }
                    if (!found) config.servers.push_back(server);
                }
            } else if (line.rfind("DNS Domain:", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    std::istringstream ds(line.substr(colon + 1));
                    std::string domain;
                    while (ds >> domain) config.search_domains.push_back(domain);
                }
            } else if (line.find("DNSSEC") != std::string::npos) {
                config.dnssec = (line.find("yes") != std::string::npos);
            } else if (line.find("DNS over TLS") != std::string::npos) {
                config.dns_over_tls = (line.find("yes") != std::string::npos);
            }
        }
        return Result<DnsConfig, std::string>::ok(config);
    }

    config.mode = "resolv.conf";
    std::ifstream resolv("/etc/resolv.conf");
    if (!resolv.is_open()) {
        return Result<DnsConfig, std::string>::error("cannot read DNS configuration");
    }
    std::string line;
    while (std::getline(resolv, line)) {
        if (line.rfind("nameserver", 0) == 0) {
            auto pos = line.find_first_not_of(" \t", 10);
            if (pos != std::string::npos) config.servers.push_back(line.substr(pos));
        } else if (line.rfind("search", 0) == 0) {
            std::istringstream ds(line.substr(7));
            std::string domain;
            while (ds >> domain) config.search_domains.push_back(domain);
        }
    }
    return Result<DnsConfig, std::string>::ok(config);
}

// ===========================================================================
// Bond / Bridge
// ===========================================================================

Result<void, std::string> NetworkManager::bond_create(const BondConfig& config) {
    if (config.is_bridge) {
        auto res = run_cmd(
            "ip link add name " + config.name + " type bridge 2>/dev/null");
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "failed to create bridge: " + res.error());
        }
        for (const auto& member : config.members)
            run_cmd("ip link set " + member + " master " + config.name + " 2>/dev/null");
        if (!config.ip.empty())
            run_cmd("ip addr add " + config.ip + " dev " + config.name + " 2>/dev/null");
        run_cmd("ip link set " + config.name + " up 2>/dev/null");
    } else {
        std::string mode = config.mode.empty() ? "balance-rr" : config.mode;
        auto res = run_cmd(
            "ip link add " + config.name + " type bond mode " + mode + " 2>/dev/null");
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "failed to create bond: " + res.error());
        }
        for (const auto& member : config.members) {
            run_cmd("ip link set " + member + " down 2>/dev/null");
            run_cmd("ip link set " + member + " master " + config.name + " 2>/dev/null");
        }
        if (!config.ip.empty())
            run_cmd("ip addr add " + config.ip + " dev " + config.name + " 2>/dev/null");
        run_cmd("ip link set " + config.name + " up 2>/dev/null");
        for (const auto& member : config.members)
            run_cmd("ip link set " + member + " up 2>/dev/null");
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> NetworkManager::bond_destroy(const std::string& name) {
    run_cmd("ip link set " + name + " down 2>/dev/null");
    auto res = run_cmd("ip link delete " + name + " 2>/dev/null");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to destroy: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<std::vector<BondConfig>, std::string> NetworkManager::bond_list() const {
    std::vector<BondConfig> bonds;
    std::string net_dir = "/sys/class/net";
    if (!fs::exists(net_dir)) {
        return Result<std::vector<BondConfig>, std::string>::ok(bonds);
    }

    for (const auto& entry : fs::directory_iterator(net_dir)) {
        std::string name = entry.path().filename().string();
        BondConfig config;
        config.name = name;

        if (fs::exists(entry.path().string() + "/bonding")) {
            config.is_bridge = false;
            std::ifstream mode_f(entry.path().string() + "/bonding/mode");
            if (mode_f.is_open()) std::getline(mode_f, config.mode);
            std::ifstream slaves_f(entry.path().string() + "/bonding/slaves");
            if (slaves_f.is_open()) {
                std::string slaves;
                std::getline(slaves_f, slaves);
                std::istringstream ss(slaves);
                std::string member;
                while (ss >> member) config.members.push_back(member);
            }
            bonds.push_back(config);
        } else if (fs::exists(entry.path().string() + "/bridge")) {
            config.is_bridge = true;
            std::string brif = entry.path().string() + "/brif";
            if (fs::exists(brif)) {
                for (const auto& brentry : fs::directory_iterator(brif))
                    config.members.push_back(brentry.path().filename().string());
            }
            bonds.push_back(config);
        }
    }

    return Result<std::vector<BondConfig>, std::string>::ok(bonds);
}

// ===========================================================================
// Saved connections
// ===========================================================================

std::vector<SavedConnection> NetworkManager::saved_connections() const {
    std::vector<SavedConnection> connections;
    std::string dir = config_dir();
    if (!fs::exists(dir)) return connections;

    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.size() <= 5 || fname.substr(fname.size() - 5) != ".json") continue;

        std::ifstream f(entry.path().string());
        if (!f.is_open()) continue;
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        SavedConnection conn;
        conn.name = fname.substr(0, fname.size() - 5);

        std::regex type_re(R"("type"\s*:\s*"([^"]*)")");
        std::regex ssid_re(R"("ssid"\s*:\s*"([^"]*)")");
        std::regex auto_re(R"("auto_connect"\s*:\s*(true|false))");
        std::smatch m;
        if (std::regex_search(content, m, type_re)) conn.type = m[1].str();
        if (std::regex_search(content, m, ssid_re)) conn.ssid = m[1].str();
        if (std::regex_search(content, m, auto_re)) conn.auto_connect = (m[1].str() == "true");

        connections.push_back(conn);
    }

    return connections;
}

Result<void, std::string> NetworkManager::forget_connection(const std::string& name) {
    std::string path = config_dir() + "/" + name + ".json";
    if (fs::exists(path)) fs::remove(path);

    if (has_nmcli())
        run_cmd("nmcli connection delete '" + name + "' 2>/dev/null");

    return Result<void, std::string>::ok();
}

// ===========================================================================
// WiFi-extended private helpers
// ===========================================================================

std::string NetworkManager::find_wifi_interface() const {
    auto res = run_cmd("iw dev 2>/dev/null");
    if (res.has_value()) {
        std::regex iface_re(R"(Interface\s+(\S+))");
        std::smatch m;
        std::string out = res.value();
        if (std::regex_search(out, m, iface_re)) return m[1].str();
    }
    // Fallback: scan /sys/class/net
    if (fs::exists("/sys/class/net")) {
        for (const auto& entry : fs::directory_iterator("/sys/class/net")) {
            std::string name = entry.path().filename().string();
            if (fs::exists(entry.path().string() + "/wireless")) return name;
        }
    }
    return "wlan0";
}

int NetworkManager::freq_to_channel(int freq_mhz) const {
    if (freq_mhz >= 2412 && freq_mhz <= 2484) {
        if (freq_mhz == 2484) return 14;
        return (freq_mhz - 2407) / 5;
    }
    if (freq_mhz >= 5180 && freq_mhz <= 5825) return (freq_mhz - 5000) / 5;
    if (freq_mhz >= 5955 && freq_mhz <= 7115) return (freq_mhz - 5950) / 5;
    return 0;
}

std::string NetworkManager::wifi_signal_bar(int dbm) const {
    int quality;
    if (dbm >= -50) quality = 4;
    else if (dbm >= -60) quality = 3;
    else if (dbm >= -70) quality = 2;
    else if (dbm >= -80) quality = 1;
    else quality = 0;

    std::string bar = "[";
    for (int i = 0; i < 4; ++i) bar += (i < quality) ? "#" : ".";
    bar += "]";
    return bar;
}

void NetworkManager::save_wifi_profile(const std::string& ssid,
                                        const std::string& security) const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    // Store in the wifi sub-directory under config_dir's parent
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    std::string wifi_dir = std::string(home) + "/.config/straylight/wifi";
    fs::create_directories(wifi_dir);

    std::string path = wifi_dir + "/" + ssid + ".conf";
    std::ofstream out(path);
    if (out.is_open()) {
        out << "ssid=" << ssid << "\n"
            << "security=" << security << "\n"
            << "auto_connect=true\n"
            << "last_connected=" << buf << "\n"
            << "priority=0\n";
    }
}

// ===========================================================================
// WiFi-extended public API
// ===========================================================================

Result<std::vector<ChannelInfo>, std::string> NetworkManager::wifi_channels() const {
    // We reuse scan_wifi() and build channel-congestion data from it.
    // The internal scan returns WifiNetwork which carries channel + signal_strength.
    auto scan_res = scan_wifi();
    if (!scan_res.has_value()) {
        return Result<std::vector<ChannelInfo>, std::string>::error(scan_res.error());
    }

    const auto& networks = scan_res.value();
    std::map<int, std::vector<const WifiNetwork*>> by_channel;
    for (const auto& net : networks) {
        if (net.channel > 0) by_channel[net.channel].push_back(&net);
    }

    const std::vector<int> channels_24 = {1,2,3,4,5,6,7,8,9,10,11,12,13};
    const std::vector<int> channels_5  = {
        36,40,44,48,52,56,60,64,
        100,104,108,112,116,120,124,128,
        132,136,140,144,149,153,157,161,165
    };

    std::vector<ChannelInfo> result;

    auto process = [&](int ch, const std::string& band) {
        ChannelInfo info;
        info.channel = ch;
        info.band = band;
        info.frequency_mhz = (band == "2.4GHz") ? 2407 + ch * 5 : 5000 + ch * 5;

        auto it = by_channel.find(ch);
        if (it != by_channel.end()) {
            info.network_count = static_cast<int>(it->second.size());
            int total = 0;
            for (const auto* n : it->second) total += n->signal_strength;
            info.avg_signal_dbm = info.network_count > 0 ? total / info.network_count : 0;
        }

        if (band == "2.4GHz") {
            int overlap = 0;
            for (int adj = ch - 2; adj <= ch + 2; ++adj) {
                if (adj == ch) continue;
                auto ait = by_channel.find(adj);
                if (ait != by_channel.end())
                    overlap += static_cast<int>(ait->second.size());
            }
            info.interference_score =
                std::min(100, info.network_count * 20 + overlap * 10);
        } else {
            info.interference_score = std::min(100, info.network_count * 25);
        }

        result.push_back(info);
    };

    for (int ch : channels_24) process(ch, "2.4GHz");
    for (int ch : channels_5)  process(ch, "5GHz");

    return Result<std::vector<ChannelInfo>, std::string>::ok(result);
}

Result<SignalQuality, std::string> NetworkManager::wifi_signal() const {
    std::string iface = find_wifi_interface();
    SignalQuality sq;

    auto link_res = run_cmd("iw dev " + iface + " link 2>/dev/null");
    if (!link_res.has_value() ||
        link_res.value().find("Not connected") != std::string::npos) {
        return Result<SignalQuality, std::string>::error(
            "not connected to any WiFi network");
    }

    std::string info = link_res.value();
    std::smatch m;

    std::regex ssid_re(R"(SSID:\s+(.+))");
    if (std::regex_search(info, m, ssid_re)) sq.ssid = m[1].str();

    std::regex signal_re(R"(signal:\s+(-?\d+)\s+dBm)");
    if (std::regex_search(info, m, signal_re)) sq.signal_dbm = std::stoi(m[1].str());

    std::regex freq_re(R"(freq:\s+(\d+))");
    if (std::regex_search(info, m, freq_re)) {
        sq.frequency_mhz = std::stoi(m[1].str());
        sq.channel = freq_to_channel(sq.frequency_mhz);
    }

    std::regex tx_re(R"(tx bitrate:\s+([\d.]+)\s+MBit)");
    if (std::regex_search(info, m, tx_re)) sq.tx_rate_mbps = std::stod(m[1].str());

    std::regex rx_re(R"(rx bitrate:\s+([\d.]+)\s+MBit)");
    if (std::regex_search(info, m, rx_re)) sq.rx_rate_mbps = std::stod(m[1].str());

    auto survey_res = run_cmd("iw dev " + iface + " survey dump 2>/dev/null");
    if (survey_res.has_value()) {
        std::regex noise_re(R"(noise:\s+(-?\d+)\s+dBm)");
        std::string survey = survey_res.value();
        if (std::regex_search(survey, m, noise_re))
            sq.noise_dbm = std::stoi(m[1].str());
    }

    if (sq.signal_dbm >= -50) sq.link_quality = 100;
    else if (sq.signal_dbm <= -100) sq.link_quality = 0;
    else sq.link_quality = 2 * (sq.signal_dbm + 100);

    return Result<SignalQuality, std::string>::ok(sq);
}

Result<std::string, std::string> NetworkManager::wifi_qr(const std::string& ssid) const {
    std::string security = "WPA";
    std::string password;

    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    std::string conf_path =
        std::string(home) + "/.config/straylight/wifi/" + ssid + ".conf";

    if (fs::exists(conf_path)) {
        std::ifstream f(conf_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("security=", 0) == 0) security = line.substr(9);
            if (line.rfind("password=", 0) == 0) password = line.substr(9);
        }
    }

    if (password.empty() && has_nmcli()) {
        auto res = run_cmd("nmcli -s -g 802-11-wireless-security.psk connection show '"
                           + ssid + "' 2>/dev/null");
        if (res.has_value()) {
            password = res.value();
            if (!password.empty() && password.back() == '\n') password.pop_back();
        }
    }

    std::string wifi_str = "WIFI:T:" + security + ";S:" + ssid + ";";
    if (!password.empty()) wifi_str += "P:" + password + ";";
    wifi_str += ";";

    auto qr_res = run_cmd(
        "echo -n '" + wifi_str + "' | qrencode -t UTF8 2>/dev/null");
    if (qr_res.has_value() && !qr_res.value().empty()) {
        return Result<std::string, std::string>::ok(qr_res.value());
    }

    std::ostringstream out;
    out << "WiFi Network QR Data:\n"
        << "  SSID:     " << ssid << "\n"
        << "  Security: " << security << "\n"
        << "  String:   " << wifi_str << "\n"
        << "\n"
        << "  (Install 'qrencode' for visual QR code)\n"
        << "\n"
        << "  Manual connect:\n"
        << "    straylight-network wifi connect " << ssid << "\n";
    return Result<std::string, std::string>::ok(out.str());
}

Result<std::vector<SavedWifi>, std::string> NetworkManager::saved_wifi() const {
    std::vector<SavedWifi> networks;

    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    std::string dir = std::string(home) + "/.config/straylight/wifi";

    if (fs::exists(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            std::string fname = entry.path().filename().string();
            if (fname.size() <= 5 || fname.substr(fname.size() - 5) != ".conf") continue;

            std::ifstream f(entry.path().string());
            if (!f.is_open()) continue;

            SavedWifi wifi;
            wifi.ssid = fname.substr(0, fname.size() - 5);

            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("security=", 0) == 0) wifi.security = line.substr(9);
                else if (line.rfind("auto_connect=", 0) == 0)
                    wifi.auto_connect = (line.substr(13) == "true");
                else if (line.rfind("last_connected=", 0) == 0)
                    wifi.last_connected = line.substr(15);
                else if (line.rfind("priority=", 0) == 0) {
                    try { wifi.priority = std::stoi(line.substr(9)); } catch (...) {}
                }
            }
            networks.push_back(wifi);
        }
    }

    // Also pull from nmcli
    if (has_nmcli()) {
        auto res = run_cmd("nmcli -t -f NAME,TYPE connection show 2>/dev/null");
        if (res.has_value()) {
            std::istringstream stream(res.value());
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("802-11-wireless") != std::string::npos) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string name = line.substr(0, colon);
                        bool already = false;
                        for (const auto& w : networks)
                            if (w.ssid == name) { already = true; break; }
                        if (!already) {
                            SavedWifi wifi;
                            wifi.ssid = name;
                            wifi.security = "WPA2";
                            networks.push_back(wifi);
                        }
                    }
                }
            }
        }
    }

    return Result<std::vector<SavedWifi>, std::string>::ok(networks);
}

// ===========================================================================
// Probe/scanner static helpers
// ===========================================================================

std::string NetworkManager::guess_os_from_ttl(int ttl) {
    if (ttl <= 0)   return "unknown";
    if (ttl <= 64)  return "Linux/macOS/Unix";
    if (ttl <= 128) return "Windows";
    if (ttl <= 255) return "Network device/Solaris";
    return "unknown";
}

std::string NetworkManager::lookup_mac_vendor(const std::string& mac) {
    static const std::map<std::string, std::string> oui_table = {
        {"00:50:56", "VMware"},     {"00:0C:29", "VMware"},
        {"08:00:27", "VirtualBox"}, {"52:54:00", "QEMU/KVM"},
        {"DC:A6:32", "Raspberry Pi"}, {"B8:27:EB", "Raspberry Pi"},
        {"E4:5F:01", "Raspberry Pi"}, {"00:1A:79", "Dell"},
        {"F8:BC:12", "Dell"},       {"00:25:B5", "Dell"},
        {"3C:D9:2B", "HP"},         {"00:17:A4", "HP"},
        {"9C:B6:D0", "HP"},         {"00:1E:67", "Intel"},
        {"00:1B:21", "Intel"},      {"A4:BF:01", "Intel"},
        {"00:1C:42", "Parallels"},  {"AC:DE:48", "Apple"},
        {"00:1B:63", "Apple"},      {"F0:18:98", "Apple"},
        {"3C:22:FB", "Apple"},      {"14:7D:DA", "Apple"},
        {"A8:66:7F", "Apple"},      {"00:23:14", "Apple"},
        {"10:DD:B1", "Apple"},      {"7C:D1:C3", "Apple"},
        {"78:4F:43", "Apple"},      {"2C:F0:A2", "TP-Link"},
        {"50:C7:BF", "TP-Link"},    {"B0:4E:26", "TP-Link"},
        {"00:14:6C", "Netgear"},    {"20:E5:2A", "Netgear"},
        {"C4:04:15", "Netgear"},    {"00:18:E7", "Linksys"},
        {"C0:56:27", "Linksys"},    {"00:26:F2", "Netgear"},
        {"00:1A:2F", "Cisco"},      {"00:1E:14", "Cisco"},
        {"00:50:F1", "NVIDIA"},     {"48:B0:2D", "NVIDIA"},
    };

    if (mac.size() < 8) return "unknown";
    std::string prefix = mac.substr(0, 8);
    for (auto& c : prefix) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    auto it = oui_table.find(prefix);
    return (it != oui_table.end()) ? it->second : "unknown";
}

std::string NetworkManager::service_name(uint16_t port) {
    static const std::map<uint16_t, std::string> services = {
        {20,"ftp-data"},{21,"ftp"},{22,"ssh"},{23,"telnet"},
        {25,"smtp"},{53,"dns"},{67,"dhcp"},{68,"dhcp"},
        {69,"tftp"},{80,"http"},{110,"pop3"},{111,"rpcbind"},
        {123,"ntp"},{135,"msrpc"},{137,"netbios-ns"},{138,"netbios-dgm"},
        {139,"netbios-ssn"},{143,"imap"},{161,"snmp"},{162,"snmptrap"},
        {179,"bgp"},{389,"ldap"},{443,"https"},{445,"microsoft-ds"},
        {465,"smtps"},{514,"syslog"},{515,"printer"},{543,"klogin"},
        {544,"kshell"},{548,"afp"},{554,"rtsp"},{587,"submission"},
        {631,"ipp"},{636,"ldaps"},{873,"rsync"},{902,"vmware"},
        {993,"imaps"},{995,"pop3s"},{1080,"socks"},{1433,"mssql"},
        {1434,"mssql-m"},{1521,"oracle"},{1723,"pptp"},{2049,"nfs"},
        {2082,"cpanel"},{2083,"cpanel-ssl"},{2181,"zookeeper"},
        {3306,"mysql"},{3389,"rdp"},{3690,"svn"},
        {4443,"pharos"},{5060,"sip"},{5061,"sip-tls"},
        {5201,"iperf"},{5432,"postgresql"},{5900,"vnc"},
        {5984,"couchdb"},{6379,"redis"},{6443,"kubernetes"},
        {8080,"http-proxy"},{8443,"https-alt"},{8888,"http-alt"},
        {9090,"prometheus"},{9200,"elasticsearch"},{9300,"elasticsearch"},
        {11211,"memcached"},{27017,"mongodb"},{50000,"db2"},
    };
    auto it = services.find(port);
    return (it != services.end()) ? it->second : "unknown";
}

std::vector<uint16_t> NetworkManager::default_ports() {
    return {
        21,22,23,25,53,80,110,111,135,139,143,443,445,993,995,
        1433,1521,2049,3306,3389,5432,5900,6379,6443,8080,8443,
        9090,9200,27017
    };
}

Result<std::pair<uint32_t, uint32_t>, std::string>
NetworkManager::parse_cidr(const std::string& cidr) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        return Result<std::pair<uint32_t, uint32_t>, std::string>::error(
            "Invalid CIDR: missing /prefix");
    }

    std::string ip_str = cidr.substr(0, slash);
    int prefix = 0;
    try {
        prefix = std::stoi(cidr.substr(slash + 1));
    } catch (...) {
        return Result<std::pair<uint32_t, uint32_t>, std::string>::error(
            "Invalid CIDR prefix");
    }

    if (prefix < 0 || prefix > 32) {
        return Result<std::pair<uint32_t, uint32_t>, std::string>::error(
            "CIDR prefix out of range");
    }

    auto ip_r = string_to_ip(ip_str);
    if (!ip_r.has_value()) {
        return Result<std::pair<uint32_t, uint32_t>, std::string>::error(ip_r.error());
    }

    uint32_t base = ip_r.value();
    uint32_t mask = (prefix == 0) ? 0 : (~uint32_t(0) << (32 - prefix));
    base &= mask;
    uint32_t count = (prefix == 32) ? 1 : (uint32_t(1) << (32 - prefix));

    return Result<std::pair<uint32_t, uint32_t>, std::string>::ok({base, count});
}

std::string NetworkManager::ip_to_string(uint32_t ip) {
    struct in_addr addr{};
    addr.s_addr = htonl(ip);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return buf;
}

Result<uint32_t, std::string> NetworkManager::string_to_ip(const std::string& ip) {
    struct in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        return Result<uint32_t, std::string>::error("Invalid IP address: " + ip);
    }
    return Result<uint32_t, std::string>::ok(ntohl(addr.s_addr));
}

// ===========================================================================
// Probe — subnet detection
// ===========================================================================

Result<std::string, std::string> NetworkManager::detect_subnet() const {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return Result<std::string, std::string>::error(
            std::string("getifaddrs failed: ") + strerror(errno));
    }

    struct IfGuard {
        struct ifaddrs* p;
        ~IfGuard() { freeifaddrs(p); }
    } guard{ifaddr};

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        std::string name = ifa->ifa_name;
        if (name == "lo" || name == "lo0") continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (!(ifa->ifa_flags & IFF_RUNNING)) continue;

        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        auto* mask_sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_netmask);

        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        uint32_t mask = ntohl(mask_sin->sin_addr.s_addr);
        uint32_t network = ip & mask;

        int prefix = 0;
        uint32_t m = mask;
        while (m & 0x80000000) { ++prefix; m <<= 1; }

        return Result<std::string, std::string>::ok(
            ip_to_string(network) + "/" + std::to_string(prefix));
    }

    return Result<std::string, std::string>::error(
        "No suitable network interface found");
}

// ===========================================================================
// Probe — ping
// ===========================================================================

Result<DiscoveredHost, std::string> NetworkManager::ping_host(
    const std::string& host) const
{
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (gai != 0) {
        return Result<DiscoveredHost, std::string>::error(
            std::string("Cannot resolve ") + host + ": " + gai_strerror(gai));
    }

    struct AddrGuard {
        struct addrinfo* p;
        ~AddrGuard() { freeaddrinfo(p); }
    } ag{res};

    auto* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));

    DiscoveredHost dh;
    dh.ip = ip_buf;
    dh.hostname = host;

    uint16_t probe_ports[] = {80, 443, 22};
    for (uint16_t port : probe_ports) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr = sin->sin_addr;

        auto start = std::chrono::steady_clock::now();
        int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));

        if (rc == 0) {
            auto end = std::chrono::steady_clock::now();
            dh.rtt_ms = std::chrono::duration<double, std::milli>(end - start).count();
            dh.alive = true;
            close(sock);
            break;
        }

        if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, 2000);
            auto end = std::chrono::steady_clock::now();

            if (pr > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0 || so_error == ECONNREFUSED) {
                    dh.rtt_ms =
                        std::chrono::duration<double, std::milli>(end - start).count();
                    dh.alive = true;
                    close(sock);
                    break;
                }
            }
        }
        close(sock);
    }

    if (dh.alive) {
        char hbuf[NI_MAXHOST];
        struct sockaddr_in rev_sa{};
        rev_sa.sin_family = AF_INET;
        rev_sa.sin_addr = sin->sin_addr;
        if (getnameinfo(reinterpret_cast<struct sockaddr*>(&rev_sa), sizeof(rev_sa),
                        hbuf, sizeof(hbuf), nullptr, 0, NI_NAMEREQD) == 0) {
            dh.hostname = hbuf;
        }
    }

    if (!dh.alive) {
        return Result<DiscoveredHost, std::string>::error(
            "Host " + host + " is not responding");
    }

    return Result<DiscoveredHost, std::string>::ok(std::move(dh));
}

// ===========================================================================
// Probe — subnet scan
// ===========================================================================

Result<std::vector<DiscoveredHost>, std::string> NetworkManager::scan_subnet(
    const std::string& subnet) const
{
    auto cidr_r = parse_cidr(subnet);
    if (!cidr_r.has_value()) {
        return Result<std::vector<DiscoveredHost>, std::string>::error(cidr_r.error());
    }

    auto [base, count] = cidr_r.value();

    if (count > 4096) {
        return Result<std::vector<DiscoveredHost>, std::string>::error(
            "Subnet too large (max /20 = 4096 hosts). Specify a smaller range.");
    }

    std::vector<DiscoveredHost> results;
    uint32_t start = base + 1;
    uint32_t end   = base + count - 1;

    for (uint32_t ip = start; ip < end; ++ip) {
        std::string ip_str = ip_to_string(ip);

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(80);
        inet_pton(AF_INET, ip_str.c_str(), &sa.sin_addr);

        auto probe_start = std::chrono::steady_clock::now();
        int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));

        bool alive = false;
        double rtt  = 0.0;

        if (rc == 0) {
            auto now = std::chrono::steady_clock::now();
            rtt   = std::chrono::duration<double, std::milli>(now - probe_start).count();
            alive = true;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLOUT;
            int pr = poll(&pfd, 1, 200);
            if (pr > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0 || so_error == ECONNREFUSED) {
                    auto now = std::chrono::steady_clock::now();
                    rtt   = std::chrono::duration<double, std::milli>(now - probe_start).count();
                    alive = true;
                }
            }
        }
        close(sock);

        if (alive) {
            DiscoveredHost dh;
            dh.ip    = ip_str;
            dh.alive = true;
            dh.rtt_ms   = rtt;
            dh.os_guess = "unknown";

            char hbuf[NI_MAXHOST];
            struct sockaddr_in rev{};
            rev.sin_family = AF_INET;
            inet_pton(AF_INET, ip_str.c_str(), &rev.sin_addr);
            if (getnameinfo(reinterpret_cast<struct sockaddr*>(&rev), sizeof(rev),
                            hbuf, sizeof(hbuf), nullptr, 0, NI_NAMEREQD) == 0) {
                dh.hostname = hbuf;
            }
            results.push_back(std::move(dh));
        }
    }

    return Result<std::vector<DiscoveredHost>, std::string>::ok(std::move(results));
}

// ===========================================================================
// Probe — port scan
// ===========================================================================

Result<std::vector<PortResult>, std::string> NetworkManager::scan_ports(
    const std::string& host,
    const std::vector<uint16_t>& ports) const
{
    const auto& target_ports = ports.empty() ? default_ports() : ports;

    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (gai != 0) {
        return Result<std::vector<PortResult>, std::string>::error(
            std::string("Cannot resolve ") + host + ": " + gai_strerror(gai));
    }

    struct AddrGuard {
        struct addrinfo* p;
        ~AddrGuard() { freeaddrinfo(p); }
    } ag{res};

    auto* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    std::vector<PortResult> results;

    for (uint16_t port : target_ports) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(port);
        sa.sin_addr   = sin->sin_addr;

        auto start = std::chrono::steady_clock::now();
        int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa));

        PortResult pr;
        pr.port    = port;
        pr.service = service_name(port);

        if (rc == 0) {
            auto end = std::chrono::steady_clock::now();
            pr.state  = "open";
            pr.rtt_ms = std::chrono::duration<double, std::milli>(end - start).count();
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd     = sock;
            pfd.events = POLLOUT;
            int poll_rc = poll(&pfd, 1, 1000);
            auto end    = std::chrono::steady_clock::now();

            if (poll_rc > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) {
                    pr.state  = "open";
                    pr.rtt_ms = std::chrono::duration<double, std::milli>(end - start).count();
                } else if (so_error == ECONNREFUSED) {
                    pr.state  = "closed";
                    pr.rtt_ms = std::chrono::duration<double, std::milli>(end - start).count();
                } else {
                    pr.state = "filtered";
                }
            } else {
                pr.state = "filtered";
            }
        } else if (errno == ECONNREFUSED) {
            pr.state = "closed";
        } else {
            pr.state = "filtered";
        }

        close(sock);

        if (pr.state == "open") results.push_back(std::move(pr));
    }

    return Result<std::vector<PortResult>, std::string>::ok(std::move(results));
}

// ===========================================================================
// Probe — traceroute
// ===========================================================================

Result<std::vector<TraceHop>, std::string> NetworkManager::traceroute(
    const std::string& host, int max_hops) const
{
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (gai != 0) {
        return Result<std::vector<TraceHop>, std::string>::error(
            std::string("Cannot resolve ") + host + ": " + gai_strerror(gai));
    }

    struct AddrGuard {
        struct addrinfo* p;
        ~AddrGuard() { freeaddrinfo(p); }
    } ag{res};

    auto* target_sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    uint32_t target_ip = target_sin->sin_addr.s_addr;

    std::vector<TraceHop> hops;
    uint16_t dest_port = 33434;

    for (int ttl = 1; ttl <= max_hops; ++ttl) {
        int send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (send_sock < 0) {
            return Result<std::vector<TraceHop>, std::string>::error(
                std::string("socket() failed: ") + strerror(errno));
        }

        if (setsockopt(send_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            close(send_sock);
            continue;
        }

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port   = htons(static_cast<uint16_t>(dest_port + ttl));
        dest.sin_addr   = target_sin->sin_addr;

        auto start = std::chrono::steady_clock::now();

        char probe_data[] = "STRAYLIGHT";
        ssize_t sent = sendto(send_sock, probe_data, sizeof(probe_data), 0,
                              reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
        close(send_sock);

        TraceHop hop;
        hop.hop = ttl;

        if (sent < 0) {
            hop.timeout = true;
            hops.push_back(hop);
            continue;
        }

        // Try raw ICMP receive (requires elevated privileges on Linux)
        int recv_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (recv_sock >= 0) {
            struct pollfd pfd{};
            pfd.fd     = recv_sock;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 2000);

            if (pr > 0) {
                char buf[512];
                struct sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                ssize_t n = recvfrom(recv_sock, buf, sizeof(buf), 0,
                                     reinterpret_cast<struct sockaddr*>(&from), &from_len);

                auto end = std::chrono::steady_clock::now();
                hop.rtt_ms = std::chrono::duration<double, std::milli>(end - start).count();

                if (n > 0) {
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
                    hop.ip = ip_str;

                    char hbuf[NI_MAXHOST];
                    if (getnameinfo(reinterpret_cast<struct sockaddr*>(&from), from_len,
                                    hbuf, sizeof(hbuf), nullptr, 0, NI_NAMEREQD) == 0) {
                        hop.hostname = hbuf;
                    }
                }
            } else {
                hop.timeout = true;
            }
            close(recv_sock);
        } else {
            hop.timeout = true;
        }

        hops.push_back(hop);

        if (!hop.ip.empty()) {
            struct in_addr hop_addr{};
            inet_pton(AF_INET, hop.ip.c_str(), &hop_addr);
            if (hop_addr.s_addr == target_ip) break;
        }
    }

    return Result<std::vector<TraceHop>, std::string>::ok(std::move(hops));
}

// ===========================================================================
// Probe — DNS lookup
// ===========================================================================

Result<std::vector<DnsRecord>, std::string> NetworkManager::dns_lookup(
    const std::string& domain,
    const std::string& type) const
{
    std::vector<DnsRecord> records;

    bool want_a    = (type == "ANY" || type == "A");
    bool want_aaaa = (type == "ANY" || type == "AAAA");

    if (want_a) {
        struct addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) == 0) {
            for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
                auto* sin = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
                DnsRecord rec;
                rec.type  = "A";
                rec.name  = domain;
                rec.value = buf;
                records.push_back(std::move(rec));
            }
            freeaddrinfo(res);
        }
    }

    if (want_aaaa) {
        struct addrinfo hints{};
        hints.ai_family   = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(domain.c_str(), nullptr, &hints, &res) == 0) {
            for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
                auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(rp->ai_addr);
                char buf[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
                DnsRecord rec;
                rec.type  = "AAAA";
                rec.name  = domain;
                rec.value = buf;
                records.push_back(std::move(rec));
            }
            freeaddrinfo(res);
        }
    }

    if (records.empty()) {
        return Result<std::vector<DnsRecord>, std::string>::error(
            "No DNS records found for " + domain);
    }

    return Result<std::vector<DnsRecord>, std::string>::ok(std::move(records));
}

// ===========================================================================
// Probe — bandwidth test
// ===========================================================================

Result<double, std::string> NetworkManager::bandwidth_test(
    const std::string& host, uint16_t port) const
{
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0) {
        return Result<double, std::string>::error(
            std::string("Cannot resolve ") + host + ": " + gai_strerror(gai));
    }

    struct AddrGuard {
        struct addrinfo* p;
        ~AddrGuard() { freeaddrinfo(p); }
    } ag{res};

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        return Result<double, std::string>::error(
            std::string("socket() failed: ") + strerror(errno));
    }

    int sndbuf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        return Result<double, std::string>::error(
            std::string("connect() failed: ") + strerror(errno) +
            " — is an iperf3 server running on " + host + ":" + port_str + "?");
    }

    static constexpr size_t kBufSize = 128 * 1024;
    std::vector<char> buf(kBufSize, 'X');

    auto start    = std::chrono::steady_clock::now();
    size_t total  = 0;
    auto deadline = start + std::chrono::seconds(3);

    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = write(sock, buf.data(), buf.size());
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(end - start).count();
    close(sock);

    if (total == 0 || elapsed_s < 0.001) {
        return Result<double, std::string>::error("Bandwidth test produced no data");
    }

    double mbps = (static_cast<double>(total) * 8.0) / (elapsed_s * 1e6);
    return Result<double, std::string>::ok(mbps);
}

// ===========================================================================
// Probe — health check
// ===========================================================================

Result<std::vector<HealthCheck>, std::string> NetworkManager::health_check() const {
    std::vector<HealthCheck> checks;

    // 1. Gateway
    {
        HealthCheck hc;
        hc.name = "Gateway";
        std::string gateway_ip;

#ifdef __APPLE__
        FILE* pipe = popen(
            "route -n get default 2>/dev/null | grep gateway", "r");
#else
        FILE* pipe = popen(
            "ip route show default 2>/dev/null | awk '/default/ {print $3}'", "r");
#endif
        if (pipe) {
            char buf[256];
            if (fgets(buf, sizeof(buf), pipe)) {
                std::string line = buf;
                auto pos = line.find_first_of("0123456789");
                if (pos != std::string::npos) {
                    auto ep = line.find_first_not_of("0123456789.", pos);
                    gateway_ip = line.substr(pos, ep - pos);
                }
            }
            pclose(pipe);
        }

        if (gateway_ip.empty()) {
            hc.passed = false;
            hc.detail = "No default gateway found";
        } else {
            auto ping_r = ping_host(gateway_ip);
            if (ping_r.has_value()) {
                hc.passed    = true;
                hc.latency_ms = ping_r.value().rtt_ms;
                hc.detail = "Gateway " + gateway_ip + " reachable (" +
                            std::to_string(static_cast<int>(hc.latency_ms)) + "ms)";
            } else {
                hc.passed = false;
                hc.detail = "Gateway " + gateway_ip + " unreachable";
            }
        }
        checks.push_back(std::move(hc));
    }

    // 2. DNS
    {
        HealthCheck hc;
        hc.name = "DNS";
        auto t0  = std::chrono::steady_clock::now();
        auto dns_r = dns_lookup("google.com", "A");
        auto t1  = std::chrono::steady_clock::now();
        hc.latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (dns_r.has_value() && !dns_r.value().empty()) {
            hc.passed = true;
            hc.detail = "DNS resolving correctly (" +
                        std::to_string(static_cast<int>(hc.latency_ms)) + "ms)";
        } else {
            hc.passed = false;
            hc.detail = "DNS resolution failed";
        }
        checks.push_back(std::move(hc));
    }

    // 3. Internet
    {
        HealthCheck hc;
        hc.name = "Internet";
        const std::string test_hosts[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};
        bool reached = false;
        for (const auto& th : test_hosts) {
            auto ping_r = ping_host(th);
            if (ping_r.has_value()) {
                hc.passed     = true;
                hc.latency_ms = ping_r.value().rtt_ms;
                hc.detail = "Internet reachable via " + th + " (" +
                            std::to_string(static_cast<int>(hc.latency_ms)) + "ms)";
                reached = true;
                break;
            }
        }
        if (!reached) {
            hc.passed = false;
            hc.detail = "Cannot reach any public DNS servers";
        }
        checks.push_back(std::move(hc));
    }

    // 4. Packet loss (5-ping test)
    {
        HealthCheck hc;
        hc.name = "Packet Loss";
        int success = 0;
        int total   = 5;
        for (int i = 0; i < total; ++i) {
            if (ping_host("8.8.8.8").has_value()) ++success;
        }
        double loss_pct = 100.0 * (1.0 - static_cast<double>(success) / total);
        hc.passed = (loss_pct < 20.0);
        std::ostringstream oss;
        oss << std::fixed;
        oss.precision(0);
        oss << loss_pct << "% packet loss (" << success << "/" << total
            << " probes succeeded)";
        hc.detail = oss.str();
        checks.push_back(std::move(hc));
    }

    return Result<std::vector<HealthCheck>, std::string>::ok(std::move(checks));
}

} // namespace straylight
