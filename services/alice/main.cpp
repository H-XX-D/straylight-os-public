// services/alice/main.cpp
#include "alice_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-alice");

    auto cfg_result = straylight::Config::load("/etc/straylight/alice.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("alice: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::AliceDaemon daemon;
    return daemon.run(cfg_result.value());
}
