// services/replay/main.cpp
// StrayLight Replay — System Event Flight Recorder daemon.
// D-Bus: org.straylight.Replay1
// Config: /etc/straylight/replay.conf
#include "replay_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-replay");

    auto cfg_result = straylight::Config::load("/etc/straylight/replay.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("replay: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::ReplayDaemon daemon;
    return daemon.run(cfg_result.value());
}
