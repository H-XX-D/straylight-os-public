/**
 * straylight-bridge — Cross-machine shared memory daemon.
 *
 * Manages bridge connections allowing processes on different nodes
 * to share memory regions transparently. Handles page fault tracking,
 * delta compression, and network sync.
 *
 * Usage:
 *   straylight-bridge [--port <port>] [--foreground]
 */

#include "bridge_engine.h"
#include "straylight/daemon_base.h"
#include "straylight/result.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace straylight::bridge {

class BridgeDaemon : public DaemonBase {
public:
    BridgeDaemon(int argc, char** argv)
        : DaemonBase("straylight-bridge")
    {
        parse_bridge_args(argc, argv);
    }

protected:
    VoidResult<> init() override {
        fprintf(stdout, "[straylight-bridge] initializing on port %d\n", listen_port_);

        engine_.set_default_port(listen_port_);

        // Start background sync thread for batched/immediate bridges.
        engine_.start_sync_thread();

        // Create control directory for CLI communication.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories("/run/straylight/bridge", ec);

        set_tick_interval_ms(1000);
        return VoidResult<>::ok();
    }

    void tick() override {
        // Check for CLI commands via control file.
        process_control_commands();

        // Periodic stats reporting.
        ++tick_count_;
        if (tick_count_ % 10 == 0) {
            auto bridges = engine_.list_bridges();
            for (const auto& b : bridges) {
                auto stats_result = engine_.get_stats(b.id);
                if (stats_result) {
                    auto& s = stats_result.value();
                    fprintf(stdout, "[straylight-bridge] bridge %u (%s): "
                            "syncs=%llu pages=%llu connected=%s\n",
                            s.id, s.region_name.c_str(),
                            static_cast<unsigned long long>(s.total_syncs),
                            static_cast<unsigned long long>(s.total_pages_synced),
                            s.connected ? "yes" : "no");
                }
            }
        }
    }

    void shutdown() override {
        fprintf(stdout, "[straylight-bridge] shutting down, destroying all bridges\n");
        engine_.stop_sync_thread();

        auto bridges = engine_.list_bridges();
        for (const auto& b : bridges) {
            (void)engine_.destroy_bridge(b.id);
        }
    }

private:
    BridgeEngine engine_;
    uint16_t listen_port_ = 9901;
    uint64_t tick_count_ = 0;

    void parse_bridge_args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc) {
                listen_port_ = static_cast<uint16_t>(std::stoi(argv[++i]));
            }
        }
    }

    void process_control_commands() {
        const std::string cmd_path = "/run/straylight/bridge/command";
        namespace fs = std::filesystem;

        if (!fs::exists(cmd_path)) return;

        std::ifstream cf(cmd_path);
        if (!cf) return;

        std::string line;
        while (std::getline(cf, line)) {
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            if (cmd == "create") {
                std::string host, name;
                size_t size = 0;
                std::string mode_str = "batched";
                iss >> host >> name >> size >> mode_str;

                if (!host.empty() && !name.empty() && size > 0) {
                    SyncMode mode = sync_mode_from_string(mode_str);
                    auto result = engine_.create_bridge(host, name, size, mode);
                    if (result) {
                        write_response("created bridge " + std::to_string(result.value()) +
                                       ": " + name);
                    } else {
                        write_response("error: " + result.err());
                    }
                }
            } else if (cmd == "destroy") {
                uint32_t id = 0;
                iss >> id;
                auto result = engine_.destroy_bridge(id);
                if (result) {
                    write_response("destroyed bridge " + std::to_string(id));
                } else {
                    write_response("error: " + result.err());
                }
            } else if (cmd == "sync") {
                uint32_t id = 0;
                iss >> id;
                auto result = engine_.sync_bridge(id);
                if (result) {
                    write_response("synced bridge " + std::to_string(id));
                } else {
                    write_response("error: " + result.err());
                }
            } else if (cmd == "list") {
                auto bridges = engine_.list_bridges();
                std::ostringstream oss;
                for (const auto& b : bridges) {
                    oss << b.id << " " << b.region_name << " "
                        << b.remote_host << ":" << b.remote_port << " "
                        << b.size << " " << sync_mode_to_string(b.sync_mode) << "\n";
                }
                write_response(oss.str());
            } else if (cmd == "stats") {
                uint32_t id = 0;
                iss >> id;
                auto result = engine_.get_stats(id);
                if (result) {
                    auto& s = result.value();
                    std::ostringstream oss;
                    oss << "id=" << s.id << " name=" << s.region_name
                        << " size=" << s.region_size
                        << " syncs=" << s.total_syncs
                        << " pages=" << s.total_pages_synced
                        << " bytes=" << s.total_bytes_synced
                        << " dirty=" << s.dirty_pages_current
                        << " latency=" << s.avg_sync_latency_ms << "ms"
                        << " connected=" << (s.connected ? "yes" : "no")
                        << " uptime=" << s.uptime_seconds << "s";
                    write_response(oss.str());
                } else {
                    write_response("error: " + result.err());
                }
            }
        }

        cf.close();
        // Remove command file after processing.
        fs::remove(cmd_path);
    }

    void write_response(const std::string& msg) {
        const std::string resp_path = "/run/straylight/bridge/response";
        std::ofstream rf(resp_path, std::ios::app);
        if (rf) rf << msg << "\n";
    }
};

} // namespace straylight::bridge

int main(int argc, char** argv) {
    straylight::bridge::BridgeDaemon daemon(argc, argv);
    return daemon.run();
}
