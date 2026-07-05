// services/audio/main.cpp
// straylight-audio daemon — audio routing and management service.
#include "audio_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

#include <chrono>
#include <thread>

namespace straylight {

/// Audio daemon — wraps AudioEngine in the DaemonBase lifecycle.
class AudioDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("audio: initializing daemon");

        tick_interval_s_ = cfg.get<int>("tick_interval_seconds", 5);
        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/audio.sock");

        auto res = engine_.init();
        if (!res.has_value()) {
            SL_ERROR("audio: failed to initialize engine: {}", res.error());
            return Result<void, SLError>::error(
                SLError{SLErrorCode::NotInitialized, res.error()});
        }

        auto devices = engine_.list_devices();
        if (devices.has_value()) {
            int sinks = 0, sources = 0;
            for (const auto& d : devices.value()) {
                if (d.type == AudioDevice::Type::Sink) ++sinks;
                else ++sources;
            }
            SL_INFO("audio: detected {} sink(s), {} source(s)", sinks, sources);
        }

        auto streams = engine_.list_streams();
        if (streams.has_value()) {
            SL_INFO("audio: tracking {} active stream(s)", streams.value().size());
        }

        SL_INFO("audio: daemon initialized (tick={}s)", tick_interval_s_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto res = engine_.refresh();
        if (!res.has_value()) {
            SL_WARN("audio: refresh failed: {}", res.error());
        }

        std::this_thread::sleep_for(std::chrono::seconds(tick_interval_s_));
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("audio: shutting down");
        SL_INFO("audio: shutdown complete");
    }

private:
    AudioEngine engine_;
    std::string socket_path_;
    int tick_interval_s_ = 5;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-audio");

    auto cfg_result = straylight::Config::load("/etc/straylight/audio.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("audio: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::AudioDaemon daemon;
    return daemon.run(cfg_result.value());
}
