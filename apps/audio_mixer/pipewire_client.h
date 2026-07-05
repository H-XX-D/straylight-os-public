// apps/audio_mixer/pipewire_client.h
// PipeWire client: connects to the pw_main_loop, enumerates audio nodes,
// and provides volume/mute control via spa_pod parameter sets.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare PipeWire types to avoid polluting headers
struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_node;
struct spa_dict;

namespace straylight::mixer {

/// Per-audio-node metadata cached from PipeWire registry events.
struct PwNodeInfo {
    uint32_t    id       = 0;
    std::string name;           ///< node.name property
    std::string nick;           ///< node.nick (friendly name, if available)
    std::string media_class;    ///< media.class: "Audio/Sink", "Stream/Output/Audio", etc.
    float       volume   = 1.0f;///< channel-average volume [0..1]
    bool        muted    = false;
    bool        active   = true;
};

using NodeListCallback = std::function<void(std::vector<PwNodeInfo>)>;

/// Manages a pw_main_loop and pw_registry connection.
/// Node enumeration and parameter changes run in a background thread.
class PipeWireClient {
public:
    PipeWireClient();
    ~PipeWireClient();

    PipeWireClient(const PipeWireClient&) = delete;
    PipeWireClient& operator=(const PipeWireClient&) = delete;

    /// Initialise PipeWire (must be called once before constructing any client).
    static void pw_init_once(int* argc, char*** argv);

    /// Connect to the default PipeWire daemon and start the event loop thread.
    Result<void, SLError> connect();

    /// Stop the event loop and disconnect.
    void disconnect();

    bool connected() const { return connected_.load(std::memory_order_acquire); }

    /// Register a callback invoked (from the PW thread) when the node list changes.
    void on_nodes_changed(NodeListCallback cb) { nodes_cb_ = std::move(cb); }

    /// Thread-safe snapshot of current node list.
    std::vector<PwNodeInfo> nodes() const;

    /// Set volume [0..1] on a node identified by its PipeWire object ID.
    /// Uses spa_pod to construct a Props parameter.
    Result<void, SLError> set_volume(uint32_t node_id, float volume);

    /// Set mute state on a node.
    Result<void, SLError> set_mute(uint32_t node_id, bool muted);

    // Static C-style PipeWire callbacks.
    static void on_registry_global_static(void* data, uint32_t id, uint32_t permissions,
                                           const char* type, uint32_t version,
                                           const ::spa_dict* props);
    static void on_registry_global_remove_static(void* data, uint32_t id);

private:
    pw_main_loop* loop_     = nullptr;
    pw_context*   context_  = nullptr;
    pw_core*      core_     = nullptr;
    pw_registry*  registry_ = nullptr;

    std::thread           pw_thread_;
    std::atomic<bool>     connected_{false};

    mutable std::mutex    nodes_mutex_;
    std::vector<PwNodeInfo> nodes_;

    NodeListCallback nodes_cb_;

    /// Registry event: global object appeared.
    void on_global(uint32_t id, const char* type, uint32_t version,
                    const ::spa_dict* props);
    /// Registry event: global object removed.
    void on_global_remove(uint32_t id);

    /// Fire the nodes_changed callback with a snapshot.
    void notify_nodes_changed();

    /// Build a spa_pod volume parameter and send it via pw_node_set_param.
    Result<void, SLError> send_volume_param(uint32_t node_id, float volume, bool muted);

};

} // namespace straylight::mixer
