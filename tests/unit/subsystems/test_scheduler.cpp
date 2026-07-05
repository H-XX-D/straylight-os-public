// tests/unit/subsystems/test_scheduler.cpp
#include <gtest/gtest.h>
#include "topology.h"
#include "cgroup.h"
#include "scheduler_daemon.h"

#include <filesystem>
#include <fstream>

using namespace straylight;

TEST(Topology, ParsesCpuInfo) {
    const std::string fake_cpuinfo = R"(
processor       : 0
core id         : 0
physical id     : 0

processor       : 1
core id         : 0
physical id     : 0

processor       : 2
core id         : 1
physical id     : 0
)";
    Topology topo;
    ASSERT_TRUE(topo.parse_cpuinfo(fake_cpuinfo).has_value());
    EXPECT_EQ(topo.logical_cpu_count(), 3u);
    EXPECT_EQ(topo.physical_core_count(), 2u);
}

TEST(Cgroup, ParsesCpuWeight) {
    auto tmp = std::filesystem::temp_directory_path() / "sl_test_cgroup";
    std::filesystem::create_directories(tmp);
    std::ofstream(tmp / "cpu.weight") << "100\n";

    CgroupV2 cg(tmp);
    auto w = cg.read_cpu_weight();
    ASSERT_TRUE(w.has_value());
    EXPECT_EQ(w.value(), 100u);

    std::filesystem::remove_all(tmp);
}

TEST(Cgroup, SetsCpuWeight) {
    auto tmp = std::filesystem::temp_directory_path() / "sl_test_cgroup_set";
    std::filesystem::create_directories(tmp);
    std::ofstream(tmp / "cpu.weight") << "100\n";

    CgroupV2 cg(tmp);
    ASSERT_TRUE(cg.set_cpu_weight(200).has_value());

    std::ifstream f(tmp / "cpu.weight");
    std::string val;
    f >> val;
    EXPECT_EQ(val, "200");

    std::filesystem::remove_all(tmp);
}

TEST(SchedulerPriority, HigherPriorityGetsMoreWeight) {
    PriorityQueue pq;
    pq.enqueue("straylight-core",  Priority::High);
    pq.enqueue("straylight-agent", Priority::Normal);
    pq.enqueue("straylight-fuse",  Priority::Low);

    EXPECT_GT(pq.cpu_weight("straylight-core"),
              pq.cpu_weight("straylight-agent"));
    EXPECT_GT(pq.cpu_weight("straylight-agent"),
              pq.cpu_weight("straylight-fuse"));
}
