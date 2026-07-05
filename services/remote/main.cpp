// services/remote/main.cpp
// Entry point for straylight-remote-agent daemon.
// D-Bus name: org.straylight.Remote1
// Config: /etc/straylight/remote.conf
#include "agent.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-remote-agent");

    auto cfg_result = straylight::Config::load("/etc/straylight/remote.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("remote-agent: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::RemoteAgent daemon;
    return daemon.run(cfg_result.value());
}
