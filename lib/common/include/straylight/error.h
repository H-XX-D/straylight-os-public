// lib/common/include/straylight/error.h
#pragma once

#include <cstdint>
#include <string>

namespace straylight {

/// Unified error codes used across all StrayLight subsystems.
enum class SLErrorCode : uint8_t {
    Ok = 0,
    NotFound = 1,
    AlreadyExists = 2,
    PermissionDenied = 3,
    ParseError = 4,
    IpcFailed = 5,
    NotInitialized = 6,
    HardwareFault = 7,
    Timeout = 8,
    InvalidArgument = 9,
    Internal = 10,
    IOError = 11,
};

/// Structured error type carrying a code and human-readable message.
struct SLError {
    SLErrorCode code_ = SLErrorCode::Ok;
    std::string message_;

    SLErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }
};

} // namespace straylight
