// bin/fuse/tensor_format.cpp
#include "tensor_format.h"

#include <cstring>
#include <numeric>

namespace straylight::fuse_fs {

Result<TensorMeta, std::string>
TensorFormat::parse_header(const std::vector<uint8_t>& data) {
    if (data.size() < MIN_HEADER_SIZE) {
        return Result<TensorMeta, std::string>::error("Data too small for .slt header");
    }

    size_t pos = 0;

    // Magic.
    uint64_t magic;
    std::memcpy(&magic, data.data() + pos, 8);
    pos += 8;
    if (magic != SLT_MAGIC) {
        return Result<TensorMeta, std::string>::error("Invalid .slt magic number");
    }

    // Version.
    uint32_t version;
    std::memcpy(&version, data.data() + pos, 4);
    pos += 4;
    if (version != SLT_VERSION) {
        return Result<TensorMeta, std::string>::error(
            "Unsupported .slt version " + std::to_string(version));
    }

    TensorMeta meta{};

    // Fields.
    if (pos + 4 > data.size()) {
        return Result<TensorMeta, std::string>::error("Truncated header fields");
    }
    meta.dtype = static_cast<TensorDtype>(data[pos++]);
    meta.compression = static_cast<CompressionType>(data[pos++]);
    uint8_t ndim = data[pos++];
    uint8_t name_len = data[pos++];

    // Name.
    if (pos + name_len > data.size()) {
        return Result<TensorMeta, std::string>::error("Truncated tensor name");
    }
    meta.name.assign(reinterpret_cast<const char*>(data.data() + pos), name_len);
    pos += name_len;

    // Shape.
    if (pos + static_cast<size_t>(ndim) * 4 > data.size()) {
        return Result<TensorMeta, std::string>::error("Truncated shape data");
    }
    meta.shape.resize(ndim);
    for (uint8_t i = 0; i < ndim; ++i) {
        std::memcpy(&meta.shape[i], data.data() + pos, 4);
        pos += 4;
    }

    // Sizes.
    if (pos + 16 > data.size()) {
        return Result<TensorMeta, std::string>::error("Truncated size fields");
    }
    std::memcpy(&meta.original_size, data.data() + pos, 8);
    pos += 8;
    std::memcpy(&meta.compressed_size, data.data() + pos, 8);
    pos += 8;

    meta.data_offset = pos;

    return Result<TensorMeta, std::string>::ok(std::move(meta));
}

Result<std::vector<uint8_t>, std::string>
TensorFormat::write_header(const TensorMeta& meta) {
    if (meta.name.size() > 255) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Tensor name too long (max 255 chars)");
    }
    if (meta.shape.size() > 255) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Too many dimensions (max 255)");
    }

    size_t header_size = 8 + 4 + 4 + meta.name.size() + meta.shape.size() * 4 + 16;
    std::vector<uint8_t> out(header_size);
    size_t pos = 0;

    // Magic.
    uint64_t magic = SLT_MAGIC;
    std::memcpy(out.data() + pos, &magic, 8);
    pos += 8;

    // Version.
    uint32_t version = SLT_VERSION;
    std::memcpy(out.data() + pos, &version, 4);
    pos += 4;

    // Fields.
    out[pos++] = static_cast<uint8_t>(meta.dtype);
    out[pos++] = static_cast<uint8_t>(meta.compression);
    out[pos++] = static_cast<uint8_t>(meta.shape.size());
    out[pos++] = static_cast<uint8_t>(meta.name.size());

    // Name.
    std::memcpy(out.data() + pos, meta.name.data(), meta.name.size());
    pos += meta.name.size();

    // Shape.
    for (auto dim : meta.shape) {
        std::memcpy(out.data() + pos, &dim, 4);
        pos += 4;
    }

    // Sizes.
    std::memcpy(out.data() + pos, &meta.original_size, 8);
    pos += 8;
    std::memcpy(out.data() + pos, &meta.compressed_size, 8);
    pos += 8;

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(out));
}

size_t TensorFormat::tensor_byte_size(TensorDtype dtype, const std::vector<uint32_t>& shape) {
    if (shape.empty()) return 0;
    size_t count = std::accumulate(shape.begin(), shape.end(), static_cast<size_t>(1),
                                    std::multiplies<size_t>());
    return count * dtype_size(dtype);
}

size_t TensorFormat::dtype_size(TensorDtype dtype) {
    switch (dtype) {
        case TensorDtype::Float32: return 4;
        case TensorDtype::Float16: return 2;
        case TensorDtype::Int8:    return 1;
        case TensorDtype::Int32:   return 4;
        case TensorDtype::Float64: return 8;
    }
    return 0;
}

} // namespace straylight::fuse_fs
