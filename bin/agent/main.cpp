// bin/agent/main.cpp
#include "agent_daemon.h"

#include <fstream>

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-agent");

    auto cfg_result = straylight::Config::load("/etc/straylight/agent.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("agent: config not found, using defaults: {}", cfg_result.error());
    }

    // If config load failed, create a fallback config from a temp empty JSON file.
    // In production, /etc/straylight/agent.conf is deployed by the installer.
    // The daemon's init() uses get() with default values, so missing keys are safe.
    if (!cfg_result.has_value()) {
        const char* fallback = "/tmp/straylight-agent-default.conf";
        {
            std::ofstream f(fallback);
            f << "{}";
        }
        cfg_result = straylight::Config::load(fallback);
        if (!cfg_result.has_value()) {
            SL_ERROR("agent: cannot create fallback config: {}", cfg_result.error());
            return 1;
        }
    }

    straylight::AgentDaemon daemon;
    return daemon.run(cfg_result.value());
}
