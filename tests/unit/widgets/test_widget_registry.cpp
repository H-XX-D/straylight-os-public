// tests/unit/widgets/test_widget_registry.cpp
#include <gtest/gtest.h>
#include "widget_registry.h"

using namespace straylight;
using namespace straylight::widgets;

namespace {

class DummyWidget : public WidgetBase {
public:
    const char* name() const override { return "Dummy"; }
    void update() override { updated_ = true; }
    void render(bool* /*p_open*/) override { rendered_ = true; }
    float poll_interval() const override { return 0.5f; }

    bool updated_ = false;
    bool rendered_ = false;
};

class AnotherWidget : public WidgetBase {
public:
    const char* name() const override { return "Another"; }
    void update() override {}
    void render(bool* /*p_open*/) override {}
};

} // namespace

class WidgetRegistryTest : public ::testing::Test {
protected:
    // We test against the global singleton — entries accumulate, which is fine
    // since we only care about our test registrations existing.
};

// Use the registrar pattern to register our test widgets
static WidgetRegistrar<DummyWidget> reg_dummy("test_dummy", "Test Dummy", WidgetCategory::System, 0.5f);
static WidgetRegistrar<AnotherWidget> reg_another("test_another", "Test Another", WidgetCategory::ML);

TEST_F(WidgetRegistryTest, RegistryContainsRegisteredWidgets) {
    auto& reg = WidgetRegistry::instance();
    EXPECT_TRUE(reg.has("test_dummy"));
    EXPECT_TRUE(reg.has("test_another"));
    EXPECT_FALSE(reg.has("nonexistent_widget"));
}

TEST_F(WidgetRegistryTest, CreateReturnsCorrectType) {
    auto& reg = WidgetRegistry::instance();
    auto w = reg.create("test_dummy");
    ASSERT_NE(w, nullptr);
    EXPECT_STREQ(w->name(), "Dummy");
}

TEST_F(WidgetRegistryTest, CreateUnknownReturnsNull) {
    auto& reg = WidgetRegistry::instance();
    auto w = reg.create("no_such_widget");
    EXPECT_EQ(w, nullptr);
}

TEST_F(WidgetRegistryTest, CategoryFilterWorks) {
    auto& reg = WidgetRegistry::instance();
    auto sys_widgets = reg.by_category(WidgetCategory::System);
    bool found_dummy = false;
    for (const auto* entry : sys_widgets) {
        if (entry->id == "test_dummy") {
            found_dummy = true;
            EXPECT_EQ(entry->display_name, "Test Dummy");
            EXPECT_FLOAT_EQ(entry->default_poll_interval, 0.5f);
        }
    }
    EXPECT_TRUE(found_dummy);
}

TEST_F(WidgetRegistryTest, WidgetUpdateAndRender) {
    auto& reg = WidgetRegistry::instance();
    auto w = reg.create("test_dummy");
    ASSERT_NE(w, nullptr);

    auto* dummy = dynamic_cast<DummyWidget*>(w.get());
    ASSERT_NE(dummy, nullptr);

    EXPECT_FALSE(dummy->updated_);
    EXPECT_FALSE(dummy->rendered_);

    dummy->update();
    EXPECT_TRUE(dummy->updated_);

    bool open = true;
    dummy->render(&open);
    EXPECT_TRUE(dummy->rendered_);
}

TEST_F(WidgetRegistryTest, SizeReflectsRegistrations) {
    auto& reg = WidgetRegistry::instance();
    // At minimum we have our 2 test widgets + whatever the real widgets register
    EXPECT_GE(reg.size(), 2u);
}

TEST_F(WidgetRegistryTest, CategoryNameStrings) {
    EXPECT_STREQ(category_name(WidgetCategory::ML), "Machine Learning");
    EXPECT_STREQ(category_name(WidgetCategory::HPC), "HPC");
    EXPECT_STREQ(category_name(WidgetCategory::System), "System");
    EXPECT_STREQ(category_name(WidgetCategory::Research), "Research");
}
