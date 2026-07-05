// apps/pipe/pipeline.h
// StrayLight Pipe — Visual Dataflow Pipeline Engine
#pragma once

#include <straylight/result.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::pipe {

// ---------------------------------------------------------------------------
// Node taxonomy
// ---------------------------------------------------------------------------

enum class NodeType : uint8_t {
    // Sources
    FileInput, CameraInput, MicInput, NetworkInput, GpuBuffer,
    // Processors
    Resize, Crop, ColorConvert, FFT, Filter, Normalize,
    NeuralNet, Tokenize, Encode, Decode, Compress,
    // Sinks
    FileOutput, Display, NetworkOutput, GpuOutput, AudioOutput,
    // Control
    Split, Merge, Switch, Loop, Delay,
    // StrayLight-specific
    AliceAnalyze, VpuAlloc, BusTransfer, SwarmDistribute,
};

enum class NodeCategory : uint8_t {
    Source, Processor, Sink, Control, StrayLight,
};

NodeCategory category_of(NodeType type);
const char* node_type_name(NodeType type);
ImVec4 category_color(NodeCategory cat);

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

enum class PortDataType : uint8_t {
    Any, Bytes, Image, Audio, Text, Tensor, Command,
};

const char* port_data_type_name(PortDataType dt);
ImVec4 port_data_type_color(PortDataType dt);

struct Port {
    uint32_t    id;
    std::string label;
    PortDataType data_type = PortDataType::Any;
    bool        is_input   = true;
};

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

struct PipeNode {
    uint32_t          id       = 0;
    NodeType          type     = NodeType::FileInput;
    std::string       label;
    ImVec2            position = {0.0f, 0.0f};
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    nlohmann::json    config;

    /// True if the node ran successfully in the last execution.
    bool executed = false;
};

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

struct PipeConnection {
    uint32_t id        = 0;
    uint32_t from_node = 0;
    uint32_t from_port = 0;
    uint32_t to_node   = 0;
    uint32_t to_port   = 0;
};

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

/// Holds a typed blob that flows between nodes at execution time.
struct DataBlob {
    PortDataType type = PortDataType::Bytes;
    std::vector<uint8_t> data;
};

class Pipeline {
public:
    Pipeline();
    ~Pipeline();

    // ---- Mutation ----

    uint32_t add_node(NodeType type, ImVec2 pos);

    Result<void, std::string> connect(uint32_t from_node, uint32_t from_port,
                                       uint32_t to_node, uint32_t to_port);

    void disconnect(uint32_t from_node, uint32_t from_port);
    void remove_node(uint32_t id);

    // ---- Queries ----

    PipeNode*       find_node(uint32_t id);
    const PipeNode* find_node(uint32_t id) const;

    const std::vector<PipeNode>&       nodes()       const { return nodes_; }
    const std::vector<PipeConnection>& connections() const { return connections_; }

    // ---- Processing ----

    /// Validate the graph: detect cycles, check port type compatibility.
    Result<void, std::string> validate() const;

    /// Execute the pipeline in topological order.
    Result<void, std::string> execute();

    /// Generate compilable C++ source from the current graph.
    Result<std::string, std::string> generate_code() const;

    // ---- Serialization ----

    Result<void, std::string> save(const std::string& path) const;
    Result<void, std::string> load(const std::string& path);

    void clear();

private:
    /// Return nodes in topological order. Error if cycle detected.
    Result<std::vector<uint32_t>, std::string> topological_sort() const;

    /// Create default ports for a node type.
    void populate_ports(PipeNode& node);

    /// Execute a single node given its input blobs.
    Result<std::vector<DataBlob>, std::string> execute_node(
        const PipeNode& node,
        const std::vector<DataBlob>& inputs) const;

    std::vector<PipeNode>       nodes_;
    std::vector<PipeConnection> connections_;
    uint32_t next_node_id_ = 1;
    uint32_t next_conn_id_ = 1;
    uint32_t next_port_id_ = 1;
};

} // namespace straylight::pipe
