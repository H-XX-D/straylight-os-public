// services/swarm/main.cpp
#include "swarm_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-swarm");

    auto cfg_result = straylight::Config::load("/etc/straylight/swarm.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("swarm: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::SwarmDaemon daemon;
    return daemon.run(cfg_result.value());
}
