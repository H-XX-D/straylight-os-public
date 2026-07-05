#include <gtest/gtest.h>
#include "service_graph.h"

using namespace straylight::test;

class DaemonStartupTest : public ::testing::Test {
protected:
    ServiceGraph graph;

    void SetUp() override {
        // Load from the project's etc/systemd/system/ directory
        // CMAKE_SOURCE_DIR is passed via compile definitions
        graph.load_service_dir(SERVICE_DIR);
    }
};

TEST_F(DaemonStartupTest, EntropyHasNoStraylightDeps) {
    auto deps = graph.dependencies("straylight-entropy.service");
    EXPECT_TRUE(deps.empty())
        << "entropy should start before all other straylight services";
}

TEST_F(DaemonStartupTest, BusDoesNotDependOnCore) {
    EXPECT_FALSE(graph.has_dep("straylight-bus.service",
                               "straylight-core.service"))
        << "bus must not depend on core (would create circular dependency)";
}

TEST_F(DaemonStartupTest, CoreDependsOnBusAndRegistry) {
    EXPECT_TRUE(graph.has_dep("straylight-core.service",
                              "straylight-bus.service"))
        << "core must depend on bus for D-Bus communication";
    EXPECT_TRUE(graph.has_dep("straylight-core.service",
                              "straylight-registry.service"))
        << "core must depend on registry for config lookup";
}

TEST_F(DaemonStartupTest, RegistryDependsOnBus) {
    EXPECT_TRUE(graph.has_dep("straylight-registry.service",
                              "straylight-bus.service"))
        << "registry must depend on bus for D-Bus exposure";
}

TEST_F(DaemonStartupTest, SchedulerDependsOnRegistry) {
    EXPECT_TRUE(graph.has_dep("straylight-scheduler.service",
                              "straylight-registry.service"))
        << "scheduler must depend on registry for config";
}

TEST_F(DaemonStartupTest, NoCycles) {
    EXPECT_FALSE(graph.has_cycle())
        << "daemon dependency graph must be a DAG (no cycles)";
}
