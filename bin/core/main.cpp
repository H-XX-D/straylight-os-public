// bin/core/main.cpp
#include "core_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-core");

    auto cfg_result = straylight::Config::load("/etc/straylight/core.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("core: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::CoreDaemon daemon;
    return daemon.run(cfg_result.value());
}
