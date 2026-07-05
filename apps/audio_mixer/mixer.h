// apps/audio_mixer/mixer.h
// Per-application volume model with peak-meter ring buffers and device routing.
#pragma once

#include "pipewire_client.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::mixer {

/// Per-sample peak data for a single audio stream (updated from PipeWire events).
struct PeakMeter {
    static constexpr int kRingLen = 64;

    /// Ring buffer of linear peak values [0..1].
    std::array<float, kRingLen> ring{};
    int                          head = 0;
    float                        peak_hold = 0.0f;
    int                          hold_frames = 0;

    void push(float linear_peak);
    float current_peak() const { return ring[static_cast<size_t>((head - 1 + kRingLen) % kRingLen)]; }
};

/// Full mixer model: wraps PipeWireClient and adds volume/peak state.
class AudioMixer {
public:
    explicit AudioMixer(PipeWireClient& client);

    /// Sync node list from PipeWire client into our model.
    void sync_nodes();

    /// Per-app volume slider value [0..1].
    float get_volume(uint32_t node_id) const;
    void  set_volume(uint32_t node_id, float vol);

    /// Mute state.
    bool get_muted(uint32_t node_id) const;
    void set_mute(uint32_t node_id, bool muted);

    /// Simulated peak meter (derived from volume level changes for demo purposes).
    float get_peak(uint32_t node_id) const;

    /// Update meters (call each frame; simulates peak decay).
    void update_meters();

    /// Names of available output sinks (for device routing dropdown).
    std::vector<std::string> sink_names() const;
    std::vector<uint32_t>    sink_ids()   const;

    const std::vector<PwNodeInfo>& node_infos() const;

private:
    PipeWireClient& client_;

    mutable std::mutex       mtx_;
    std::vector<PwNodeInfo>  nodes_;

    struct NodeState {
        uint32_t id     = 0;
        float    volume = 1.0f;
        bool     muted  = false;
        PeakMeter meter;
        // Simulated audio activity — just derived from volume changes
        float    simulated_activity = 0.0f;
    };
    std::vector<NodeState> states_;

    NodeState* find_state(uint32_t id);
    const NodeState* find_state(uint32_t id) const;
};

} // namespace straylight::mixer
