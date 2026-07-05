// bin/scheduler/main.cpp
#include "scheduler_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-scheduler");

    auto cfg_result = straylight::Config::load("/etc/straylight/scheduler.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("scheduler: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::SchedulerDaemon daemon;
    return daemon.run(cfg_result.value());
}
