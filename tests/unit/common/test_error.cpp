#include <gtest/gtest.h>
#include <straylight/error.h>
#include <straylight/result.h>

using namespace straylight;

TEST(SLError, DefaultConstructible) {
    SLError err;
    EXPECT_EQ(err.code(), SLErrorCode::Ok);
    EXPECT_TRUE(err.message().empty());
}

TEST(SLError, ConstructWithCodeAndMessage) {
    SLError err{SLErrorCode::NotFound, "file not found"};
    EXPECT_EQ(err.code(), SLErrorCode::NotFound);
    EXPECT_EQ(err.message(), "file not found");
}

TEST(SLErrorResult, VoidResultWithSLError) {
    auto ok_result = Result<void, SLError>::ok();
    EXPECT_TRUE(ok_result.has_value());

    auto err_result = Result<void, SLError>::error(SLError{SLErrorCode::IpcFailed, "connection refused"});
    EXPECT_FALSE(err_result.has_value());
    EXPECT_EQ(err_result.error().code(), SLErrorCode::IpcFailed);
}
