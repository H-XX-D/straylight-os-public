#include "registry_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-registry");

    auto cfg_result = straylight::Config::load("/etc/straylight/registry.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("registry: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::RegistryDaemon daemon;
    return daemon.run(cfg_result.value());
}
