#include <straylight/ml/tensor.h>
#include <cstring>

namespace straylight::ml {

Tensor::Tensor(std::vector<int64_t> shape, DType dtype, DeviceType device)
    : desc_{std::move(shape), dtype, device, 0} {
    auto bytes = desc_.nbytes();
    data_ = std::make_unique<uint8_t[]>(bytes);
    std::memset(data_.get(), 0, bytes);
}

Tensor::~Tensor() = default;

Tensor::Tensor(Tensor&& other) noexcept
    : desc_(std::move(other.desc_)), data_(std::move(other.data_)) {}

Tensor& Tensor::operator=(Tensor&& other) noexcept {
    if (this != &other) {
        desc_ = std::move(other.desc_);
        data_ = std::move(other.data_);
    }
    return *this;
}

} // namespace straylight::ml
