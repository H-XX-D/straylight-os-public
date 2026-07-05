// services/weave/node_registry.cpp
#include "node_registry.h"

#include <filesystem>
#include <fstream>

namespace straylight {

NodeRegistry::NodeRegistry() {
    register_builtins();
}

void NodeRegistry::register_builtins() {
    // Screen capture is intentionally not registered in the release image until
    // a distro-desktop capture backend replaces the retired StrayLight compositor.

    // vpu-encode: raw video → compressed video via VPU hardware
    register_node({
        .name = "vpu-encode",
        .description = "Hardware video encoding via VPU",
        .input_type = "video/raw",
        .output_type = "video/encoded",
        .config_schema = {{"properties", {
            {"codec", {{"type", "string"}, {"enum", {"h264", "h265", "av1"}}, {"default", "h265"}}},
            {"bitrate", {{"type", "integer"}, {"default", 5000000}}},
            {"quality", {{"type", "integer"}, {"minimum", 0}, {"maximum", 51}}}
        }}},
        .launch_command = "straylight-vpu --encode",
        .is_builtin = true
    });

    // vpu-decode: compressed video → raw video
    register_node({
        .name = "vpu-decode",
        .description = "Hardware video decoding via VPU",
        .input_type = "video/encoded",
        .output_type = "video/raw",
        .config_schema = {{"properties", {
            {"codec", {{"type", "string"}, {"enum", {"h264", "h265", "av1"}}}}
        }}},
        .launch_command = "straylight-vpu --decode",
        .is_builtin = true
    });

    // mesh-broadcast: any data → multicast to mesh cluster
    register_node({
        .name = "mesh-broadcast",
        .description = "Broadcast data to mesh cluster via multicast",
        .input_type = "any",
        .output_type = "none",
        .config_schema = {{"properties", {
            {"group", {{"type", "string"}}},
            {"reliable", {{"type", "boolean"}, {"default", true}}},
            {"compress", {{"type", "boolean"}, {"default", false}}}
        }}},
        .launch_command = "straylight-mesh --broadcast",
        .is_builtin = true
    });

    // flux-publish: any data → publish to flux stream
    register_node({
        .name = "flux-publish",
        .description = "Publish data to a flux stream",
        .input_type = "any",
        .output_type = "none",
        .config_schema = {{"properties", {
            {"stream", {{"type", "string"}}},
            {"buffer_size", {{"type", "integer"}, {"default", 1000}}}
        }}},
        .launch_command = "straylight-flux-cli publish",
        .is_builtin = true
    });

    // file-sink: any data → write to file
    register_node({
        .name = "file-sink",
        .description = "Write pipeline data to file",
        .input_type = "any",
        .output_type = "none",
        .config_schema = {{"properties", {
            {"path", {{"type", "string"}}},
            {"append", {{"type", "boolean"}, {"default", false}}},
            {"rotate_mb", {{"type", "integer"}, {"default", 0}}}
        }}},
        .launch_command = "straylight-weave-sink",
        .is_builtin = true
    });

    // file-source: read from file → data
    register_node({
        .name = "file-source",
        .description = "Read data from file into pipeline",
        .input_type = "none",
        .output_type = "data/bytes",
        .config_schema = {{"properties", {
            {"path", {{"type", "string"}}},
            {"loop", {{"type", "boolean"}, {"default", false}}},
            {"rate_limit", {{"type", "integer"}, {"description", "bytes per second, 0=unlimited"}}}
        }}},
        .launch_command = "straylight-weave-source",
        .is_builtin = true
    });

    // filter: apply flux filter expression
    register_node({
        .name = "filter",
        .description = "Filter data using flux filter expression",
        .input_type = "data/json",
        .output_type = "data/json",
        .config_schema = {{"properties", {
            {"expression", {{"type", "string"}}},
            {"drop_unmatched", {{"type", "boolean"}, {"default", true}}}
        }}},
        .launch_command = "straylight-flux-cli filter",
        .is_builtin = true
    });

    // transform: apply flux transform
    register_node({
        .name = "transform",
        .description = "Transform data using flux transform expression",
        .input_type = "data/json",
        .output_type = "data/json",
        .config_schema = {{"properties", {
            {"expression", {{"type", "string"}}},
            {"path", {{"type", "string"}}}
        }}},
        .launch_command = "straylight-flux-cli transform",
        .is_builtin = true
    });

    // whisper-send: encrypted IPC send
    register_node({
        .name = "whisper-send",
        .description = "Send data via encrypted IPC (whisper)",
        .input_type = "any",
        .output_type = "none",
        .config_schema = {{"properties", {
            {"channel", {{"type", "string"}}},
            {"recipient", {{"type", "string"}}},
            {"encrypt", {{"type", "boolean"}, {"default", true}}}
        }}},
        .launch_command = "straylight-whisper-cli send",
        .is_builtin = true
    });

    // alice-analyze: send to Alice for AI analysis
    register_node({
        .name = "alice-analyze",
        .description = "Send data to Alice AI for analysis",
        .input_type = "any",
        .output_type = "data/json",
        .config_schema = {{"properties", {
            {"model", {{"type", "string"}, {"default", "default"}}},
            {"prompt", {{"type", "string"}}},
            {"max_tokens", {{"type", "integer"}, {"default", 1024}}}
        }}},
        .launch_command = "straylight-alice-cli analyze",
        .is_builtin = true
    });
}

Result<int, std::string> NodeRegistry::scan_plugins() {
    if (!std::filesystem::exists(PLUGIN_DIR)) {
        return Result<int, std::string>::ok(0);
    }

    int loaded = 0;

    for (const auto& entry : std::filesystem::directory_iterator(PLUGIN_DIR)) {
        if (entry.path().extension() != ".json") continue;

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) continue;

        nlohmann::json j;
        try {
            ifs >> j;
        } catch (...) {
            continue;
        }

        NodeType node;
        node.name = j.value("name", "");
        if (node.name.empty()) continue;

        node.description = j.value("description", "");
        node.input_type = j.value("input_type", "any");
        node.output_type = j.value("output_type", "any");
        node.config_schema = j.value("config_schema", nlohmann::json::object());
        node.launch_command = j.value("launch_command", "");
        node.is_builtin = false;

        auto result = register_node(std::move(node));
        if (result.has_value()) {
            ++loaded;
        }
    }

    return Result<int, std::string>::ok(loaded);
}

Result<void, std::string> NodeRegistry::register_node(NodeType node) {
    if (node.name.empty()) {
        return Result<void, std::string>::error("Node name cannot be empty");
    }

    nodes_[node.name] = std::move(node);
    return Result<void, std::string>::ok();
}

Result<void, std::string> NodeRegistry::unregister_node(const std::string& name) {
    auto it = nodes_.find(name);
    if (it == nodes_.end()) {
        return Result<void, std::string>::error("Node type not found: " + name);
    }
    nodes_.erase(it);
    return Result<void, std::string>::ok();
}

Result<const NodeType*, std::string> NodeRegistry::get(const std::string& name) const {
    auto it = nodes_.find(name);
    if (it == nodes_.end()) {
        return Result<const NodeType*, std::string>::error(
            "Unknown node type: " + name);
    }
    return Result<const NodeType*, std::string>::ok(&it->second);
}

bool NodeRegistry::has(const std::string& name) const {
    return nodes_.find(name) != nodes_.end();
}

std::vector<const NodeType*> NodeRegistry::list() const {
    std::vector<const NodeType*> result;
    result.reserve(nodes_.size());
    for (const auto& [name, node] : nodes_) {
        result.push_back(&node);
    }
    return result;
}

bool NodeRegistry::types_compatible(const std::string& output_type,
                                     const std::string& input_type) const {
    // "any" matches everything
    if (output_type == "any" || input_type == "any") return true;

    // "none" is a sink — no downstream
    if (output_type == "none") return false;

    // Exact match
    if (output_type == input_type) return true;

    // Category match: "data/*" matches "data/json", "data/bytes", etc.
    auto out_slash = output_type.find('/');
    auto in_slash = input_type.find('/');

    if (out_slash != std::string::npos && in_slash != std::string::npos) {
        std::string out_cat = output_type.substr(0, out_slash);
        std::string in_cat = input_type.substr(0, in_slash);

        // "data/bytes" can feed into "data/json" (parsing happens in the node)
        if (out_cat == in_cat) return true;
    }

    return false;
}

} // namespace straylight
