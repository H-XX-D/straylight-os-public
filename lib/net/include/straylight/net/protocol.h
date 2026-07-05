#pragma once

#include <straylight/types.h>
#include <cstdint>

namespace straylight::net {

/// Wire protocol header for tensor transport.
/// All multi-byte fields are little-endian.
/// NOTE: __attribute__((packed)) is GCC/Clang-specific but both are the target
/// compilers for this Linux-only project.
struct __attribute__((packed)) TensorHeader {
    uint32_t magic = 0x53544C54;  // "STLT"
    uint16_t version = 1;
    uint8_t dtype;
    uint8_t ndim;
    uint64_t data_size;
    int64_t shape[8];  // up to 8 dimensions
};
static_assert(sizeof(TensorHeader) == 80, "TensorHeader must be exactly 80 bytes packed");

/// Message types for the StrayLight IPC protocol.
enum class MessageType : uint8_t {
    Ping = 0,
    Pong = 1,
    TensorPublish = 10,
    TensorSubscribe = 11,
    TensorData = 12,
    RegistryGet = 20,
    RegistrySet = 21,
    RegistryValue = 22,
    HealthCheck = 30,
    HealthReport = 31,
};

} // namespace straylight::net
