// lib/common/src/log.cpp
#include <straylight/log.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <mutex>

namespace straylight {

namespace {
std::shared_ptr<spdlog::logger> g_logger;

spdlog::level::level_enum to_spdlog(Log::Level level) {
    switch (level) {
        case Log::Level::Trace:    return spdlog::level::trace;
        case Log::Level::Debug:    return spdlog::level::debug;
        case Log::Level::Info:     return spdlog::level::info;
        case Log::Level::Warn:     return spdlog::level::warn;
        case Log::Level::Error:    return spdlog::level::err;
        case Log::Level::Critical: return spdlog::level::critical;
        case Log::Level::Off:      return spdlog::level::off;
    }
    return spdlog::level::info;
}
} // namespace

void Log::init(std::string_view app_name, Level level) {
    // Allow re-init in tests
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

    g_logger = std::make_shared<spdlog::logger>(
        std::string(app_name), console_sink);
    g_logger->set_level(to_spdlog(level));
    g_logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> Log::get() {
    if (!g_logger) {
        static std::once_flag flag;
        std::call_once(flag, []() {
            if (!g_logger) {
                init("straylight", Level::Info);
            }
        });
    }
    return g_logger;
}

std::shared_ptr<spdlog::logger> Log::subsystem(std::string_view name) {
    auto parent = get();
    auto sub = std::make_shared<spdlog::logger>(
        std::string(name), parent->sinks().begin(), parent->sinks().end());
    sub->set_level(parent->level());
    return sub;
}

void Log::set_level(Level level) {
    if (g_logger) {
        g_logger->set_level(to_spdlog(level));
    }
}

} // namespace straylight
