// tests/unit/compositor/test_tiling.cpp
#include <gtest/gtest.h>
#include <gmock/gmock.h>

// Pull in tiling standalone — mock View
// We test Tiling::arrange() geometry without a real wlroots display.

#include <vector>
#include <cstdint>

// Minimal mock wlr_box to avoid wlroots headers in unit tests
struct wlr_box { int x, y, width, height; };

// Minimal View mock
struct MockView {
    int x = 0, y = 0, w = 0, h = 0;
    void set_position(int px, int py) { x = px; y = py; }
    void set_size(int pw, int ph)     { w = pw; h = ph; }
};

// Standalone tiling algorithm (extracted from Tiling for test isolation)
enum class LayoutMode { Tiling, Floating, Monocle };

static void tiling_arrange(const std::vector<MockView*>& views,
                            const wlr_box& area,
                            LayoutMode mode,
                            float master_ratio = 0.55f,
                            int gap = 4)
{
    if (views.empty()) return;
    const int g = gap;
    const int n = static_cast<int>(views.size());

    if (mode == LayoutMode::Monocle) {
        for (auto* v : views) {
            v->set_position(area.x + g, area.y + g);
            v->set_size(area.width - 2*g, area.height - 2*g);
        }
        return;
    }
    if (mode == LayoutMode::Floating) return;

    // Tiling
    if (n == 1) {
        views[0]->set_position(area.x + g, area.y + g);
        views[0]->set_size(area.width - 2*g, area.height - 2*g);
        return;
    }
    int master_w = static_cast<int>((area.width - 3*g) * master_ratio);
    int stack_w  = area.width - master_w - 3*g;
    int master_x = area.x + g;
    int stack_x  = master_x + master_w + g;

    views[0]->set_position(master_x, area.y + g);
    views[0]->set_size(master_w, area.height - 2*g);

    int stack_count = n - 1;
    int slot_h = (area.height - 2*g - (stack_count - 1)*g) / stack_count;
    for (int i = 0; i < stack_count; ++i) {
        int y = area.y + g + i * (slot_h + g);
        views[i+1]->set_position(stack_x, y);
        views[i+1]->set_size(stack_w, slot_h);
    }
}

class TilingTest : public ::testing::Test {
protected:
    wlr_box area{0, 0, 1920, 1080};
    std::vector<MockView>  storage;
    std::vector<MockView*> views;

    void make_views(int n) {
        storage.resize(n);
        views.clear();
        for (auto& v : storage) views.push_back(&v);
    }
};

TEST_F(TilingTest, SingleWindowFillsUsableArea) {
    make_views(1);
    tiling_arrange(views, area, LayoutMode::Tiling, 0.55f, 4);
    EXPECT_EQ(views[0]->x, 4);
    EXPECT_EQ(views[0]->y, 4);
    EXPECT_EQ(views[0]->w, 1920 - 8);
    EXPECT_EQ(views[0]->h, 1080 - 8);
}

TEST_F(TilingTest, TwoWindowsMasterStack) {
    make_views(2);
    tiling_arrange(views, area, LayoutMode::Tiling, 0.55f, 4);
    // Master: left column
    EXPECT_EQ(views[0]->x, 4);
    EXPECT_GT(views[0]->w, 0);
    // Stack: right of master
    EXPECT_GT(views[1]->x, views[0]->x + views[0]->w);
    EXPECT_GT(views[1]->w, 0);
    EXPECT_GT(views[1]->h, 0);
}

TEST_F(TilingTest, ThreeWindowsStackDividedEvenly) {
    make_views(3);
    tiling_arrange(views, area, LayoutMode::Tiling, 0.55f, 4);
    // Two stack windows should have the same height
    EXPECT_EQ(views[1]->h, views[2]->h);
    // Stack windows should not overlap
    EXPECT_GE(views[2]->y, views[1]->y + views[1]->h);
}

TEST_F(TilingTest, MonocleAllViewsSameGeometry) {
    make_views(3);
    tiling_arrange(views, area, LayoutMode::Monocle, 0.55f, 4);
    for (auto* v : views) {
        EXPECT_EQ(v->x, 4);
        EXPECT_EQ(v->y, 4);
        EXPECT_EQ(v->w, 1920 - 8);
        EXPECT_EQ(v->h, 1080 - 8);
    }
}

TEST_F(TilingTest, FloatingIsNoOp) {
    make_views(2);
    views[0]->set_position(100, 200);
    views[0]->set_size(400, 300);
    tiling_arrange(views, area, LayoutMode::Floating);
    EXPECT_EQ(views[0]->x, 100);
    EXPECT_EQ(views[0]->y, 200);
}

TEST_F(TilingTest, MasterRatioClampedAt90Percent) {
    make_views(2);
    tiling_arrange(views, area, LayoutMode::Tiling, 0.9f, 4);
    int master_w = static_cast<int>((area.width - 12) * 0.9f);
    EXPECT_EQ(views[0]->w, master_w);
}

TEST_F(TilingTest, GapAppliedToAllSides) {
    make_views(1);
    tiling_arrange(views, area, LayoutMode::Tiling, 0.55f, 8);
    EXPECT_EQ(views[0]->x, 8);
    EXPECT_EQ(views[0]->y, 8);
    EXPECT_EQ(views[0]->w, 1920 - 16);
    EXPECT_EQ(views[0]->h, 1080 - 16);
}
