// bin/entropy/main.cpp
#include "entropy_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-entropy");

    auto cfg_result = straylight::Config::load("/etc/straylight/entropy.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("entropy: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::EntropyDaemon daemon;
    return daemon.run(cfg_result.value());
}
