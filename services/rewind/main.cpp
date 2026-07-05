/**
 * StrayLight Rewind — Daemon
 *
 * Checkpoint/restore daemon for any process. Manages checkpoint sessions,
 * auto-checkpoints tracked processes on a configurable interval, and
 * handles restore requests.
 *
 * Usage:
 *   straylight-rewind                    # run as daemon
 *   straylight-rewind --config /path     # custom config
 */

#include "checkpoint_engine.h"
#include "checkpoint_store.h"
#include "process_monitor.h"
#include "straylight/daemon_base.h"
#include "straylight/result.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace straylight::rewind {

// ── Configuration parser ────────────────────────────────────────────

struct RewindConfig {
    int    checkpoint_interval_s        = 30;
    size_t max_checkpoints_per_process  = 20;
    uint64_t max_storage_mb             = 2048;
    bool   compression                  = true;
    bool   delta_checkpoints            = true;
    bool   auto_track_services          = true;
    std::string store_path              = "/var/lib/straylight/rewind";
    std::string config_path             = "/etc/straylight/rewind.conf";
};

RewindConfig load_config(const std::string& path) {
    RewindConfig cfg;
    std::ifstream in(path);
    if (!in) return cfg;

    std::string line;
    while (std::getline(in, line)) {
        // Strip comments
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            auto f = s.find_first_not_of(" \t\r\n");
            auto l = s.find_last_not_of(" \t\r\n");
            if (f == std::string::npos) { s.clear(); return; }
            s = s.substr(f, l - f + 1);
        };
        trim(key);
        trim(val);

        if (key == "checkpoint_interval_s")
            cfg.checkpoint_interval_s = std::stoi(val);
        else if (key == "max_checkpoints_per_process")
            cfg.max_checkpoints_per_process = std::stoull(val);
        else if (key == "max_storage_mb")
            cfg.max_storage_mb = std::stoull(val);
        else if (key == "compression")
            cfg.compression = (val == "zstd" || val == "true" || val == "1");
        else if (key == "delta_checkpoints")
            cfg.delta_checkpoints = (val == "true" || val == "1");
        else if (key == "auto_track_services")
            cfg.auto_track_services = (val == "true" || val == "1");
        else if (key == "store_path")
            cfg.store_path = val;
    }

    return cfg;
}

// ── Daemon ──────────────────────────────────────────────────────────

class RewindDaemon : public DaemonBase {
public:
    explicit RewindDaemon(RewindConfig config)
        : DaemonBase("straylight-rewind")
        , config_(std::move(config))
        , store_({
            .base_path = config_.store_path,
            .max_checkpoints_per_process = config_.max_checkpoints_per_process,
            .max_storage_mb = config_.max_storage_mb,
            .compression_enabled = config_.compression,
            .delta_checkpoints = config_.delta_checkpoints,
          })
        , engine_(store_, {
            .checkpoint_interval_s = config_.checkpoint_interval_s,
            .delta_checkpoints = config_.delta_checkpoints,
          })
        , monitor_(engine_)
    {}

protected:
    VoidResult<> init() override {
        auto r = store_.initialize();
        if (!r) return r;

        set_tick_interval_ms(1000); // 1-second tick

        fprintf(stdout, "[rewind] store: %s\n", config_.store_path.c_str());
        fprintf(stdout, "[rewind] interval: %ds, max per process: %zu, budget: %luMB\n",
                config_.checkpoint_interval_s,
                config_.max_checkpoints_per_process,
                static_cast<unsigned long>(config_.max_storage_mb));
        fprintf(stdout, "[rewind] delta: %s, compression: %s\n",
                config_.delta_checkpoints ? "on" : "off",
                config_.compression ? "zstd" : "off");

        if (config_.auto_track_services) {
            auto_track_straylight_services();
        }

        // Listen on a Unix socket for CLI commands
        setup_command_socket();

        return VoidResult<>::ok();
    }

    void tick() override {
        // Let the monitor observe all tracked processes
        monitor_.tick();

        // Auto-checkpoint any processes that are due
        engine_.tick_auto_checkpoints();

        // Prune dead processes periodically
        tick_count_++;
        if (tick_count_ % 60 == 0) { // every 60 seconds
            engine_.prune_dead_processes();
        }

        // Process any pending CLI commands from the socket
        process_commands();
    }

    void shutdown() override {
        fprintf(stdout, "[rewind] total storage used: %lu bytes\n",
                static_cast<unsigned long>(store_.total_storage_bytes()));
        cleanup_command_socket();
    }

    void on_reload() override {
        fprintf(stdout, "[rewind] reloading configuration...\n");
        config_ = load_config(config_.config_path);
        fprintf(stdout, "[rewind] interval now: %ds\n", config_.checkpoint_interval_s);
    }

private:
    RewindConfig config_;
    CheckpointStore store_;
    CheckpointEngine engine_;
    ProcessMonitor monitor_;
    uint64_t tick_count_ = 0;
    std::string socket_path_ = "/var/run/straylight/rewind.sock";

    void auto_track_straylight_services() {
        // Scan for running StrayLight services and auto-track them
        namespace fs = std::filesystem;
        std::error_code ec;
        auto run_dir = fs::path("/var/run/straylight");
        if (!fs::exists(run_dir, ec)) return;

        for (auto& entry : fs::directory_iterator(run_dir, ec)) {
            if (entry.path().extension() != ".pid") continue;
            if (entry.path().filename().string() == "straylight-rewind.pid") continue;

            std::ifstream pf(entry.path());
            if (!pf) continue;
            pid_t pid = 0;
            pf >> pid;
            if (pid <= 0) continue;

            auto res = engine_.start_tracking(pid);
            if (res) {
                fprintf(stdout, "[rewind] auto-tracking service pid %d (%s)\n",
                        pid, entry.path().stem().string().c_str());
            }
        }
    }

    void setup_command_socket() {
        // In production, this creates a Unix domain socket at socket_path_
        // and listens for commands from rewind-cli. For now, we use a
        // command file approach: CLI writes to /var/run/straylight/rewind.cmd
        // and daemon reads it each tick.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(socket_path_).parent_path(), ec);
    }

    void cleanup_command_socket() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove(socket_path_, ec);
        fs::remove(socket_path_ + ".cmd", ec);
    }

    void process_commands() {
        auto cmd_path = socket_path_ + ".cmd";
        std::ifstream in(cmd_path);
        if (!in) return;

        std::string line;
        while (std::getline(in, line)) {
            handle_command(line);
        }
        in.close();

        // Clear the command file
        std::ofstream clear(cmd_path, std::ios::trunc);
    }

    void handle_command(const std::string& line) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        auto response_path = socket_path_ + ".resp";
        std::ofstream resp(response_path, std::ios::app);

        if (cmd == "track") {
            pid_t pid;
            iss >> pid;
            auto r = engine_.start_tracking(pid);
            resp << (r ? "OK" : ("ERROR: " + r.err())) << "\n";
        }
        else if (cmd == "untrack") {
            pid_t pid;
            iss >> pid;
            auto r = engine_.stop_tracking(pid);
            resp << (r ? "OK" : ("ERROR: " + r.err())) << "\n";
        }
        else if (cmd == "checkpoint") {
            pid_t pid;
            iss >> pid;
            auto r = engine_.create_checkpoint(pid);
            if (r) resp << "OK: " << r.value() << "\n";
            else   resp << "ERROR: " << r.err() << "\n";
        }
        else if (cmd == "restore") {
            pid_t pid;
            std::string ckpt_id;
            iss >> pid >> ckpt_id;
            auto r = engine_.restore_checkpoint(pid, ckpt_id);
            resp << (r ? "OK" : ("ERROR: " + r.err())) << "\n";
        }
        else if (cmd == "list") {
            pid_t pid;
            iss >> pid;
            auto checkpoints = engine_.list_checkpoints(pid);
            resp << "CHECKPOINTS: " << checkpoints.size() << "\n";
            for (auto& c : checkpoints) {
                resp << c.checkpoint_id << " "
                     << c.timestamp << " "
                     << c.size_bytes << " "
                     << (c.is_delta ? "delta" : "full") << "\n";
            }
        }
        else if (cmd == "status") {
            auto info = engine_.get_tracking_info();
            resp << "TRACKING: " << info.size() << "\n";
            for (auto& ti : info) {
                resp << ti.pid << " "
                     << (ti.process_alive ? "alive" : "dead") << " "
                     << ti.checkpoint_count << " checkpoints\n";
            }
            resp << "STORAGE: " << store_.total_storage_bytes() << " bytes\n";
        }
        else {
            resp << "ERROR: unknown command: " << cmd << "\n";
        }
    }
};

} // namespace straylight::rewind

// ── main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string config_path = "/etc/straylight/rewind.conf";

    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--config") == 0 || std::strcmp(argv[i], "-c") == 0)
            && i + 1 < argc) {
            config_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            fprintf(stdout,
                "Usage: straylight-rewind [OPTIONS]\n"
                "\n"
                "Checkpoint/restore daemon for StrayLight OS processes.\n"
                "\n"
                "Options:\n"
                "  -c, --config <path>   Config file (default: /etc/straylight/rewind.conf)\n"
                "  -h, --help            Show this help\n");
            return 0;
        }
    }

    auto config = straylight::rewind::load_config(config_path);
    config.config_path = config_path;

    straylight::rewind::RewindDaemon daemon(config);
    return daemon.run();
}
