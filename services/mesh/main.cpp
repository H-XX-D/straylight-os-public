// services/mesh/main.cpp
// StrayLight Mesh — Zero-Config GPU Sharing Daemon
// D-Bus: org.straylight.Mesh1
// Config: /etc/straylight/mesh.conf
#include "mesh_daemon.h"

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-mesh");

    auto cfg_result = straylight::Config::load("/etc/straylight/mesh.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("mesh: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::MeshDaemon daemon;
    return daemon.run(cfg_result.value());
}
