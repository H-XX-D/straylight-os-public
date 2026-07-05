// tests/unit/subsystems/test_agent_queue.cpp
#include <gtest/gtest.h>
#include "task_queue.h"

using namespace straylight::agent;

TEST(AgentQueue, PushAndPopFIFO) {
    TaskQueue q(16);

    Task t1{1, Priority::Normal, TaskType::Inference, "payload-1"};
    Task t2{2, Priority::Normal, TaskType::Training, "payload-2"};
    Task t3{3, Priority::Normal, TaskType::Custom, "payload-3"};

    ASSERT_TRUE(q.push(t1).has_value());
    ASSERT_TRUE(q.push(t2).has_value());
    ASSERT_TRUE(q.push(t3).has_value());
    EXPECT_EQ(q.size(), 3u);

    auto r1 = q.pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().id, 1u);

    auto r2 = q.pop();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().id, 2u);

    auto r3 = q.pop();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3.value().id, 3u);

    EXPECT_EQ(q.size(), 0u);
}

TEST(AgentQueue, HighPriorityFirst) {
    TaskQueue q(16);

    Task low{1, Priority::Low, TaskType::Custom, "low"};
    Task normal{2, Priority::Normal, TaskType::Custom, "normal"};
    Task high{3, Priority::High, TaskType::Custom, "high"};
    Task critical{4, Priority::Critical, TaskType::Custom, "critical"};

    // Push in ascending priority order
    ASSERT_TRUE(q.push(low).has_value());
    ASSERT_TRUE(q.push(normal).has_value());
    ASSERT_TRUE(q.push(high).has_value());
    ASSERT_TRUE(q.push(critical).has_value());

    // Pop should yield descending priority order
    auto r1 = q.pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().id, 4u);
    EXPECT_EQ(r1.value().priority, Priority::Critical);

    auto r2 = q.pop();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().id, 3u);
    EXPECT_EQ(r2.value().priority, Priority::High);

    auto r3 = q.pop();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3.value().id, 2u);
    EXPECT_EQ(r3.value().priority, Priority::Normal);

    auto r4 = q.pop();
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4.value().id, 1u);
    EXPECT_EQ(r4.value().priority, Priority::Low);
}

TEST(AgentQueue, PopEmptyReturnsError) {
    TaskQueue q(16);
    auto r = q.pop();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "task queue is empty");
}

TEST(AgentQueue, CapacityEnforced) {
    TaskQueue q(2);  // capacity of 2

    Task t1{1, Priority::Normal, TaskType::Inference, "a"};
    Task t2{2, Priority::Normal, TaskType::Inference, "b"};
    Task t3{3, Priority::Normal, TaskType::Inference, "c"};

    ASSERT_TRUE(q.push(t1).has_value());
    ASSERT_TRUE(q.push(t2).has_value());
    EXPECT_EQ(q.size(), 2u);

    // Third push should fail — at capacity
    auto r = q.push(t3);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "task queue at capacity");
    EXPECT_EQ(q.size(), 2u);
}

TEST(AgentQueue, CancelRemovesTask) {
    TaskQueue q(16);

    Task t1{1, Priority::Normal, TaskType::Inference, "a"};
    Task t2{2, Priority::High, TaskType::Training, "b"};
    Task t3{3, Priority::Normal, TaskType::Custom, "c"};

    ASSERT_TRUE(q.push(t1).has_value());
    ASSERT_TRUE(q.push(t2).has_value());
    ASSERT_TRUE(q.push(t3).has_value());
    EXPECT_EQ(q.size(), 3u);

    // Cancel the high-priority task
    EXPECT_TRUE(q.cancel(2));
    EXPECT_EQ(q.size(), 2u);

    // Cancelling a non-existent id returns false
    EXPECT_FALSE(q.cancel(999));

    // Remaining tasks should come out in priority/FIFO order: t1 then t3
    auto r1 = q.pop();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().id, 1u);

    auto r2 = q.pop();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().id, 3u);
}

TEST(AgentQueue, PeekDoesNotRemove) {
    TaskQueue q(16);

    Task t1{1, Priority::High, TaskType::Inference, "peek-test"};
    ASSERT_TRUE(q.push(t1).has_value());

    // Peek should return the task
    auto p1 = q.peek();
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(p1.value().id, 1u);
    EXPECT_EQ(p1.value().payload, "peek-test");

    // Size should still be 1
    EXPECT_EQ(q.size(), 1u);

    // Peek again should return the same task
    auto p2 = q.peek();
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p2.value().id, 1u);

    // Pop should also return it
    auto r = q.pop();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value().id, 1u);
    EXPECT_EQ(q.size(), 0u);

    // Peek on empty should fail
    auto p3 = q.peek();
    EXPECT_FALSE(p3.has_value());
}
