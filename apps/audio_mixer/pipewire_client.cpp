// apps/audio_mixer/pipewire_client.cpp
// PipeWire client implementation: node enumeration and volume control via spa_pod.
#include "pipewire_client.h"

#include <straylight/log.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace straylight::mixer {

// ---------------------------------------------------------------------------
// Static pw_init
// ---------------------------------------------------------------------------

void PipeWireClient::pw_init_once(int* argc, char*** argv) {
    pw_init(argc, argv);
}

// ---------------------------------------------------------------------------
// Registry listener (static C callbacks)
// ---------------------------------------------------------------------------

static const pw_registry_events registry_events_impl = {
    .version       = PW_VERSION_REGISTRY_EVENTS,
    .global        = PipeWireClient::on_registry_global_static,
    .global_remove = PipeWireClient::on_registry_global_remove_static,
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PipeWireClient::PipeWireClient() = default;

PipeWireClient::~PipeWireClient() {
    disconnect();
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------

Result<void, SLError> PipeWireClient::connect() {
    loop_ = pw_main_loop_new(nullptr);
    if (!loop_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pw_main_loop_new failed"});
    }

    context_ = pw_context_new(pw_main_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
        pw_main_loop_destroy(loop_); loop_ = nullptr;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pw_context_new failed"});
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        pw_context_destroy(context_); context_ = nullptr;
        pw_main_loop_destroy(loop_);  loop_    = nullptr;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pw_context_connect failed"});
    }

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (!registry_) {
        pw_core_disconnect(core_); core_ = nullptr;
        pw_context_destroy(context_); context_ = nullptr;
        pw_main_loop_destroy(loop_);  loop_    = nullptr;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "pw_core_get_registry failed"});
    }

    // Add listener (spa_hook managed inline — lifetime bound to registry_)
    static struct spa_hook registry_hook;
    static struct spa_hook_list hook_list;
    spa_hook_list_init(&hook_list);
    pw_registry_add_listener(registry_, &registry_hook,
                              &registry_events_impl, this);

    connected_.store(true, std::memory_order_release);

    // Run the main loop in a background thread
    pw_thread_ = std::thread([this] {
        pw_main_loop_run(loop_);
    });

    // Sync: wait for initial registry enumeration to complete
    // (One roundtrip is sufficient for initial node list)
    pw_core_sync(core_, PW_ID_CORE, 0);

    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// disconnect
// ---------------------------------------------------------------------------

void PipeWireClient::disconnect() {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;

    if (loop_) pw_main_loop_quit(loop_);
    if (pw_thread_.joinable()) pw_thread_.join();

    if (registry_) { pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_)); registry_ = nullptr; }
    if (core_)     { pw_core_disconnect(core_);  core_    = nullptr; }
    if (context_)  { pw_context_destroy(context_); context_ = nullptr; }
    if (loop_)     { pw_main_loop_destroy(loop_); loop_     = nullptr; }
}

// ---------------------------------------------------------------------------
// nodes
// ---------------------------------------------------------------------------

std::vector<PwNodeInfo> PipeWireClient::nodes() const {
    std::lock_guard<std::mutex> lk(nodes_mutex_);
    return nodes_;
}

// ---------------------------------------------------------------------------
// set_volume / set_mute
// ---------------------------------------------------------------------------

Result<void, SLError> PipeWireClient::set_volume(uint32_t node_id, float volume) {
    return send_volume_param(node_id, volume, false);
}

Result<void, SLError> PipeWireClient::set_mute(uint32_t node_id, bool muted) {
    // Retrieve current volume for the node
    float vol = 1.0f;
    {
        std::lock_guard<std::mutex> lk(nodes_mutex_);
        for (auto& n : nodes_) {
            if (n.id == node_id) { vol = n.volume; break; }
        }
    }
    return send_volume_param(node_id, vol, muted);
}

// ---------------------------------------------------------------------------
// send_volume_param — build spa_pod and call pw_node_set_param
// ---------------------------------------------------------------------------

Result<void, SLError> PipeWireClient::send_volume_param(uint32_t node_id,
                                                          float volume,
                                                          bool muted) {
    if (!connected_.load(std::memory_order_acquire)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "Not connected to PipeWire"});
    }

    // Build a spa_pod with SPA_PROP_volume and SPA_PROP_mute
    uint8_t buf[512];
    struct spa_pod_builder b;
    spa_pod_builder_init(&b, buf, sizeof(buf));

    // Single channel volume (could be extended to multi-channel)
    const float vols[1] = {std::clamp(volume, 0.0f, 1.0f)};

    struct spa_pod* param = static_cast<struct spa_pod*>(
        spa_pod_builder_add_object(
            &b,
            SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_volume,        SPA_POD_Float(vols[0]),
            SPA_PROP_mute,          SPA_POD_Bool(muted),
            0));

    if (!param) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "spa_pod_builder_add_object failed"});
    }

    // Get the proxy for this node id from the registry
    // In real usage we'd cache proxies; here we do a sync lookup via pw_registry_bind
    struct pw_node* node_proxy = static_cast<struct pw_node*>(
        pw_registry_bind(registry_, node_id, PW_TYPE_INTERFACE_Node,
                         PW_VERSION_NODE, 0));
    if (!node_proxy) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "pw_registry_bind failed for node"});
    }

    pw_node_set_param(node_proxy, SPA_PARAM_Props, 0, param);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node_proxy));

    // Update our local cache
    {
        std::lock_guard<std::mutex> lk(nodes_mutex_);
        for (auto& n : nodes_) {
            if (n.id == node_id) {
                n.volume = volume;
                n.muted  = muted;
                break;
            }
        }
    }

    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// notify_nodes_changed
// ---------------------------------------------------------------------------

void PipeWireClient::notify_nodes_changed() {
    if (nodes_cb_) {
        std::vector<PwNodeInfo> snap;
        {
            std::lock_guard<std::mutex> lk(nodes_mutex_);
            snap = nodes_;
        }
        nodes_cb_(std::move(snap));
    }
}

// ---------------------------------------------------------------------------
// on_global — handle new objects appearing in the registry
// ---------------------------------------------------------------------------

void PipeWireClient::on_global(uint32_t id, const char* type,
                                uint32_t /*version*/,
                                const ::spa_dict* props) {
    // We only care about PW_TYPE_INTERFACE_Node
    if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    PwNodeInfo info;
    info.id = id;

    if (props) {
        const char* val = nullptr;
        if ((val = spa_dict_lookup(props, PW_KEY_NODE_NAME)))      info.name = val;
        if ((val = spa_dict_lookup(props, PW_KEY_NODE_NICK)))      info.nick = val;
        if ((val = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)))    info.media_class = val;
        if ((val = spa_dict_lookup(props, "node.description")))    {
            if (info.nick.empty()) info.nick = val;
        }
    }

    // Filter: only show audio sinks and output streams
    if (info.media_class.empty()) return;
    const bool is_audio = (info.media_class.find("Audio") != std::string::npos ||
                            info.media_class.find("Stream/Output") != std::string::npos);
    if (!is_audio) return;

    if (info.nick.empty()) info.nick = info.name;
    info.volume = 1.0f;
    info.muted  = false;
    info.active = true;

    {
        std::lock_guard<std::mutex> lk(nodes_mutex_);
        // Avoid duplicates
        for (const auto& n : nodes_)
            if (n.id == id) return;
        nodes_.push_back(info);
    }
    notify_nodes_changed();
}

void PipeWireClient::on_global_remove(uint32_t id) {
    {
        std::lock_guard<std::mutex> lk(nodes_mutex_);
        auto it = std::remove_if(nodes_.begin(), nodes_.end(),
                                  [id](const PwNodeInfo& n) { return n.id == id; });
        if (it == nodes_.end()) return;
        nodes_.erase(it, nodes_.end());
    }
    notify_nodes_changed();
}

// ---------------------------------------------------------------------------
// Static callbacks
// ---------------------------------------------------------------------------

void PipeWireClient::on_registry_global_static(void* data, uint32_t id,
                                                uint32_t /*permissions*/,
                                                const char* type, uint32_t version,
                                                const ::spa_dict* props) {
    static_cast<PipeWireClient*>(data)->on_global(id, type, version, props);
}

void PipeWireClient::on_registry_global_remove_static(void* data, uint32_t id) {
    static_cast<PipeWireClient*>(data)->on_global_remove(id);
}

} // namespace straylight::mixer
