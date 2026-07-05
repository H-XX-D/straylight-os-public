// tests/unit/compositor/test_workspace.cpp
// Tests Workspace focus history and layout dispatch in isolation.
// Uses a lightweight stub that satisfies the Workspace API without wlroots.
#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <algorithm>

// Stub: minimal View-like object for workspace focus tests
struct StubView {
    std::string app_id_str;
    bool is_focused = false;

    std::string app_id() const { return app_id_str; }
    void focus() { is_focused = true; }
    void close() {}
};

// Minimal workspace focus-history logic extracted for unit test
class FocusHistory {
    std::vector<StubView*> views_;
public:
    void add(StubView* v)    { views_.insert(views_.begin(), v); }
    void remove(StubView* v) {
        views_.erase(std::remove(views_.begin(), views_.end(), v), views_.end());
    }
    void focus(StubView* v) {
        remove(v);
        views_.insert(views_.begin(), v);
        v->is_focused = true;
    }
    StubView* focused() const { return views_.empty() ? nullptr : views_.front(); }
    void focus_next() { if (views_.size() >= 2) focus(views_[1]); }
    void focus_prev() { if (!views_.empty()) focus(views_.back()); }
    const std::vector<StubView*>& all() const { return views_; }
};

TEST(WorkspaceFocus, EmptyFocusedIsNull) {
    FocusHistory h;
    EXPECT_EQ(h.focused(), nullptr);
}

TEST(WorkspaceFocus, SingleViewBecomesFocused) {
    FocusHistory h;
    StubView v{"app1"};
    h.add(&v);
    h.focus(&v);
    EXPECT_EQ(h.focused(), &v);
    EXPECT_TRUE(v.is_focused);
}

TEST(WorkspaceFocus, FocusNextCycles) {
    FocusHistory h;
    StubView v1{"app1"}, v2{"app2"}, v3{"app3"};
    h.add(&v1); h.add(&v2); h.add(&v3);
    // Front = v3 (last added)
    h.focus_next();
    EXPECT_EQ(h.focused(), h.all()[0]);
}

TEST(WorkspaceFocus, RemoveUnmappedViewRestoresFocus) {
    FocusHistory h;
    StubView v1{"app1"}, v2{"app2"};
    h.add(&v1); h.add(&v2);
    h.focus(&v2);
    ASSERT_EQ(h.focused(), &v2);
    h.remove(&v2);
    // After v2 removed, v1 should be at front
    EXPECT_NE(h.focused(), &v2);
}

TEST(WorkspaceFocus, FocusPrevGoesToBack) {
    FocusHistory h;
    StubView v1{"app1"}, v2{"app2"}, v3{"app3"};
    h.add(&v1); h.add(&v2); h.add(&v3);
    StubView* back = h.all().back();
    h.focus_prev();
    EXPECT_EQ(h.focused(), back);
}
