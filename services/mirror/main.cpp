/**
 * straylight-mirror — Live system cloning daemon.
 *
 * Can run in source mode (pushing state) or target mode (receiving state).
 * Streams entire OS state over the mesh network.
 *
 * Usage:
 *   straylight-mirror --source --target-host <host> --target-port <port>
 *   straylight-mirror --target --listen-port <port>
 */

#include "mirror_engine.h"
#include "straylight/daemon_base.h"
#include "straylight/result.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace straylight::mirror {

class MirrorDaemon : public DaemonBase {
public:
    MirrorDaemon(int argc, char** argv)
        : DaemonBase("straylight-mirror")
    {
        parse_mirror_args(argc, argv);
    }

protected:
    VoidResult<> init() override {
        fprintf(stdout, "[straylight-mirror] role: %s\n",
                is_source_ ? "source" : "target");

        if (is_source_) {
            if (target_host_.empty()) {
                return VoidResult<>::error("--target-host required in source mode");
            }
            auto result = engine_.start_mirror(target_host_, target_port_);
            if (!result) {
                return VoidResult<>::error("start_mirror failed: " + result.err());
            }
        } else {
            auto result = engine_.start_target(listen_port_);
            if (!result) {
                return VoidResult<>::error("start_target failed: " + result.err());
            }
        }

        set_tick_interval_ms(500);
        return VoidResult<>::ok();
    }

    void tick() override {
        auto progress = engine_.get_progress();

        // Print progress periodically.
        ++tick_count_;
        if (tick_count_ % 4 == 0) {  // Every 2 seconds
            fprintf(stdout, "[straylight-mirror] phase=%s progress=%.1f%% "
                    "synced=%lluMB files=%llu/%llu elapsed=%.1fs\n",
                    phase_to_string(progress.phase),
                    progress.percent_complete(),
                    static_cast<unsigned long long>(progress.synced_bytes / (1024 * 1024)),
                    static_cast<unsigned long long>(progress.files_synced),
                    static_cast<unsigned long long>(progress.files_total),
                    progress.elapsed_seconds);
        }

        // Check if mirror is complete or failed.
        if (!engine_.is_active()) {
            if (progress.phase == MirrorPhase::Complete) {
                fprintf(stdout, "[straylight-mirror] mirror completed successfully\n");
            } else if (progress.phase == MirrorPhase::Failed) {
                fprintf(stderr, "[straylight-mirror] mirror failed: %s\n",
                        progress.error.c_str());
            }
            request_shutdown();
        }
    }

    void shutdown() override {
        engine_.stop_mirror();
    }

private:
    MirrorEngine engine_;
    bool is_source_ = true;
    std::string target_host_;
    uint16_t target_port_ = 9900;
    uint16_t listen_port_ = 9900;
    uint64_t tick_count_ = 0;

    void parse_mirror_args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--source") {
                is_source_ = true;
            } else if (arg == "--target") {
                is_source_ = false;
            } else if (arg == "--target-host" && i + 1 < argc) {
                target_host_ = argv[++i];
            } else if (arg == "--target-port" && i + 1 < argc) {
                target_port_ = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (arg == "--listen-port" && i + 1 < argc) {
                listen_port_ = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (arg == "--bandwidth" && i + 1 < argc) {
                engine_.set_bandwidth_limit_mbps(std::stod(argv[++i]));
            }
        }
    }
};

} // namespace straylight::mirror

int main(int argc, char** argv) {
    straylight::mirror::MirrorDaemon daemon(argc, argv);
    return daemon.run();
}
