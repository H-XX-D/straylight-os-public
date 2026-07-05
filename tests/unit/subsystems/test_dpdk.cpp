// tests/unit/subsystems/test_dpdk.cpp
#include <gtest/gtest.h>
#include "flow.h"
#include "pipeline.h"
#include "port.h"
#include "tensor_transport.h"

using namespace straylight::dpdk;

// ─── FlowClassifier tests ───────────────────────────────────────────────

TEST(DpdkFlow, AddAndClassifyExactMatch) {
    FlowClassifier fc;
    FlowRule rule;
    rule.src_ip   = 0xC0000201; // 192.0.2.1
    rule.dst_ip   = 0xC0000202; // 192.0.2.2
    rule.src_port = 12345;
    rule.dst_port = 80;
    rule.protocol = 6; // TCP
    rule.action   = FlowAction::Drop;

    auto id_res = fc.add_rule(rule);
    ASSERT_TRUE(id_res.has_value());
    EXPECT_GE(id_res.value(), 1u);
    EXPECT_EQ(fc.rule_count(), 1u);

    auto action_res = fc.classify(0xC0000201, 0xC0000202, 12345, 80, 6);
    ASSERT_TRUE(action_res.has_value());
    EXPECT_EQ(action_res.value(), FlowAction::Drop);
}

TEST(DpdkFlow, WildcardFieldsMatch) {
    FlowClassifier fc;
    FlowRule rule;
    rule.src_ip   = 0;     // wildcard
    rule.dst_ip   = 0;     // wildcard
    rule.src_port = 0;     // wildcard
    rule.dst_port = 443;   // match HTTPS
    rule.protocol = 0;     // wildcard
    rule.action   = FlowAction::Mirror;

    auto id_res = fc.add_rule(rule);
    ASSERT_TRUE(id_res.has_value());

    // Should match any packet going to port 443
    auto a1 = fc.classify(0x01020304, 0x05060708, 9999, 443, 17);
    ASSERT_TRUE(a1.has_value());
    EXPECT_EQ(a1.value(), FlowAction::Mirror);

    // Should NOT match port 80
    auto a2 = fc.classify(0x01020304, 0x05060708, 9999, 80, 17);
    ASSERT_TRUE(a2.has_value());
    EXPECT_EQ(a2.value(), FlowAction::Pass); // default, no match
}

TEST(DpdkFlow, RemoveRule) {
    FlowClassifier fc;
    FlowRule rule;
    rule.dst_port = 22;
    rule.action   = FlowAction::Drop;

    auto id_res = fc.add_rule(rule);
    ASSERT_TRUE(id_res.has_value());
    uint64_t id = id_res.value();
    EXPECT_EQ(fc.rule_count(), 1u);

    auto rem_res = fc.remove_rule(id);
    EXPECT_TRUE(rem_res.has_value());
    EXPECT_EQ(fc.rule_count(), 0u);

    // Removing again should fail
    auto rem2 = fc.remove_rule(id);
    EXPECT_FALSE(rem2.has_value());
}

TEST(DpdkFlow, RemoveNonexistentRuleFails) {
    FlowClassifier fc;
    auto res = fc.remove_rule(99999);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not found"), std::string::npos);
}

TEST(DpdkFlow, FirstMatchWins) {
    FlowClassifier fc;

    FlowRule rule1;
    rule1.dst_port = 80;
    rule1.action   = FlowAction::Drop;
    fc.add_rule(rule1);

    FlowRule rule2;
    rule2.dst_port = 80;
    rule2.action   = FlowAction::Pass;
    fc.add_rule(rule2);

    auto res = fc.classify(0, 0, 0, 80, 0);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res.value(), FlowAction::Drop); // first rule wins
}

TEST(DpdkFlow, GetRule) {
    FlowClassifier fc;
    FlowRule rule;
    rule.protocol = 17;
    rule.action   = FlowAction::Redirect;
    rule.redirect_target = 3;

    auto id_res = fc.add_rule(rule);
    ASSERT_TRUE(id_res.has_value());

    const FlowRule* got = fc.get_rule(id_res.value());
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->protocol, 17);
    EXPECT_EQ(got->action, FlowAction::Redirect);
    EXPECT_EQ(got->redirect_target, 3);

    EXPECT_EQ(fc.get_rule(999999), nullptr);
}

// ─── Pipeline tests ─────────────────────────────────────────────────────

TEST(DpdkPipeline, AddStageEmptyNameFails) {
    Pipeline p;
    auto res = p.add_stage("", [](auto**, auto n) { return n; });
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("empty"), std::string::npos);
}

TEST(DpdkPipeline, AddStageNullHandlerFails) {
    Pipeline p;
    auto res = p.add_stage("test", nullptr);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("null"), std::string::npos);
}

TEST(DpdkPipeline, AddDuplicateStageNameFails) {
    Pipeline p;
    auto r1 = p.add_stage("filter", [](auto**, auto n) { return n; });
    EXPECT_TRUE(r1.has_value());
    auto r2 = p.add_stage("filter", [](auto**, auto n) { return n; });
    EXPECT_FALSE(r2.has_value());
    EXPECT_NE(r2.error().find("Duplicate"), std::string::npos);
}

TEST(DpdkPipeline, StageCountAndStats) {
    Pipeline p;
    p.add_stage("stage1", [](auto**, auto n) { return n; });
    p.add_stage("stage2", [](auto**, auto n) { return n; });
    EXPECT_EQ(p.stage_count(), 2u);

    auto stats = p.stats();
    EXPECT_EQ(stats.total_rx, 0u);
    EXPECT_EQ(stats.total_tx, 0u);

    p.reset_stats();
    stats = p.stats();
    EXPECT_EQ(stats.stages_run, 0u);
}

// ─── PortManager tests ──────────────────────────────────────────────────

TEST(DpdkPort, ConfigureWithoutInitFails) {
    PortManager pm;
    auto res = pm.configure_port(0, 1, 1);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not initialized"), std::string::npos);
}

TEST(DpdkPort, StartWithoutInitFails) {
    PortManager pm;
    auto res = pm.start(0);
    EXPECT_FALSE(res.has_value());
}

TEST(DpdkPort, DefaultState) {
    PortManager pm;
    EXPECT_FALSE(pm.initialized());
    EXPECT_EQ(pm.port_count(), 0);
}

// ─── TensorTransport tests ─────────────────────────────────────────────

TEST(DpdkTensor, SendWithoutMempoolFails) {
    TensorTransport tt;
    uint8_t data[] = {1, 2, 3, 4};
    uint8_t mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    auto res = tt.send_tensor(0, data, sizeof(data), mac);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("mempool"), std::string::npos);
}

TEST(DpdkTensor, SendNullDataFails) {
    TensorTransport tt;
    uint8_t mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    auto res = tt.send_tensor(0, nullptr, 100, mac);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Invalid"), std::string::npos);
}

TEST(DpdkTensor, RecvNullBufferFails) {
    TensorTransport tt;
    auto res = tt.recv_tensor(0, nullptr, 1024);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Invalid"), std::string::npos);
}

TEST(DpdkTensor, SetMtuAndEthertype) {
    TensorTransport tt;
    tt.set_mtu(9000);
    tt.set_ethertype(0x1234);
    // No crash; configuration stored internally
    // Verify by attempting send (will fail for different reason)
    uint8_t data[] = {1};
    uint8_t mac[] = {0};
    auto res = tt.send_tensor(0, data, 1, mac);
    EXPECT_FALSE(res.has_value()); // fails because no mempool, not because of MTU
}
