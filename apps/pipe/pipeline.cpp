// apps/pipe/pipeline.cpp
// StrayLight Pipe — Pipeline engine implementation.
#include "pipeline.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace straylight::pipe {

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

NodeCategory category_of(NodeType type) {
    switch (type) {
        case NodeType::FileInput:
        case NodeType::CameraInput:
        case NodeType::MicInput:
        case NodeType::NetworkInput:
        case NodeType::GpuBuffer:
            return NodeCategory::Source;

        case NodeType::Resize:
        case NodeType::Crop:
        case NodeType::ColorConvert:
        case NodeType::FFT:
        case NodeType::Filter:
        case NodeType::Normalize:
        case NodeType::NeuralNet:
        case NodeType::Tokenize:
        case NodeType::Encode:
        case NodeType::Decode:
        case NodeType::Compress:
            return NodeCategory::Processor;

        case NodeType::FileOutput:
        case NodeType::Display:
        case NodeType::NetworkOutput:
        case NodeType::GpuOutput:
        case NodeType::AudioOutput:
            return NodeCategory::Sink;

        case NodeType::Split:
        case NodeType::Merge:
        case NodeType::Switch:
        case NodeType::Loop:
        case NodeType::Delay:
            return NodeCategory::Control;

        case NodeType::AliceAnalyze:
        case NodeType::VpuAlloc:
        case NodeType::BusTransfer:
        case NodeType::SwarmDistribute:
            return NodeCategory::StrayLight;
    }
    return NodeCategory::Processor;
}

const char* node_type_name(NodeType type) {
    switch (type) {
        case NodeType::FileInput:        return "File Input";
        case NodeType::CameraInput:      return "Camera Input";
        case NodeType::MicInput:         return "Mic Input";
        case NodeType::NetworkInput:     return "Network Input";
        case NodeType::GpuBuffer:        return "GPU Buffer";
        case NodeType::Resize:           return "Resize";
        case NodeType::Crop:             return "Crop";
        case NodeType::ColorConvert:     return "Color Convert";
        case NodeType::FFT:              return "FFT";
        case NodeType::Filter:           return "Filter";
        case NodeType::Normalize:        return "Normalize";
        case NodeType::NeuralNet:        return "Neural Net";
        case NodeType::Tokenize:         return "Tokenize";
        case NodeType::Encode:           return "Encode";
        case NodeType::Decode:           return "Decode";
        case NodeType::Compress:         return "Compress";
        case NodeType::FileOutput:       return "File Output";
        case NodeType::Display:          return "Display";
        case NodeType::NetworkOutput:    return "Network Output";
        case NodeType::GpuOutput:        return "GPU Output";
        case NodeType::AudioOutput:      return "Audio Output";
        case NodeType::Split:            return "Split";
        case NodeType::Merge:            return "Merge";
        case NodeType::Switch:           return "Switch";
        case NodeType::Loop:             return "Loop";
        case NodeType::Delay:            return "Delay";
        case NodeType::AliceAnalyze:     return "Alice Analyze";
        case NodeType::VpuAlloc:         return "VPU Alloc";
        case NodeType::BusTransfer:      return "Bus Transfer";
        case NodeType::SwarmDistribute:  return "Swarm Distribute";
    }
    return "Unknown";
}

ImVec4 category_color(NodeCategory cat) {
    switch (cat) {
        case NodeCategory::Source:     return ImVec4(0.2f, 0.7f, 0.3f, 1.0f);  // green
        case NodeCategory::Processor:  return ImVec4(0.3f, 0.5f, 0.9f, 1.0f);  // blue
        case NodeCategory::Sink:       return ImVec4(0.9f, 0.4f, 0.3f, 1.0f);  // red
        case NodeCategory::Control:    return ImVec4(0.9f, 0.7f, 0.2f, 1.0f);  // yellow
        case NodeCategory::StrayLight: return ImVec4(0.0f, 1.0f, 0.67f, 1.0f); // straylight cyan
    }
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
}

const char* port_data_type_name(PortDataType dt) {
    switch (dt) {
        case PortDataType::Any:     return "Any";
        case PortDataType::Bytes:   return "Bytes";
        case PortDataType::Image:   return "Image";
        case PortDataType::Audio:   return "Audio";
        case PortDataType::Text:    return "Text";
        case PortDataType::Tensor:  return "Tensor";
        case PortDataType::Command: return "Command";
    }
    return "Unknown";
}

ImVec4 port_data_type_color(PortDataType dt) {
    switch (dt) {
        case PortDataType::Any:     return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        case PortDataType::Bytes:   return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        case PortDataType::Image:   return ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
        case PortDataType::Audio:   return ImVec4(0.8f, 0.6f, 0.2f, 1.0f);
        case PortDataType::Text:    return ImVec4(0.8f, 0.8f, 0.3f, 1.0f);
        case PortDataType::Tensor:  return ImVec4(0.6f, 0.3f, 0.9f, 1.0f);
        case PortDataType::Command: return ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Port type compatibility
// ---------------------------------------------------------------------------

static bool ports_compatible(PortDataType src, PortDataType dst) {
    if (src == PortDataType::Any || dst == PortDataType::Any) return true;
    return src == dst;
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

void Pipeline::populate_ports(PipeNode& node) {
    auto make_in = [&](const char* lbl, PortDataType dt) {
        Port p;
        p.id       = next_port_id_++;
        p.label    = lbl;
        p.data_type = dt;
        p.is_input = true;
        node.inputs.push_back(std::move(p));
    };
    auto make_out = [&](const char* lbl, PortDataType dt) {
        Port p;
        p.id       = next_port_id_++;
        p.label    = lbl;
        p.data_type = dt;
        p.is_input = false;
        node.outputs.push_back(std::move(p));
    };

    switch (node.type) {
        // Sources
        case NodeType::FileInput:
            make_out("data", PortDataType::Bytes);
            node.config = {{"path", ""}, {"mode", "binary"}};
            break;
        case NodeType::CameraInput:
            make_out("frame", PortDataType::Image);
            node.config = {{"device", "/dev/video0"}, {"width", 640}, {"height", 480}};
            break;
        case NodeType::MicInput:
            make_out("audio", PortDataType::Audio);
            node.config = {{"device", "default"}, {"sample_rate", 44100}, {"channels", 1}};
            break;
        case NodeType::NetworkInput:
            make_out("data", PortDataType::Bytes);
            node.config = {{"host", "0.0.0.0"}, {"port", 9000}, {"protocol", "tcp"}};
            break;
        case NodeType::GpuBuffer:
            make_out("tensor", PortDataType::Tensor);
            node.config = {{"size_bytes", 0}, {"gpu_index", 0}};
            break;

        // Processors
        case NodeType::Resize:
            make_in("image", PortDataType::Image);
            make_out("image", PortDataType::Image);
            node.config = {{"width", 256}, {"height", 256}, {"interpolation", "bilinear"}};
            break;
        case NodeType::Crop:
            make_in("image", PortDataType::Image);
            make_out("image", PortDataType::Image);
            node.config = {{"x", 0}, {"y", 0}, {"w", 128}, {"h", 128}};
            break;
        case NodeType::ColorConvert:
            make_in("image", PortDataType::Image);
            make_out("image", PortDataType::Image);
            node.config = {{"from", "rgb"}, {"to", "grayscale"}};
            break;
        case NodeType::FFT:
            make_in("signal", PortDataType::Audio);
            make_out("spectrum", PortDataType::Tensor);
            node.config = {{"window_size", 1024}, {"hop", 512}};
            break;
        case NodeType::Filter:
            make_in("data", PortDataType::Any);
            make_out("data", PortDataType::Any);
            node.config = {{"expression", "pass"}, {"threshold", 0.5}};
            break;
        case NodeType::Normalize:
            make_in("tensor", PortDataType::Tensor);
            make_out("tensor", PortDataType::Tensor);
            node.config = {{"mean", 0.0}, {"std", 1.0}};
            break;
        case NodeType::NeuralNet:
            make_in("input", PortDataType::Tensor);
            make_out("output", PortDataType::Tensor);
            node.config = {{"model_path", ""}, {"backend", "vpu"}, {"batch_size", 1}};
            break;
        case NodeType::Tokenize:
            make_in("text", PortDataType::Text);
            make_out("tokens", PortDataType::Tensor);
            node.config = {{"vocab_path", ""}, {"max_length", 512}};
            break;
        case NodeType::Encode:
            make_in("data", PortDataType::Bytes);
            make_out("encoded", PortDataType::Bytes);
            node.config = {{"codec", "h264"}, {"bitrate", 4000000}};
            break;
        case NodeType::Decode:
            make_in("encoded", PortDataType::Bytes);
            make_out("data", PortDataType::Bytes);
            node.config = {{"codec", "h264"}};
            break;
        case NodeType::Compress:
            make_in("data", PortDataType::Bytes);
            make_out("compressed", PortDataType::Bytes);
            node.config = {{"algorithm", "zstd"}, {"level", 3}};
            break;

        // Sinks
        case NodeType::FileOutput:
            make_in("data", PortDataType::Bytes);
            node.config = {{"path", "/tmp/output.bin"}, {"mode", "binary"}};
            break;
        case NodeType::Display:
            make_in("image", PortDataType::Image);
            node.config = {{"window_title", "Preview"}, {"scale", 1.0}};
            break;
        case NodeType::NetworkOutput:
            make_in("data", PortDataType::Bytes);
            node.config = {{"host", "127.0.0.1"}, {"port", 9001}, {"protocol", "tcp"}};
            break;
        case NodeType::GpuOutput:
            make_in("tensor", PortDataType::Tensor);
            node.config = {{"gpu_index", 0}};
            break;
        case NodeType::AudioOutput:
            make_in("audio", PortDataType::Audio);
            node.config = {{"device", "default"}, {"sample_rate", 44100}};
            break;

        // Control
        case NodeType::Split:
            make_in("data", PortDataType::Any);
            make_out("out_a", PortDataType::Any);
            make_out("out_b", PortDataType::Any);
            node.config = {{"mode", "duplicate"}};
            break;
        case NodeType::Merge:
            make_in("in_a", PortDataType::Any);
            make_in("in_b", PortDataType::Any);
            make_out("merged", PortDataType::Any);
            node.config = {{"mode", "concat"}};
            break;
        case NodeType::Switch:
            make_in("data", PortDataType::Any);
            make_in("control", PortDataType::Command);
            make_out("out", PortDataType::Any);
            node.config = {{"default_route", 0}};
            break;
        case NodeType::Loop:
            make_in("data", PortDataType::Any);
            make_out("data", PortDataType::Any);
            node.config = {{"iterations", 1}, {"condition", "count"}};
            break;
        case NodeType::Delay:
            make_in("data", PortDataType::Any);
            make_out("data", PortDataType::Any);
            node.config = {{"delay_ms", 100}};
            break;

        // StrayLight-specific
        case NodeType::AliceAnalyze:
            make_in("data", PortDataType::Any);
            make_out("analysis", PortDataType::Text);
            make_out("passthrough", PortDataType::Any);
            node.config = {{"model", "alice-7b"}, {"mode", "classify"}};
            break;
        case NodeType::VpuAlloc:
            make_in("request", PortDataType::Command);
            make_out("handle", PortDataType::Tensor);
            node.config = {{"size_bytes", 0}, {"gpu_index", -1}};
            break;
        case NodeType::BusTransfer:
            make_in("data", PortDataType::Any);
            make_out("data", PortDataType::Any);
            node.config = {{"bus_channel", "default"}, {"priority", "normal"}};
            break;
        case NodeType::SwarmDistribute:
            make_in("data", PortDataType::Any);
            make_out("results", PortDataType::Any);
            node.config = {{"strategy", "gpu_affinity"}, {"min_vram_gb", 0}};
            break;
    }
}

uint32_t Pipeline::add_node(NodeType type, ImVec2 pos) {
    PipeNode node;
    node.id       = next_node_id_++;
    node.type     = type;
    node.label    = node_type_name(type);
    node.position = pos;
    populate_ports(node);
    uint32_t id = node.id;
    nodes_.push_back(std::move(node));
    return id;
}

Result<void, std::string> Pipeline::connect(uint32_t from_node, uint32_t from_port,
                                             uint32_t to_node, uint32_t to_port) {
    // Find source and destination nodes
    const PipeNode* src = find_node(from_node);
    const PipeNode* dst = find_node(to_node);
    if (!src) return Result<void, std::string>::error("Source node not found");
    if (!dst) return Result<void, std::string>::error("Destination node not found");

    // Find the ports
    const Port* src_port = nullptr;
    for (const auto& p : src->outputs) {
        if (p.id == from_port) { src_port = &p; break; }
    }
    if (!src_port) return Result<void, std::string>::error("Source port not found");

    const Port* dst_port = nullptr;
    for (const auto& p : dst->inputs) {
        if (p.id == to_port) { dst_port = &p; break; }
    }
    if (!dst_port) return Result<void, std::string>::error("Destination port not found");

    // Type compatibility
    if (!ports_compatible(src_port->data_type, dst_port->data_type)) {
        std::string msg = "Type mismatch: ";
        msg += port_data_type_name(src_port->data_type);
        msg += " -> ";
        msg += port_data_type_name(dst_port->data_type);
        return Result<void, std::string>::error(msg);
    }

    // Check for duplicate connection
    for (const auto& c : connections_) {
        if (c.from_node == from_node && c.from_port == from_port &&
            c.to_node == to_node && c.to_port == to_port) {
            return Result<void, std::string>::error("Connection already exists");
        }
    }

    // Check that the destination port is not already connected
    for (const auto& c : connections_) {
        if (c.to_node == to_node && c.to_port == to_port) {
            return Result<void, std::string>::error("Destination port already connected");
        }
    }

    PipeConnection conn;
    conn.id        = next_conn_id_++;
    conn.from_node = from_node;
    conn.from_port = from_port;
    conn.to_node   = to_node;
    conn.to_port   = to_port;
    connections_.push_back(conn);

    // Verify no cycle was introduced
    auto topo = topological_sort();
    if (!topo.has_value()) {
        connections_.pop_back();
        return Result<void, std::string>::error("Connection would create a cycle");
    }

    return Result<void, std::string>::ok();
}

void Pipeline::disconnect(uint32_t from_node, uint32_t from_port) {
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [=](const PipeConnection& c) {
                return c.from_node == from_node && c.from_port == from_port;
            }),
        connections_.end());
}

void Pipeline::remove_node(uint32_t id) {
    // Remove all connections involving this node
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
            [=](const PipeConnection& c) {
                return c.from_node == id || c.to_node == id;
            }),
        connections_.end());

    // Remove the node
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(),
            [=](const PipeNode& n) { return n.id == id; }),
        nodes_.end());
}

PipeNode* Pipeline::find_node(uint32_t id) {
    for (auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const PipeNode* Pipeline::find_node(uint32_t id) const {
    for (const auto& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Topological sort (Kahn's algorithm)
// ---------------------------------------------------------------------------

Result<std::vector<uint32_t>, std::string> Pipeline::topological_sort() const {
    // Build adjacency and in-degree maps
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    std::unordered_map<uint32_t, int> in_degree;

    for (const auto& n : nodes_) {
        adj[n.id];           // ensure entry exists
        in_degree[n.id] = 0; // start at zero
    }

    for (const auto& c : connections_) {
        adj[c.from_node].push_back(c.to_node);
        in_degree[c.to_node]++;
    }

    std::queue<uint32_t> ready;
    for (const auto& [nid, deg] : in_degree) {
        if (deg == 0) ready.push(nid);
    }

    std::vector<uint32_t> order;
    order.reserve(nodes_.size());

    while (!ready.empty()) {
        uint32_t cur = ready.front();
        ready.pop();
        order.push_back(cur);

        for (uint32_t next : adj[cur]) {
            if (--in_degree[next] == 0) {
                ready.push(next);
            }
        }
    }

    if (order.size() != nodes_.size()) {
        return Result<std::vector<uint32_t>, std::string>::error(
            "Cycle detected in pipeline graph");
    }

    return Result<std::vector<uint32_t>, std::string>::ok(std::move(order));
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Result<void, std::string> Pipeline::validate() const {
    if (nodes_.empty()) {
        return Result<void, std::string>::error("Pipeline is empty");
    }

    // Check for cycles
    auto topo = topological_sort();
    if (!topo.has_value()) {
        return Result<void, std::string>::error(topo.error());
    }

    // Check all connections for type compatibility
    for (const auto& c : connections_) {
        const PipeNode* src = find_node(c.from_node);
        const PipeNode* dst = find_node(c.to_node);
        if (!src || !dst) {
            return Result<void, std::string>::error("Dangling connection reference");
        }

        const Port* sp = nullptr;
        for (const auto& p : src->outputs) {
            if (p.id == c.from_port) { sp = &p; break; }
        }
        const Port* dp = nullptr;
        for (const auto& p : dst->inputs) {
            if (p.id == c.to_port) { dp = &p; break; }
        }

        if (!sp || !dp) {
            return Result<void, std::string>::error("Connection references nonexistent port");
        }

        if (!ports_compatible(sp->data_type, dp->data_type)) {
            std::string msg = "Type mismatch on connection from ";
            msg += src->label + "." + sp->label;
            msg += " to " + dst->label + "." + dp->label;
            return Result<void, std::string>::error(msg);
        }
    }

    // Warn about unconnected input ports on non-source nodes
    for (const auto& n : nodes_) {
        if (category_of(n.type) == NodeCategory::Source) continue;
        for (const auto& inp : n.inputs) {
            bool connected = false;
            for (const auto& c : connections_) {
                if (c.to_node == n.id && c.to_port == inp.id) {
                    connected = true;
                    break;
                }
            }
            if (!connected) {
                std::string msg = "Unconnected input port: " + n.label + "." + inp.label;
                return Result<void, std::string>::error(msg);
            }
        }
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

Result<std::vector<DataBlob>, std::string> Pipeline::execute_node(
    const PipeNode& node,
    const std::vector<DataBlob>& inputs) const
{
    std::vector<DataBlob> outputs;

    switch (node.type) {
        case NodeType::FileInput: {
            std::string path = node.config.value("path", std::string(""));
            if (path.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "FileInput: no path configured");
            }
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "FileInput: cannot open " + path);
            }
            std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)),
                                      std::istreambuf_iterator<char>());
            DataBlob blob;
            blob.type = PortDataType::Bytes;
            blob.data = std::move(buf);
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::CameraInput: {
            // Produce a placeholder frame: solid gray RGBA
            int w = node.config.value("width", 640);
            int h = node.config.value("height", 480);
            DataBlob blob;
            blob.type = PortDataType::Image;
            blob.data.resize(static_cast<size_t>(w) * h * 4, 128);
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::MicInput: {
            int sr = node.config.value("sample_rate", 44100);
            int ch = node.config.value("channels", 1);
            // Produce 100ms of silence
            size_t samples = static_cast<size_t>(sr) / 10 * ch;
            DataBlob blob;
            blob.type = PortDataType::Audio;
            blob.data.resize(samples * sizeof(float), 0);
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::NetworkInput: {
            DataBlob blob;
            blob.type = PortDataType::Bytes;
            // Network input would block for real data; emit empty placeholder
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::GpuBuffer: {
            size_t sz = node.config.value("size_bytes", 0);
            DataBlob blob;
            blob.type = PortDataType::Tensor;
            blob.data.resize(sz, 0);
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Resize: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Resize: no input");
            }
            int w = node.config.value("width", 256);
            int h = node.config.value("height", 256);
            DataBlob blob;
            blob.type = PortDataType::Image;
            // Nearest-neighbor downsample: just allocate target size
            blob.data.resize(static_cast<size_t>(w) * h * 4, 0);
            // Copy available source bytes, clamped
            size_t copy_len = std::min(blob.data.size(), inputs[0].data.size());
            if (copy_len > 0) {
                std::memcpy(blob.data.data(), inputs[0].data.data(), copy_len);
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Crop: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Crop: no input");
            }
            int cw = node.config.value("w", 128);
            int ch = node.config.value("h", 128);
            DataBlob blob;
            blob.type = PortDataType::Image;
            blob.data.resize(static_cast<size_t>(cw) * ch * 4, 0);
            size_t copy_len = std::min(blob.data.size(), inputs[0].data.size());
            if (copy_len > 0) {
                std::memcpy(blob.data.data(), inputs[0].data.data(), copy_len);
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::ColorConvert: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "ColorConvert: no input");
            }
            std::string to = node.config.value("to", std::string("grayscale"));
            DataBlob blob;
            blob.type = PortDataType::Image;
            if (to == "grayscale") {
                // Convert RGBA to single-channel grayscale
                size_t pixels = inputs[0].data.size() / 4;
                blob.data.resize(pixels);
                for (size_t i = 0; i < pixels; i++) {
                    size_t off = i * 4;
                    if (off + 2 < inputs[0].data.size()) {
                        uint8_t r = inputs[0].data[off];
                        uint8_t g = inputs[0].data[off + 1];
                        uint8_t b = inputs[0].data[off + 2];
                        blob.data[i] = static_cast<uint8_t>(
                            0.299f * r + 0.587f * g + 0.114f * b);
                    }
                }
            } else {
                blob.data = inputs[0].data; // passthrough
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::FFT: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "FFT: no input");
            }
            int window = node.config.value("window_size", 1024);
            DataBlob blob;
            blob.type = PortDataType::Tensor;
            // Produce magnitude spectrum: half-window complex magnitudes as floats
            size_t n_bins = static_cast<size_t>(window) / 2 + 1;
            blob.data.resize(n_bins * sizeof(float), 0);
            // Simple DFT magnitude for first window of input samples
            const float* samples_in = reinterpret_cast<const float*>(inputs[0].data.data());
            size_t n_samples = inputs[0].data.size() / sizeof(float);
            float* mag_out = reinterpret_cast<float*>(blob.data.data());
            for (size_t k = 0; k < n_bins && k < n_bins; k++) {
                float re = 0.0f, im = 0.0f;
                size_t N = std::min(static_cast<size_t>(window), n_samples);
                for (size_t n = 0; n < N; n++) {
                    float angle = 2.0f * 3.14159265f * k * n / static_cast<float>(N);
                    re += samples_in[n] * std::cos(angle);
                    im -= samples_in[n] * std::sin(angle);
                }
                mag_out[k] = std::sqrt(re * re + im * im);
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Filter: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Filter: no input");
            }
            std::string expr = node.config.value("expression", std::string("pass"));
            float threshold = node.config.value("threshold", 0.5f);
            DataBlob blob;
            blob.type = inputs[0].type;
            if (expr == "threshold") {
                // Zero out bytes below threshold * 255
                blob.data = inputs[0].data;
                uint8_t t = static_cast<uint8_t>(threshold * 255.0f);
                for (auto& b : blob.data) {
                    if (b < t) b = 0;
                }
            } else {
                // pass-through
                blob.data = inputs[0].data;
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Normalize: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Normalize: no input");
            }
            float target_mean = node.config.value("mean", 0.0f);
            float target_std  = node.config.value("std", 1.0f);
            DataBlob blob;
            blob.type = PortDataType::Tensor;
            blob.data = inputs[0].data;
            // Treat as float array, compute mean/std, normalize
            float* vals = reinterpret_cast<float*>(blob.data.data());
            size_t count = blob.data.size() / sizeof(float);
            if (count > 0) {
                float sum = 0.0f;
                for (size_t i = 0; i < count; i++) sum += vals[i];
                float mean = sum / static_cast<float>(count);
                float var_sum = 0.0f;
                for (size_t i = 0; i < count; i++) {
                    float d = vals[i] - mean;
                    var_sum += d * d;
                }
                float stddev = std::sqrt(var_sum / static_cast<float>(count));
                if (stddev < 1e-8f) stddev = 1.0f;
                for (size_t i = 0; i < count; i++) {
                    vals[i] = (vals[i] - mean) / stddev * target_std + target_mean;
                }
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::NeuralNet: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "NeuralNet: no input");
            }
            // In production this would invoke the VPU; here pass-through with size info
            DataBlob blob;
            blob.type = PortDataType::Tensor;
            blob.data = inputs[0].data;
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Tokenize: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Tokenize: no input");
            }
            int max_len = node.config.value("max_length", 512);
            // Simple whitespace tokenizer producing uint32 token IDs
            std::string text(inputs[0].data.begin(), inputs[0].data.end());
            std::vector<uint32_t> token_ids;
            std::istringstream iss(text);
            std::string word;
            while (iss >> word && static_cast<int>(token_ids.size()) < max_len) {
                // Hash the word into a token ID
                uint32_t hash = 0;
                for (char ch : word) {
                    hash = hash * 31 + static_cast<uint32_t>(ch);
                }
                token_ids.push_back(hash);
            }
            DataBlob blob;
            blob.type = PortDataType::Tensor;
            blob.data.resize(token_ids.size() * sizeof(uint32_t));
            std::memcpy(blob.data.data(), token_ids.data(),
                        token_ids.size() * sizeof(uint32_t));
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Encode: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Encode: no input");
            }
            // Placeholder: prepend a 4-byte header with original size
            DataBlob blob;
            blob.type = PortDataType::Bytes;
            uint32_t orig_size = static_cast<uint32_t>(inputs[0].data.size());
            blob.data.resize(sizeof(uint32_t) + inputs[0].data.size());
            std::memcpy(blob.data.data(), &orig_size, sizeof(uint32_t));
            std::memcpy(blob.data.data() + sizeof(uint32_t),
                        inputs[0].data.data(), inputs[0].data.size());
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Decode: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Decode: no input");
            }
            DataBlob blob;
            blob.type = PortDataType::Bytes;
            if (inputs[0].data.size() > sizeof(uint32_t)) {
                blob.data.assign(inputs[0].data.begin() + sizeof(uint32_t),
                                 inputs[0].data.end());
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Compress: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Compress: no input");
            }
            // Simple RLE compression
            DataBlob blob;
            blob.type = PortDataType::Bytes;
            const auto& src = inputs[0].data;
            std::vector<uint8_t> compressed;
            compressed.reserve(src.size());
            size_t i = 0;
            while (i < src.size()) {
                uint8_t val = src[i];
                uint8_t run = 1;
                while (i + run < src.size() && src[i + run] == val && run < 255) {
                    run++;
                }
                compressed.push_back(run);
                compressed.push_back(val);
                i += run;
            }
            blob.data = std::move(compressed);
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::FileOutput: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "FileOutput: no input");
            }
            std::string path = node.config.value("path", std::string("/tmp/output.bin"));
            std::ofstream ofs(path, std::ios::binary);
            if (!ofs.is_open()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "FileOutput: cannot open " + path);
            }
            ofs.write(reinterpret_cast<const char*>(inputs[0].data.data()),
                      static_cast<std::streamsize>(inputs[0].data.size()));
            // Sinks produce no outputs
            break;
        }

        case NodeType::Display: {
            // In production, would blit to a Wayland surface; here no-op
            break;
        }

        case NodeType::NetworkOutput: {
            // Would send data over TCP; no-op in offline execution
            break;
        }

        case NodeType::GpuOutput: {
            // Would upload to GPU via VPU ioctl; no-op
            break;
        }

        case NodeType::AudioOutput: {
            // Would play via ALSA/PipeWire; no-op
            break;
        }

        case NodeType::Split: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Split: no input");
            }
            // Duplicate input to both outputs
            DataBlob a = inputs[0];
            DataBlob b = inputs[0];
            outputs.push_back(std::move(a));
            outputs.push_back(std::move(b));
            break;
        }

        case NodeType::Merge: {
            DataBlob blob;
            blob.type = PortDataType::Any;
            // Concatenate all inputs
            for (const auto& inp : inputs) {
                blob.data.insert(blob.data.end(), inp.data.begin(), inp.data.end());
            }
            if (!inputs.empty()) blob.type = inputs[0].type;
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Switch: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Switch: no data input");
            }
            // Pass through the data input
            outputs.push_back(inputs[0]);
            break;
        }

        case NodeType::Loop: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Loop: no input");
            }
            int iters = node.config.value("iterations", 1);
            DataBlob blob = inputs[0];
            // Repeat data iters times
            if (iters > 1) {
                std::vector<uint8_t> repeated;
                repeated.reserve(blob.data.size() * iters);
                for (int i = 0; i < iters; i++) {
                    repeated.insert(repeated.end(), blob.data.begin(), blob.data.end());
                }
                blob.data = std::move(repeated);
            }
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::Delay: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "Delay: no input");
            }
            // In execution context, just pass through (delay is for real-time only)
            outputs.push_back(inputs[0]);
            break;
        }

        case NodeType::AliceAnalyze: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "AliceAnalyze: no input");
            }
            std::string mode = node.config.value("mode", std::string("classify"));
            // Emit analysis text and passthrough
            std::string analysis = "{\"type\":\"" + mode +
                                   "\",\"input_bytes\":" +
                                   std::to_string(inputs[0].data.size()) +
                                   ",\"result\":\"analyzed\"}";
            DataBlob analysis_blob;
            analysis_blob.type = PortDataType::Text;
            analysis_blob.data.assign(analysis.begin(), analysis.end());
            outputs.push_back(std::move(analysis_blob));
            // passthrough
            outputs.push_back(inputs[0]);
            break;
        }

        case NodeType::VpuAlloc: {
            size_t sz = node.config.value("size_bytes", 0);
            if (sz == 0 && !inputs.empty()) {
                sz = inputs[0].data.size();
            }
            DataBlob blob;
            blob.type = PortDataType::Tensor;
            blob.data.resize(sz, 0);
            outputs.push_back(std::move(blob));
            break;
        }

        case NodeType::BusTransfer: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "BusTransfer: no input");
            }
            // Passthrough (in production, routes through straylight-bus IPC)
            outputs.push_back(inputs[0]);
            break;
        }

        case NodeType::SwarmDistribute: {
            if (inputs.empty()) {
                return Result<std::vector<DataBlob>, std::string>::error(
                    "SwarmDistribute: no input");
            }
            // Passthrough (in production, fans out to swarm nodes)
            outputs.push_back(inputs[0]);
            break;
        }
    }

    return Result<std::vector<DataBlob>, std::string>::ok(std::move(outputs));
}

Result<void, std::string> Pipeline::execute() {
    auto valid = validate();
    if (!valid.has_value()) {
        return valid;
    }

    auto topo = topological_sort();
    if (!topo.has_value()) {
        return Result<void, std::string>::error(topo.error());
    }

    // Port ID -> DataBlob mapping for intermediate results
    std::unordered_map<uint32_t, DataBlob> port_data;

    // Reset executed flags
    for (auto& n : nodes_) n.executed = false;

    for (uint32_t nid : topo.value()) {
        PipeNode* node = find_node(nid);
        if (!node) continue;

        // Gather inputs from connected output ports
        std::vector<DataBlob> inputs;
        for (const auto& inp : node->inputs) {
            for (const auto& c : connections_) {
                if (c.to_node == nid && c.to_port == inp.id) {
                    auto it = port_data.find(c.from_port);
                    if (it != port_data.end()) {
                        inputs.push_back(it->second);
                    }
                    break;
                }
            }
        }

        auto result = execute_node(*node, inputs);
        if (!result.has_value()) {
            return Result<void, std::string>::error(
                node->label + ": " + result.error());
        }

        // Store outputs by port ID
        const auto& out_blobs = result.value();
        for (size_t i = 0; i < out_blobs.size() && i < node->outputs.size(); i++) {
            port_data[node->outputs[i].id] = out_blobs[i];
        }

        node->executed = true;
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Code generation (delegates to CodeGenerator in code_generator.cpp)
// ---------------------------------------------------------------------------

Result<std::string, std::string> Pipeline::generate_code() const {
    auto valid = validate();
    if (!valid.has_value()) {
        return Result<std::string, std::string>::error(valid.error());
    }

    auto topo = topological_sort();
    if (!topo.has_value()) {
        return Result<std::string, std::string>::error(topo.error());
    }

    // Forward to CodeGenerator (defined in code_generator.cpp)
    // Import is done via the header; this method is a convenience wrapper.
    // We inline the generation here to avoid circular dependency issues.

    std::ostringstream out;
    out << "// Generated by StrayLight Pipe\n";
    out << "// Pipeline code — do not edit manually\n\n";
    out << "#include <cstdint>\n";
    out << "#include <cstring>\n";
    out << "#include <fstream>\n";
    out << "#include <iostream>\n";
    out << "#include <string>\n";
    out << "#include <vector>\n";
    out << "#include <cmath>\n";
    out << "#include <thread>\n";
    out << "#include <future>\n\n";

    // Check which node types are used and add relevant includes
    std::unordered_set<NodeType> used_types;
    for (const auto& n : nodes_) {
        used_types.insert(n.type);
    }

    if (used_types.count(NodeType::NeuralNet) ||
        used_types.count(NodeType::VpuAlloc)) {
        out << "// VPU includes (StrayLight hardware)\n";
        out << "// #include <straylight/vpu.h>\n\n";
    }
    if (used_types.count(NodeType::AliceAnalyze)) {
        out << "// Alice AI includes\n";
        out << "// #include <straylight/alice.h>\n\n";
    }
    if (used_types.count(NodeType::SwarmDistribute)) {
        out << "// Swarm includes\n";
        out << "// #include <straylight/swarm_client.h>\n\n";
    }
    if (used_types.count(NodeType::BusTransfer)) {
        out << "// Bus IPC includes\n";
        out << "// #include <straylight/ipc.h>\n\n";
    }

    out << "// Result type for error handling\n";
    out << "template <typename T, typename E>\n";
    out << "struct Result {\n";
    out << "    bool ok;\n";
    out << "    T value;\n";
    out << "    E error_msg;\n";
    out << "    static Result success(T v) { return {true, std::move(v), {}}; }\n";
    out << "    static Result fail(E e) { return {false, {}, std::move(e)}; }\n";
    out << "};\n\n";

    out << "struct DataBlob {\n";
    out << "    std::vector<uint8_t> data;\n";
    out << "};\n\n";

    // Emit buffer declarations
    const auto& order = topo.value();
    for (size_t idx = 0; idx < order.size(); idx++) {
        const PipeNode* n = find_node(order[idx]);
        if (!n) continue;
        for (size_t p = 0; p < n->outputs.size(); p++) {
            out << "DataBlob buf_" << n->id << "_out" << p << ";\n";
        }
    }
    out << "\n";

    // Detect parallel groups: nodes with no dependency between them
    // at the same topological "level"
    std::unordered_map<uint32_t, int> node_level;
    for (uint32_t nid : order) {
        int level = 0;
        for (const auto& c : connections_) {
            if (c.to_node == nid) {
                auto it = node_level.find(c.from_node);
                if (it != node_level.end()) {
                    level = std::max(level, it->second + 1);
                }
            }
        }
        node_level[nid] = level;
    }

    int max_level = 0;
    for (const auto& [nid, lvl] : node_level) {
        max_level = std::max(max_level, lvl);
    }

    out << "int main() {\n";

    for (int lvl = 0; lvl <= max_level; lvl++) {
        std::vector<uint32_t> level_nodes;
        for (uint32_t nid : order) {
            if (node_level[nid] == lvl) level_nodes.push_back(nid);
        }

        bool use_parallel = level_nodes.size() > 1;

        if (use_parallel) {
            out << "    // --- Level " << lvl << " (parallel) ---\n";
            out << "    {\n";
            out << "        std::vector<std::future<void>> futures;\n";
            for (uint32_t nid : level_nodes) {
                const PipeNode* n = find_node(nid);
                if (!n) continue;
                out << "        futures.push_back(std::async(std::launch::async, [&]() {\n";
                out << "            // Node: " << n->label << " (id=" << n->id << ")\n";

                // Generate node body
                switch (n->type) {
                    case NodeType::FileInput:
                        out << "            {\n";
                        out << "                std::ifstream ifs(\""
                            << n->config.value("path", std::string(""))
                            << "\", std::ios::binary);\n";
                        out << "                buf_" << n->id << "_out0.data.assign("
                            << "std::istreambuf_iterator<char>(ifs), "
                            << "std::istreambuf_iterator<char>());\n";
                        out << "            }\n";
                        break;
                    case NodeType::FileOutput: {
                        // Find input connection
                        std::string src_buf = "/* no input */";
                        for (const auto& c : connections_) {
                            if (c.to_node == n->id) {
                                const PipeNode* sn = find_node(c.from_node);
                                if (sn) {
                                    for (size_t pi = 0; pi < sn->outputs.size(); pi++) {
                                        if (sn->outputs[pi].id == c.from_port) {
                                            src_buf = "buf_" + std::to_string(sn->id) + "_out" + std::to_string(pi);
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        out << "            {\n";
                        out << "                std::ofstream ofs(\""
                            << n->config.value("path", std::string("/tmp/output.bin"))
                            << "\", std::ios::binary);\n";
                        out << "                ofs.write(reinterpret_cast<const char*>("
                            << src_buf << ".data.data()), " << src_buf << ".data.size());\n";
                        out << "            }\n";
                        break;
                    }
                    default: {
                        // Generic: copy first input to output
                        std::string src_buf;
                        for (const auto& c : connections_) {
                            if (c.to_node == n->id) {
                                const PipeNode* sn = find_node(c.from_node);
                                if (sn) {
                                    for (size_t pi = 0; pi < sn->outputs.size(); pi++) {
                                        if (sn->outputs[pi].id == c.from_port) {
                                            src_buf = "buf_" + std::to_string(sn->id) + "_out" + std::to_string(pi);
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        if (!src_buf.empty() && !n->outputs.empty()) {
                            out << "            buf_" << n->id << "_out0.data = "
                                << src_buf << ".data; // " << n->label << "\n";
                        } else if (!src_buf.empty()) {
                            out << "            // Sink: " << n->label << " consumes " << src_buf << "\n";
                        } else if (!n->outputs.empty()) {
                            out << "            // Source: " << n->label << "\n";
                            out << "            buf_" << n->id << "_out0.data = {};\n";
                        }
                        break;
                    }
                }

                out << "        }));\n";
            }
            out << "        for (auto& f : futures) f.get();\n";
            out << "    }\n\n";
        } else {
            out << "    // --- Level " << lvl << " ---\n";
            for (uint32_t nid : level_nodes) {
                const PipeNode* n = find_node(nid);
                if (!n) continue;
                out << "    // Node: " << n->label << " (id=" << n->id << ")\n";

                switch (n->type) {
                    case NodeType::FileInput:
                        out << "    {\n";
                        out << "        std::ifstream ifs(\""
                            << n->config.value("path", std::string(""))
                            << "\", std::ios::binary);\n";
                        out << "        buf_" << n->id << "_out0.data.assign("
                            << "std::istreambuf_iterator<char>(ifs), "
                            << "std::istreambuf_iterator<char>());\n";
                        out << "    }\n";
                        break;
                    case NodeType::FileOutput: {
                        std::string src_buf;
                        for (const auto& c : connections_) {
                            if (c.to_node == n->id) {
                                const PipeNode* sn = find_node(c.from_node);
                                if (sn) {
                                    for (size_t pi = 0; pi < sn->outputs.size(); pi++) {
                                        if (sn->outputs[pi].id == c.from_port) {
                                            src_buf = "buf_" + std::to_string(sn->id) + "_out" + std::to_string(pi);
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        out << "    {\n";
                        out << "        std::ofstream ofs(\""
                            << n->config.value("path", std::string("/tmp/output.bin"))
                            << "\", std::ios::binary);\n";
                        if (!src_buf.empty()) {
                            out << "        ofs.write(reinterpret_cast<const char*>("
                                << src_buf << ".data.data()), " << src_buf << ".data.size());\n";
                        }
                        out << "    }\n";
                        break;
                    }
                    default: {
                        std::string src_buf;
                        for (const auto& c : connections_) {
                            if (c.to_node == n->id) {
                                const PipeNode* sn = find_node(c.from_node);
                                if (sn) {
                                    for (size_t pi = 0; pi < sn->outputs.size(); pi++) {
                                        if (sn->outputs[pi].id == c.from_port) {
                                            src_buf = "buf_" + std::to_string(sn->id) + "_out" + std::to_string(pi);
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                        if (!src_buf.empty() && !n->outputs.empty()) {
                            out << "    buf_" << n->id << "_out0.data = "
                                << src_buf << ".data; // " << n->label << "\n";
                        } else if (!src_buf.empty()) {
                            out << "    // Sink: " << n->label << " consumes " << src_buf << "\n";
                        } else if (!n->outputs.empty()) {
                            out << "    // Source: " << n->label << "\n";
                            out << "    buf_" << n->id << "_out0.data = {};\n";
                        }
                        break;
                    }
                }
            }
            out << "\n";
        }
    }

    out << "    std::cout << \"Pipeline execution complete.\" << std::endl;\n";
    out << "    return 0;\n";
    out << "}\n";

    return Result<std::string, std::string>::ok(out.str());
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

Result<void, std::string> Pipeline::save(const std::string& path) const {
    nlohmann::json j;
    j["version"] = 1;

    nlohmann::json j_nodes = nlohmann::json::array();
    for (const auto& n : nodes_) {
        nlohmann::json jn;
        jn["id"]    = n.id;
        jn["type"]  = static_cast<int>(n.type);
        jn["label"] = n.label;
        jn["pos_x"] = n.position.x;
        jn["pos_y"] = n.position.y;
        jn["config"] = n.config;

        nlohmann::json j_inputs = nlohmann::json::array();
        for (const auto& p : n.inputs) {
            j_inputs.push_back({
                {"id", p.id}, {"label", p.label},
                {"data_type", static_cast<int>(p.data_type)}
            });
        }
        jn["inputs"] = j_inputs;

        nlohmann::json j_outputs = nlohmann::json::array();
        for (const auto& p : n.outputs) {
            j_outputs.push_back({
                {"id", p.id}, {"label", p.label},
                {"data_type", static_cast<int>(p.data_type)}
            });
        }
        jn["outputs"] = j_outputs;

        j_nodes.push_back(jn);
    }
    j["nodes"] = j_nodes;

    nlohmann::json j_conns = nlohmann::json::array();
    for (const auto& c : connections_) {
        j_conns.push_back({
            {"id", c.id},
            {"from_node", c.from_node}, {"from_port", c.from_port},
            {"to_node", c.to_node},     {"to_port", c.to_port}
        });
    }
    j["connections"] = j_conns;
    j["next_node_id"] = next_node_id_;
    j["next_conn_id"] = next_conn_id_;
    j["next_port_id"] = next_port_id_;

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        return Result<void, std::string>::error("Cannot open file for writing: " + path);
    }
    ofs << j.dump(2);
    if (!ofs.good()) {
        return Result<void, std::string>::error("Write failed: " + path);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> Pipeline::load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Result<void, std::string>::error("Cannot open file: " + path);
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const nlohmann::json::exception& e) {
        return Result<void, std::string>::error(std::string("JSON parse error: ") + e.what());
    }

    if (!j.contains("version") || j["version"].get<int>() != 1) {
        return Result<void, std::string>::error("Unsupported pipeline version");
    }

    clear();

    for (const auto& jn : j["nodes"]) {
        PipeNode node;
        node.id       = jn["id"].get<uint32_t>();
        node.type     = static_cast<NodeType>(jn["type"].get<int>());
        node.label    = jn["label"].get<std::string>();
        node.position = ImVec2(jn["pos_x"].get<float>(), jn["pos_y"].get<float>());
        node.config   = jn.value("config", nlohmann::json::object());

        for (const auto& jp : jn["inputs"]) {
            Port p;
            p.id        = jp["id"].get<uint32_t>();
            p.label     = jp["label"].get<std::string>();
            p.data_type = static_cast<PortDataType>(jp["data_type"].get<int>());
            p.is_input  = true;
            node.inputs.push_back(std::move(p));
        }
        for (const auto& jp : jn["outputs"]) {
            Port p;
            p.id        = jp["id"].get<uint32_t>();
            p.label     = jp["label"].get<std::string>();
            p.data_type = static_cast<PortDataType>(jp["data_type"].get<int>());
            p.is_input  = false;
            node.outputs.push_back(std::move(p));
        }

        nodes_.push_back(std::move(node));
    }

    for (const auto& jc : j["connections"]) {
        PipeConnection c;
        c.id        = jc["id"].get<uint32_t>();
        c.from_node = jc["from_node"].get<uint32_t>();
        c.from_port = jc["from_port"].get<uint32_t>();
        c.to_node   = jc["to_node"].get<uint32_t>();
        c.to_port   = jc["to_port"].get<uint32_t>();
        connections_.push_back(c);
    }

    next_node_id_ = j.value("next_node_id", static_cast<uint32_t>(nodes_.size() + 1));
    next_conn_id_ = j.value("next_conn_id", static_cast<uint32_t>(connections_.size() + 1));
    next_port_id_ = j.value("next_port_id", next_port_id_);

    return Result<void, std::string>::ok();
}

void Pipeline::clear() {
    nodes_.clear();
    connections_.clear();
    next_node_id_ = 1;
    next_conn_id_ = 1;
    next_port_id_ = 1;
}

} // namespace straylight::pipe
