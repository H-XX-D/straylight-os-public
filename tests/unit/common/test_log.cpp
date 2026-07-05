// tests/unit/common/test_log.cpp
#include <gtest/gtest.h>
#include <straylight/log.h>

using namespace straylight;

TEST(LogTest, InitializeCreatesLogger) {
    Log::init("test-app", Log::Level::Debug);
    // Should not throw
    auto logger = Log::get();
    ASSERT_NE(logger, nullptr);
}

TEST(LogTest, LogMacrosDoNotCrash) {
    Log::init("test-macros", Log::Level::Trace);
    // These should all execute without crashing
    SL_TRACE("trace message: {}", 1);
    SL_DEBUG("debug message: {}", 2);
    SL_INFO("info message: {}", 3);
    SL_WARN("warning message: {}", 4);
    SL_ERROR("error message: {}", 5);
}

TEST(LogTest, SubsystemLoggerHasPrefix) {
    Log::init("test-app", Log::Level::Debug);
    auto sub = Log::subsystem("entropy");
    ASSERT_NE(sub, nullptr);
}
