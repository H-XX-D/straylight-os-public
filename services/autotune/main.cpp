// services/autotune/main.cpp
#include "autotune_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-autotune");

    auto cfg_result = straylight::Config::load("/etc/straylight/autotune.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("autotune: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::AutotuneDaemon daemon;
    return daemon.run(cfg_result.value());
}
