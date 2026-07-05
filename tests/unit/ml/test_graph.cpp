#include <gtest/gtest.h>
#include <straylight/ml/graph.h>

using namespace straylight::ml;

TEST(GraphTest, CreateEmptyGraph) {
    Graph g("test_model");
    EXPECT_EQ(g.name(), "test_model");
    EXPECT_EQ(g.num_nodes(), 0u);
}

TEST(GraphTest, AddNodesAndEdges) {
    Graph g("matmul_chain");
    auto input = g.add_input("x", {1, 768});
    auto weight = g.add_input("w", {768, 768});
    auto matmul = g.add_op("MatMul", {input, weight}, "mm0");
    auto relu = g.add_op("ReLU", {matmul}, "relu0");

    EXPECT_EQ(g.num_nodes(), 4u);
    EXPECT_EQ(g.node(matmul).inputs.size(), 2u);
    EXPECT_EQ(g.node(relu).inputs.size(), 1u);
    EXPECT_EQ(g.node(relu).inputs[0], matmul);
}

TEST(GraphTest, TopologicalOrder) {
    Graph g("topo_test");
    auto a = g.add_input("a", {1});
    auto b = g.add_op("Neg", {a}, "neg");
    auto c = g.add_op("Abs", {b}, "abs");

    auto order = g.topological_order();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], a);
    EXPECT_EQ(order[1], b);
    EXPECT_EQ(order[2], c);
}
