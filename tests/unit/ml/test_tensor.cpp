#include <gtest/gtest.h>
#include <straylight/ml/tensor.h>

using namespace straylight;
using namespace straylight::ml;

TEST(TensorTest, CreateFromShape) {
    Tensor t({2, 3, 4}, DType::Float32);
    EXPECT_EQ(t.numel(), 24);
    EXPECT_EQ(t.nbytes(), 96);
    EXPECT_EQ(t.ndim(), 3);
    EXPECT_EQ(t.shape()[0], 2);
}

TEST(TensorTest, DataPointerIsValid) {
    Tensor t({10}, DType::Float32);
    ASSERT_NE(t.data(), nullptr);
    auto* ptr = static_cast<float*>(t.data());
    ptr[0] = 3.14f;
    EXPECT_FLOAT_EQ(ptr[0], 3.14f);
}

TEST(TensorTest, MoveSemantics) {
    Tensor a({4, 4}, DType::Float32);
    auto* ptr = a.data();
    Tensor b = std::move(a);
    EXPECT_EQ(b.data(), ptr);
    EXPECT_EQ(a.data(), nullptr);
}

TEST(TensorTest, DescReturnsCorrectMetadata) {
    Tensor t({8, 16}, DType::Int8, DeviceType::CPU);
    auto desc = t.desc();
    EXPECT_EQ(desc.shape.size(), 2u);
    EXPECT_EQ(desc.dtype, DType::Int8);
    EXPECT_EQ(desc.nbytes(), 128u);
}
