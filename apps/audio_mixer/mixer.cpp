// apps/audio_mixer/mixer.cpp
// AudioMixer implementation — wraps PipeWireClient with UI-facing model.
#include "mixer.h"

#include <algorithm>
#include <cmath>

namespace straylight::mixer {

// ---------------------------------------------------------------------------
// PeakMeter
// ---------------------------------------------------------------------------

void PeakMeter::push(float linear_peak) {
    ring[static_cast<size_t>(head)] = linear_peak;
    head = (head + 1) % kRingLen;

    // Peak hold
    if (linear_peak >= peak_hold) {
        peak_hold   = linear_peak;
        hold_frames = 45;
    } else {
        if (hold_frames > 0) --hold_frames;
        else peak_hold = std::max(0.0f, peak_hold - 0.008f);
    }
}

// ---------------------------------------------------------------------------
// AudioMixer
// ---------------------------------------------------------------------------

AudioMixer::AudioMixer(PipeWireClient& client) : client_(client) {
    // Register for node changes
    client_.on_nodes_changed([this](std::vector<PwNodeInfo> nodes) {
        std::lock_guard<std::mutex> lk(mtx_);
        nodes_ = std::move(nodes);
        // Merge states: preserve existing, add new, remove stale
        for (const auto& n : nodes_) {
            bool found = false;
            for (auto& s : states_) {
                if (s.id == n.id) { found = true; break; }
            }
            if (!found) {
                NodeState ns;
                ns.id     = n.id;
                ns.volume = n.volume;
                ns.muted  = n.muted;
                states_.push_back(std::move(ns));
            }
        }
        // Remove states for nodes no longer present
        states_.erase(
            std::remove_if(states_.begin(), states_.end(), [this](const NodeState& s) {
                for (const auto& n : nodes_)
                    if (n.id == s.id) return false;
                return true;
            }),
            states_.end());
    });
}

void AudioMixer::sync_nodes() {
    auto fresh = client_.nodes();
    std::lock_guard<std::mutex> lk(mtx_);
    nodes_ = std::move(fresh);
    for (const auto& n : nodes_) {
        bool found = false;
        for (auto& s : states_)
            if (s.id == n.id) { found = true; break; }
        if (!found) {
            NodeState ns;
            ns.id = n.id; ns.volume = n.volume; ns.muted = n.muted;
            states_.push_back(std::move(ns));
        }
    }
}

AudioMixer::NodeState* AudioMixer::find_state(uint32_t id) {
    for (auto& s : states_) if (s.id == id) return &s;
    return nullptr;
}

const AudioMixer::NodeState* AudioMixer::find_state(uint32_t id) const {
    for (const auto& s : states_) if (s.id == id) return &s;
    return nullptr;
}

float AudioMixer::get_volume(uint32_t node_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const NodeState* s = find_state(node_id);
    return s ? s->volume : 1.0f;
}

void AudioMixer::set_volume(uint32_t node_id, float vol) {
    vol = std::clamp(vol, 0.0f, 1.0f);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        NodeState* s = find_state(node_id);
        if (s) {
            s->volume = vol;
            s->simulated_activity = vol; // drive peak meter from volume activity
        }
    }
    client_.set_volume(node_id, vol);
}

bool AudioMixer::get_muted(uint32_t node_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const NodeState* s = find_state(node_id);
    return s ? s->muted : false;
}

void AudioMixer::set_mute(uint32_t node_id, bool muted) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        NodeState* s = find_state(node_id);
        if (s) s->muted = muted;
    }
    client_.set_mute(node_id, muted);
}

float AudioMixer::get_peak(uint32_t node_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const NodeState* s = find_state(node_id);
    return s ? s->meter.current_peak() : 0.0f;
}

void AudioMixer::update_meters() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : states_) {
        if (s.muted) {
            s.meter.push(0.0f);
            continue;
        }
        // Simulate activity: random flutter around volume level
        // (In a real system we'd read the PipeWire meter events)
        const float base    = s.volume;
        const float flutter = base * 0.15f *
            std::abs(std::sin(static_cast<float>(s.id * 7 +
                               std::hash<std::string>{}(std::to_string(s.id)))));
        s.simulated_activity = std::max(0.0f, base - flutter + flutter * 0.5f);
        s.meter.push(s.simulated_activity);
    }
}

std::vector<std::string> AudioMixer::sink_names() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> names;
    for (const auto& n : nodes_) {
        if (n.media_class.find("Audio/Sink") != std::string::npos) {
            names.push_back(n.nick.empty() ? n.name : n.nick);
        }
    }
    return names;
}

std::vector<uint32_t> AudioMixer::sink_ids() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<uint32_t> ids;
    for (const auto& n : nodes_)
        if (n.media_class.find("Audio/Sink") != std::string::npos)
            ids.push_back(n.id);
    return ids;
}

const std::vector<PwNodeInfo>& AudioMixer::node_infos() const {
    return nodes_;
}

} // namespace straylight::mixer
