// tests/unit/apps/test_widgets.cpp
// Unit tests for widget registry, GPU HUD parsing, and CPU topology parsing.

#include <gtest/gtest.h>

#include "apps/widgets/widget_registry.h"
#include "apps/widgets/ml/gpu_hud.h"
#include "apps/widgets/system/cpu_topology.h"

using namespace straylight::widgets;

// ── Widget Registry Tests ───────────────────────────────────────────────────

TEST(WidgetRegistry, AllTwentyWidgetsRegistered) {
    auto& reg = WidgetRegistry::instance();
    // All 20 widget .cpp files use REGISTER_WIDGET, so they self-register.
    EXPECT_EQ(reg.size(), 20u);
}

TEST(WidgetRegistry, HasKnownIds) {
    auto& reg = WidgetRegistry::instance();
    EXPECT_TRUE(reg.has("gpu_hud"));
    EXPECT_TRUE(reg.has("training_dashboard"));
    EXPECT_TRUE(reg.has("tensor_inspector"));
    EXPECT_TRUE(reg.has("model_browser"));
    EXPECT_TRUE(reg.has("inference_monitor"));
    EXPECT_TRUE(reg.has("cluster_map"));
    EXPECT_TRUE(reg.has("job_queue"));
    EXPECT_TRUE(reg.has("network_topology"));
    EXPECT_TRUE(reg.has("resource_allocator"));
    EXPECT_TRUE(reg.has("power_monitor"));
    EXPECT_TRUE(reg.has("cpu_topology"));
    EXPECT_TRUE(reg.has("memory_pressure"));
    EXPECT_TRUE(reg.has("io_latency"));
    EXPECT_TRUE(reg.has("entropy_pool"));
    EXPECT_TRUE(reg.has("scheduler_view"));
    EXPECT_TRUE(reg.has("quantum_circuit"));
    EXPECT_TRUE(reg.has("snn_visualizer"));
    EXPECT_TRUE(reg.has("experiment_tracker"));
    EXPECT_TRUE(reg.has("dataset_browser"));
    EXPECT_TRUE(reg.has("paper_notes"));
}

TEST(WidgetRegistry, CreateReturnsValidWidget) {
    auto& reg = WidgetRegistry::instance();
    auto w = reg.create("gpu_hud");
    ASSERT_NE(w, nullptr);
    EXPECT_STREQ(w->name(), "GPU HUD");
    EXPECT_TRUE(w->supports_embed());
}

TEST(WidgetRegistry, CreateUnknownReturnsNull) {
    auto& reg = WidgetRegistry::instance();
    auto w = reg.create("nonexistent_widget");
    EXPECT_EQ(w, nullptr);
}

TEST(WidgetRegistry, CategoryFilterWorks) {
    auto& reg = WidgetRegistry::instance();
    auto ml = reg.by_category(WidgetCategory::ML);
    EXPECT_EQ(ml.size(), 5u);
    auto hpc = reg.by_category(WidgetCategory::HPC);
    EXPECT_EQ(hpc.size(), 5u);
    auto sys = reg.by_category(WidgetCategory::System);
    EXPECT_EQ(sys.size(), 5u);
    auto res = reg.by_category(WidgetCategory::Research);
    EXPECT_EQ(res.size(), 5u);
}

TEST(WidgetRegistry, EachWidgetHasUniqueName) {
    auto& reg = WidgetRegistry::instance();
    std::set<std::string> names;
    for (auto& entry : reg.entries()) {
        auto [it, inserted] = names.insert(entry.display_name);
        EXPECT_TRUE(inserted) << "Duplicate widget name: " << entry.display_name;
    }
}

TEST(WidgetRegistry, PollIntervalsArePositive) {
    auto& reg = WidgetRegistry::instance();
    for (auto& entry : reg.entries()) {
        auto w = reg.create(entry.id);
        ASSERT_NE(w, nullptr);
        EXPECT_GE(w->poll_interval(), 0.0f) << "Widget " << entry.id << " has negative poll interval";
    }
}

// ── GPU HUD Parsing Tests ───────────────────────────────────────────────────

TEST(GpuHud, ParseNvidiaSmiSingleGpu) {
    std::string csv =
        " 0, NVIDIA GeForce RTX 4090, 45, 3200, 24576, 52, 85.3, 450.0, 2520, 10501, 35\n";
    auto gpus = GpuHudWidget::parse_nvidia_smi(csv);
    ASSERT_EQ(gpus.size(), 1u);
    EXPECT_EQ(gpus[0].index, 0);
    EXPECT_EQ(gpus[0].name, "NVIDIA GeForce RTX 4090");
    EXPECT_FLOAT_EQ(gpus[0].utilization_pct, 45.0f);
    EXPECT_FLOAT_EQ(gpus[0].memory_used_mb, 3200.0f);
    EXPECT_FLOAT_EQ(gpus[0].memory_total_mb, 24576.0f);
    EXPECT_FLOAT_EQ(gpus[0].temperature_c, 52.0f);
    EXPECT_FLOAT_EQ(gpus[0].power_draw_w, 85.3f);
    EXPECT_FLOAT_EQ(gpus[0].power_limit_w, 450.0f);
    EXPECT_EQ(gpus[0].clock_graphics_mhz, 2520);
    EXPECT_EQ(gpus[0].clock_memory_mhz, 10501);
    EXPECT_EQ(gpus[0].fan_speed_pct, 35);
}

TEST(GpuHud, ParseNvidiaSmiMultiGpu) {
    std::string csv =
        " 0, NVIDIA A100-SXM4-80GB, 92, 45000, 81920, 71, 295.0, 400.0, 1410, 1215, 0\n"
        " 1, NVIDIA A100-SXM4-80GB, 88, 42000, 81920, 69, 280.5, 400.0, 1395, 1215, 0\n";
    auto gpus = GpuHudWidget::parse_nvidia_smi(csv);
    ASSERT_EQ(gpus.size(), 2u);
    EXPECT_EQ(gpus[0].index, 0);
    EXPECT_EQ(gpus[1].index, 1);
    EXPECT_FLOAT_EQ(gpus[0].utilization_pct, 92.0f);
    EXPECT_FLOAT_EQ(gpus[1].utilization_pct, 88.0f);
}

TEST(GpuHud, ParseNvidiaSmiEmpty) {
    std::string csv = "";
    auto gpus = GpuHudWidget::parse_nvidia_smi(csv);
    EXPECT_TRUE(gpus.empty());
}

TEST(GpuHud, ParseNvidiaSmiSkipsComments) {
    std::string csv =
        "# This is a comment\n"
        " 0, Tesla V100, 60, 8000, 16384, 55, 150.0, 300.0, 1530, 877, 0\n";
    auto gpus = GpuHudWidget::parse_nvidia_smi(csv);
    ASSERT_EQ(gpus.size(), 1u);
    EXPECT_EQ(gpus[0].name, "Tesla V100");
}

TEST(GpuHud, ParseNvidiaSmiPartialLine) {
    // Only 6 fields parsed (minimum required)
    std::string csv = " 0, GPU, 50, 1000, 8192, 45\n";
    auto gpus = GpuHudWidget::parse_nvidia_smi(csv);
    ASSERT_EQ(gpus.size(), 1u);
    EXPECT_FLOAT_EQ(gpus[0].utilization_pct, 50.0f);
}

// ── CPU Topology Parsing Tests ──────────────────────────────────────────────

TEST(CpuTopology, ParseProcCpuinfoSingleCore) {
    std::string content =
        "processor\t: 0\n"
        "vendor_id\t: GenuineIntel\n"
        "cpu family\t: 6\n"
        "model name\t: Intel(R) Core(TM) i9-13900K\n"
        "core id\t\t: 0\n"
        "physical id\t: 0\n"
        "cpu MHz\t\t: 3000.000\n"
        "\n";
    auto cores = CpuTopologyWidget::parse_proc_cpuinfo(content);
    ASSERT_EQ(cores.size(), 1u);
    EXPECT_EQ(cores[0].cpu_id, 0);
    EXPECT_EQ(cores[0].core_id, 0);
    EXPECT_EQ(cores[0].package_id, 0);
    EXPECT_FLOAT_EQ(cores[0].freq_mhz, 3000.0f);
}

TEST(CpuTopology, ParseProcCpuinfoMultiCore) {
    std::string content =
        "processor\t: 0\n"
        "core id\t\t: 0\n"
        "physical id\t: 0\n"
        "cpu MHz\t\t: 2500.000\n"
        "\n"
        "processor\t: 1\n"
        "core id\t\t: 1\n"
        "physical id\t: 0\n"
        "cpu MHz\t\t: 2600.000\n"
        "\n"
        "processor\t: 2\n"
        "core id\t\t: 0\n"
        "physical id\t: 1\n"
        "cpu MHz\t\t: 2700.000\n"
        "\n";
    auto cores = CpuTopologyWidget::parse_proc_cpuinfo(content);
    ASSERT_EQ(cores.size(), 3u);
    EXPECT_EQ(cores[0].cpu_id, 0);
    EXPECT_EQ(cores[1].cpu_id, 1);
    EXPECT_EQ(cores[2].cpu_id, 2);
    EXPECT_EQ(cores[2].package_id, 1);
}

TEST(CpuTopology, ParseProcCpuinfoEmpty) {
    auto cores = CpuTopologyWidget::parse_proc_cpuinfo("");
    EXPECT_TRUE(cores.empty());
}

TEST(CpuTopology, ParseProcCpuinfoNoTrailingNewline) {
    // Last block without trailing blank line
    std::string content =
        "processor\t: 0\n"
        "core id\t\t: 0\n"
        "physical id\t: 0\n"
        "cpu MHz\t\t: 4000.000\n";
    auto cores = CpuTopologyWidget::parse_proc_cpuinfo(content);
    ASSERT_EQ(cores.size(), 1u);
    EXPECT_FLOAT_EQ(cores[0].freq_mhz, 4000.0f);
}
