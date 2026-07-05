// bin/bus/main.cpp
#include "bus_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-bus");

    auto cfg_result = straylight::Config::load("/etc/straylight/bus.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("bus: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::BusDaemon daemon;
    return daemon.run(cfg_result.value());
}
