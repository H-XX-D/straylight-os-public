#pragma once

#include <straylight/export.h>
#include <straylight/types.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace straylight::ml {

/// Owning CPU tensor with contiguous memory.
/// For GPU tensors, see libstraylight-hw VPU allocator.
class STRAYLIGHT_EXPORT Tensor {
public:
    /// Allocate a new zero-initialized tensor.
    explicit Tensor(std::vector<int64_t> shape, DType dtype = DType::Float32,
                    DeviceType device = DeviceType::CPU);

    ~Tensor();

    // Move-only
    Tensor(Tensor&& other) noexcept;
    Tensor& operator=(Tensor&& other) noexcept;
    Tensor(const Tensor&) = delete;
    Tensor& operator=(const Tensor&) = delete;

    /// Raw data pointer. Returns nullptr if moved-from.
    [[nodiscard]] void* data() noexcept { return data_.get(); }
    [[nodiscard]] const void* data() const noexcept { return data_.get(); }

    /// Typed data access.
    template <typename T>
    T* typed_data() noexcept { return static_cast<T*>(data_.get()); }

    [[nodiscard]] const std::vector<int64_t>& shape() const noexcept { return desc_.shape; }
    [[nodiscard]] int64_t numel() const noexcept { return desc_.numel(); }
    [[nodiscard]] size_t nbytes() const noexcept { return desc_.nbytes(); }
    [[nodiscard]] size_t ndim() const noexcept { return desc_.shape.size(); }
    [[nodiscard]] DType dtype() const noexcept { return desc_.dtype; }
    [[nodiscard]] TensorDesc desc() const { return desc_; }

private:
    TensorDesc desc_;
    std::unique_ptr<uint8_t[]> data_;
};

} // namespace straylight::ml
