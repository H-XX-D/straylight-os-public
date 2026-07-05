// lib/common/include/straylight/log.h
#pragma once

#include <straylight/export.h>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace straylight {

/// Centralized logging facade wrapping spdlog.
/// Initialize once at startup, then use SL_* macros everywhere.
class STRAYLIGHT_EXPORT Log {
public:
    enum class Level { Trace, Debug, Info, Warn, Error, Critical, Off };

    /// Initialize the global logger. Call once in main().
    static void init(std::string_view app_name, Level level = Level::Info);

    /// Get the global logger instance.
    static std::shared_ptr<spdlog::logger> get();

    /// Create a named subsystem logger (inherits sinks from global).
    static std::shared_ptr<spdlog::logger> subsystem(std::string_view name);

    /// Set global log level at runtime.
    static void set_level(Level level);
};

} // namespace straylight

// Convenience macros — use these everywhere instead of spdlog directly.
#define SL_TRACE(...)    SPDLOG_LOGGER_TRACE(::straylight::Log::get(), __VA_ARGS__)
#define SL_DEBUG(...)    SPDLOG_LOGGER_DEBUG(::straylight::Log::get(), __VA_ARGS__)
#define SL_INFO(...)     SPDLOG_LOGGER_INFO(::straylight::Log::get(), __VA_ARGS__)
#define SL_WARN(...)     SPDLOG_LOGGER_WARN(::straylight::Log::get(), __VA_ARGS__)
#define SL_ERROR(...)    SPDLOG_LOGGER_ERROR(::straylight::Log::get(), __VA_ARGS__)
#define SL_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(::straylight::Log::get(), __VA_ARGS__)
