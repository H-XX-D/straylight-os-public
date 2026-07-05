// apps/pipe/code_generator.cpp
// StrayLight Pipe — C++ code generation from validated pipelines.
#include "code_generator.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace straylight::pipe {

CodeGenerator::CodeGenerator(const Pipeline& pipeline)
    : pipeline_(pipeline) {}

// ---------------------------------------------------------------------------
// Include emission
// ---------------------------------------------------------------------------

std::string CodeGenerator::emit_includes(const std::vector<uint32_t>& order) const {
    std::unordered_set<NodeType> used;
    for (uint32_t nid : order) {
        const PipeNode* n = pipeline_.find_node(nid);
        if (n) used.insert(n->type);
    }

    std::ostringstream out;
    out << "#include <cstdint>\n";
    out << "#include <cstring>\n";
    out << "#include <cmath>\n";
    out << "#include <fstream>\n";
    out << "#include <iostream>\n";
    out << "#include <string>\n";
    out << "#include <vector>\n";

    bool needs_threads = false;
    auto groups = find_parallel_groups(order);
    for (const auto& g : groups) {
        if (g.size() > 1) { needs_threads = true; break; }
    }
    if (needs_threads) {
        out << "#include <thread>\n";
        out << "#include <future>\n";
    }

    out << "\n";

    if (used.count(NodeType::NeuralNet) || used.count(NodeType::VpuAlloc)) {
        out << "// #include <straylight/vpu.h>  // VPU hardware access\n";
    }
    if (used.count(NodeType::AliceAnalyze)) {
        out << "// #include <straylight/alice.h>  // Alice AI subsystem\n";
    }
    if (used.count(NodeType::SwarmDistribute)) {
        out << "// #include <straylight/swarm_client.h>  // Swarm distribution\n";
    }
    if (used.count(NodeType::BusTransfer)) {
        out << "// #include <straylight/ipc.h>  // IPC bus transfer\n";
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// Buffer declarations
// ---------------------------------------------------------------------------

std::string CodeGenerator::emit_buffers(const std::vector<uint32_t>& order) const {
    std::ostringstream out;
    out << "// Intermediate buffers\n";
    out << "struct DataBlob { std::vector<uint8_t> data; };\n\n";

    for (uint32_t nid : order) {
        const PipeNode* n = pipeline_.find_node(nid);
        if (!n) continue;
        for (size_t i = 0; i < n->outputs.size(); i++) {
            out << "DataBlob buf_" << n->id << "_out" << i << ";\n";
        }
    }
    out << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// Single node call emission
// ---------------------------------------------------------------------------

std::string CodeGenerator::emit_node_call(const PipeNode& node, int /*buf_index*/) const {
    std::ostringstream out;

    // Find input buffer name
    auto find_input_buf = [&](size_t input_idx) -> std::string {
        if (input_idx >= node.inputs.size()) return "/* no input */";
        uint32_t port_id = node.inputs[input_idx].id;
        for (const auto& c : pipeline_.connections()) {
            if (c.to_node == node.id && c.to_port == port_id) {
                const PipeNode* src = pipeline_.find_node(c.from_node);
                if (src) {
                    for (size_t i = 0; i < src->outputs.size(); i++) {
                        if (src->outputs[i].id == c.from_port) {
                            return "buf_" + std::to_string(src->id) + "_out" + std::to_string(i);
                        }
                    }
                }
            }
        }
        return "/* unconnected */";
    };

    std::string input_buf = find_input_buf(0);

    switch (node.type) {
        case NodeType::FileInput:
            out << "    {\n";
            out << "        std::ifstream ifs(\"" << node.config.value("path", std::string("")) << "\", std::ios::binary);\n";
            out << "        if (ifs.is_open()) {\n";
            out << "            buf_" << node.id << "_out0.data.assign(\n";
            out << "                std::istreambuf_iterator<char>(ifs),\n";
            out << "                std::istreambuf_iterator<char>());\n";
            out << "        } else {\n";
            out << "            std::cerr << \"Error: cannot open input file\" << std::endl;\n";
            out << "            return 1;\n";
            out << "        }\n";
            out << "    }\n";
            break;

        case NodeType::CameraInput: {
            int w = node.config.value("width", 640);
            int h = node.config.value("height", 480);
            out << "    // Camera capture (placeholder: solid gray frame)\n";
            out << "    buf_" << node.id << "_out0.data.resize(" << (w * h * 4) << ", 128);\n";
            break;
        }

        case NodeType::MicInput: {
            int sr = node.config.value("sample_rate", 44100);
            int ch = node.config.value("channels", 1);
            out << "    // Mic capture (placeholder: 100ms silence)\n";
            out << "    buf_" << node.id << "_out0.data.resize(" << (sr / 10 * ch * 4) << ", 0);\n";
            break;
        }

        case NodeType::NetworkInput:
            out << "    // Network input (placeholder)\n";
            out << "    buf_" << node.id << "_out0.data = {};\n";
            break;

        case NodeType::GpuBuffer: {
            size_t sz = node.config.value("size_bytes", 0);
            out << "    buf_" << node.id << "_out0.data.resize(" << sz << ", 0);\n";
            break;
        }

        case NodeType::Resize: {
            int w = node.config.value("width", 256);
            int h = node.config.value("height", 256);
            out << "    // Resize to " << w << "x" << h << "\n";
            out << "    buf_" << node.id << "_out0.data.resize(" << (w * h * 4) << ", 0);\n";
            out << "    std::memcpy(buf_" << node.id << "_out0.data.data(), "
                << input_buf << ".data.data(),\n";
            out << "                std::min(buf_" << node.id << "_out0.data.size(), "
                << input_buf << ".data.size()));\n";
            break;
        }

        case NodeType::Crop: {
            int cw = node.config.value("w", 128);
            int ch = node.config.value("h", 128);
            out << "    // Crop to " << cw << "x" << ch << "\n";
            out << "    buf_" << node.id << "_out0.data.resize(" << (cw * ch * 4) << ", 0);\n";
            out << "    std::memcpy(buf_" << node.id << "_out0.data.data(), "
                << input_buf << ".data.data(),\n";
            out << "                std::min(buf_" << node.id << "_out0.data.size(), "
                << input_buf << ".data.size()));\n";
            break;
        }

        case NodeType::ColorConvert:
            out << "    // Color conversion\n";
            out << "    {\n";
            out << "        size_t pixels = " << input_buf << ".data.size() / 4;\n";
            out << "        buf_" << node.id << "_out0.data.resize(pixels);\n";
            out << "        for (size_t i = 0; i < pixels; i++) {\n";
            out << "            size_t off = i * 4;\n";
            out << "            uint8_t r = " << input_buf << ".data[off];\n";
            out << "            uint8_t g = " << input_buf << ".data[off + 1];\n";
            out << "            uint8_t b = " << input_buf << ".data[off + 2];\n";
            out << "            buf_" << node.id << "_out0.data[i] = static_cast<uint8_t>(0.299f*r + 0.587f*g + 0.114f*b);\n";
            out << "        }\n";
            out << "    }\n";
            break;

        case NodeType::FFT: {
            int win = node.config.value("window_size", 1024);
            out << "    // FFT (window=" << win << ")\n";
            out << "    {\n";
            out << "        size_t n_bins = " << (win / 2 + 1) << ";\n";
            out << "        buf_" << node.id << "_out0.data.resize(n_bins * sizeof(float), 0);\n";
            out << "        const float* in = reinterpret_cast<const float*>(" << input_buf << ".data.data());\n";
            out << "        float* mag = reinterpret_cast<float*>(buf_" << node.id << "_out0.data.data());\n";
            out << "        size_t N = std::min(static_cast<size_t>(" << win << "), " << input_buf << ".data.size() / sizeof(float));\n";
            out << "        for (size_t k = 0; k < n_bins; k++) {\n";
            out << "            float re = 0, im = 0;\n";
            out << "            for (size_t n = 0; n < N; n++) {\n";
            out << "                float angle = 6.28318530718f * k * n / float(N);\n";
            out << "                re += in[n] * std::cos(angle);\n";
            out << "                im -= in[n] * std::sin(angle);\n";
            out << "            }\n";
            out << "            mag[k] = std::sqrt(re*re + im*im);\n";
            out << "        }\n";
            out << "    }\n";
            break;
        }

        case NodeType::Filter:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data;\n";
            out << "    {\n";
            out << "        float threshold = " << node.config.value("threshold", 0.5f) << "f;\n";
            out << "        uint8_t t = static_cast<uint8_t>(threshold * 255.0f);\n";
            out << "        for (auto& b : buf_" << node.id << "_out0.data) {\n";
            out << "            if (b < t) b = 0;\n";
            out << "        }\n";
            out << "    }\n";
            break;

        case NodeType::Normalize:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data;\n";
            out << "    {\n";
            out << "        float* vals = reinterpret_cast<float*>(buf_" << node.id << "_out0.data.data());\n";
            out << "        size_t count = buf_" << node.id << "_out0.data.size() / sizeof(float);\n";
            out << "        float sum = 0; for (size_t i = 0; i < count; i++) sum += vals[i];\n";
            out << "        float mean = count > 0 ? sum / count : 0;\n";
            out << "        float var = 0; for (size_t i = 0; i < count; i++) { float d = vals[i]-mean; var += d*d; }\n";
            out << "        float sd = count > 0 ? std::sqrt(var/count) : 1; if (sd < 1e-8f) sd = 1;\n";
            out << "        for (size_t i = 0; i < count; i++) vals[i] = (vals[i]-mean)/sd;\n";
            out << "    }\n";
            break;

        case NodeType::NeuralNet:
            out << "    // Neural net inference (VPU)\n";
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data;\n";
            break;

        case NodeType::Tokenize:
            out << "    // Whitespace tokenizer\n";
            out << "    {\n";
            out << "        std::string text(" << input_buf << ".data.begin(), " << input_buf << ".data.end());\n";
            out << "        std::vector<uint32_t> ids;\n";
            out << "        std::istringstream iss(text); std::string w;\n";
            out << "        while (iss >> w && ids.size() < " << node.config.value("max_length", 512) << ") {\n";
            out << "            uint32_t h = 0; for (char c : w) h = h*31 + c;\n";
            out << "            ids.push_back(h);\n";
            out << "        }\n";
            out << "        buf_" << node.id << "_out0.data.resize(ids.size() * sizeof(uint32_t));\n";
            out << "        std::memcpy(buf_" << node.id << "_out0.data.data(), ids.data(), ids.size()*sizeof(uint32_t));\n";
            out << "    }\n";
            break;

        case NodeType::Encode:
            out << "    // Encode (header + payload)\n";
            out << "    {\n";
            out << "        uint32_t sz = static_cast<uint32_t>(" << input_buf << ".data.size());\n";
            out << "        buf_" << node.id << "_out0.data.resize(sizeof(uint32_t) + sz);\n";
            out << "        std::memcpy(buf_" << node.id << "_out0.data.data(), &sz, sizeof(uint32_t));\n";
            out << "        std::memcpy(buf_" << node.id << "_out0.data.data()+sizeof(uint32_t), " << input_buf << ".data.data(), sz);\n";
            out << "    }\n";
            break;

        case NodeType::Decode:
            out << "    // Decode (strip header)\n";
            out << "    if (" << input_buf << ".data.size() > sizeof(uint32_t)) {\n";
            out << "        buf_" << node.id << "_out0.data.assign(\n";
            out << "            " << input_buf << ".data.begin() + sizeof(uint32_t),\n";
            out << "            " << input_buf << ".data.end());\n";
            out << "    }\n";
            break;

        case NodeType::Compress:
            out << "    // RLE compression\n";
            out << "    {\n";
            out << "        auto& src = " << input_buf << ".data;\n";
            out << "        auto& dst = buf_" << node.id << "_out0.data;\n";
            out << "        dst.clear(); dst.reserve(src.size());\n";
            out << "        for (size_t i = 0; i < src.size(); ) {\n";
            out << "            uint8_t val = src[i], run = 1;\n";
            out << "            while (i+run < src.size() && src[i+run]==val && run<255) run++;\n";
            out << "            dst.push_back(run); dst.push_back(val);\n";
            out << "            i += run;\n";
            out << "        }\n";
            out << "    }\n";
            break;

        case NodeType::FileOutput:
            out << "    {\n";
            out << "        std::ofstream ofs(\"" << node.config.value("path", std::string("/tmp/output.bin")) << "\", std::ios::binary);\n";
            out << "        if (ofs.is_open()) {\n";
            out << "            ofs.write(reinterpret_cast<const char*>(" << input_buf << ".data.data()),\n";
            out << "                      " << input_buf << ".data.size());\n";
            out << "        }\n";
            out << "    }\n";
            break;

        case NodeType::Display:
            out << "    // Display sink (no-op in generated code)\n";
            out << "    std::cout << \"Display: \" << " << input_buf << ".data.size() << \" bytes\" << std::endl;\n";
            break;

        case NodeType::NetworkOutput:
            out << "    // Network output sink (placeholder)\n";
            out << "    std::cout << \"Network send: \" << " << input_buf << ".data.size() << \" bytes\" << std::endl;\n";
            break;

        case NodeType::GpuOutput:
            out << "    // GPU upload (placeholder)\n";
            out << "    std::cout << \"GPU upload: \" << " << input_buf << ".data.size() << \" bytes\" << std::endl;\n";
            break;

        case NodeType::AudioOutput:
            out << "    // Audio playback (placeholder)\n";
            out << "    std::cout << \"Audio play: \" << " << input_buf << ".data.size() << \" bytes\" << std::endl;\n";
            break;

        case NodeType::Split:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data;\n";
            out << "    buf_" << node.id << "_out1.data = " << input_buf << ".data;\n";
            break;

        case NodeType::Merge: {
            std::string input_buf_b = find_input_buf(1);
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data;\n";
            out << "    buf_" << node.id << "_out0.data.insert(buf_" << node.id
                << "_out0.data.end(), " << input_buf_b << ".data.begin(), "
                << input_buf_b << ".data.end());\n";
            break;
        }

        case NodeType::Switch:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data;\n";
            break;

        case NodeType::Loop: {
            int iters = node.config.value("iterations", 1);
            out << "    buf_" << node.id << "_out0.data.clear();\n";
            out << "    for (int iter = 0; iter < " << iters << "; iter++) {\n";
            out << "        buf_" << node.id << "_out0.data.insert(\n";
            out << "            buf_" << node.id << "_out0.data.end(),\n";
            out << "            " << input_buf << ".data.begin(), " << input_buf << ".data.end());\n";
            out << "    }\n";
            break;
        }

        case NodeType::Delay:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data; // delay is runtime-only\n";
            break;

        case NodeType::AliceAnalyze:
            out << "    // Alice analysis\n";
            out << "    {\n";
            out << "        std::string analysis = \"{\\\"result\\\":\\\"analyzed\\\",\\\"bytes\\\":\" + std::to_string("
                << input_buf << ".data.size()) + \"}\";\n";
            out << "        buf_" << node.id << "_out0.data.assign(analysis.begin(), analysis.end());\n";
            out << "        buf_" << node.id << "_out1.data = " << input_buf << ".data;\n";
            out << "    }\n";
            break;

        case NodeType::VpuAlloc: {
            size_t sz = node.config.value("size_bytes", 0);
            out << "    buf_" << node.id << "_out0.data.resize(" << sz << ", 0); // VPU alloc placeholder\n";
            break;
        }

        case NodeType::BusTransfer:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data; // bus transfer\n";
            break;

        case NodeType::SwarmDistribute:
            out << "    buf_" << node.id << "_out0.data = " << input_buf << ".data; // swarm distribute\n";
            break;
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// Parallel group detection
// ---------------------------------------------------------------------------

std::vector<std::vector<uint32_t>> CodeGenerator::find_parallel_groups(
    const std::vector<uint32_t>& order) const
{
    // Compute level for each node (longest path from any source)
    std::unordered_map<uint32_t, int> level;
    for (uint32_t nid : order) {
        int lvl = 0;
        for (const auto& c : pipeline_.connections()) {
            if (c.to_node == nid) {
                auto it = level.find(c.from_node);
                if (it != level.end()) {
                    lvl = std::max(lvl, it->second + 1);
                }
            }
        }
        level[nid] = lvl;
    }

    int max_level = 0;
    for (const auto& [nid, lvl] : level) {
        max_level = std::max(max_level, lvl);
    }

    std::vector<std::vector<uint32_t>> groups;
    for (int l = 0; l <= max_level; l++) {
        std::vector<uint32_t> group;
        for (uint32_t nid : order) {
            if (level[nid] == l) group.push_back(nid);
        }
        if (!group.empty()) groups.push_back(std::move(group));
    }

    return groups;
}

// ---------------------------------------------------------------------------
// Body emission
// ---------------------------------------------------------------------------

std::string CodeGenerator::emit_body(const std::vector<uint32_t>& order) const {
    std::ostringstream out;
    auto groups = find_parallel_groups(order);

    for (size_t gi = 0; gi < groups.size(); gi++) {
        const auto& group = groups[gi];
        bool parallel = group.size() > 1;

        if (parallel) {
            out << "    // --- Level " << gi << " (parallel: " << group.size() << " nodes) ---\n";
            out << "    {\n";
            out << "        std::vector<std::future<void>> futures;\n";
            for (uint32_t nid : group) {
                const PipeNode* n = pipeline_.find_node(nid);
                if (!n) continue;
                out << "        futures.push_back(std::async(std::launch::async, [&]() {\n";
                out << "        // " << n->label << "\n";
                // Indent the node call one extra level
                std::string call = emit_node_call(*n, 0);
                // Add extra indentation
                std::istringstream iss(call);
                std::string line;
                while (std::getline(iss, line)) {
                    out << "    " << line << "\n";
                }
                out << "        }));\n";
            }
            out << "        for (auto& f : futures) f.get();\n";
            out << "    }\n\n";
        } else {
            for (uint32_t nid : group) {
                const PipeNode* n = pipeline_.find_node(nid);
                if (!n) continue;
                out << "    // " << n->label << "\n";
                out << emit_node_call(*n, 0);
                out << "\n";
            }
        }
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// Full generation
// ---------------------------------------------------------------------------

Result<std::string, std::string> CodeGenerator::generate() const {
    auto valid = pipeline_.validate();
    if (!valid.has_value()) {
        return Result<std::string, std::string>::error(valid.error());
    }

    // Get topological order
    // We access the private method via pipeline_.generate_code()'s logic,
    // but since CodeGenerator is a friend-like external class, we re-derive
    // the topo order from the public API by validating + iterating connections.
    // For simplicity, we build our own topo sort here.

    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    std::unordered_map<uint32_t, int> in_degree;

    for (const auto& n : pipeline_.nodes()) {
        adj[n.id];
        in_degree[n.id] = 0;
    }
    for (const auto& c : pipeline_.connections()) {
        adj[c.from_node].push_back(c.to_node);
        in_degree[c.to_node]++;
    }

    std::vector<uint32_t> order;
    std::queue<uint32_t> ready;
    for (const auto& [nid, deg] : in_degree) {
        if (deg == 0) ready.push(nid);
    }
    while (!ready.empty()) {
        uint32_t cur = ready.front(); ready.pop();
        order.push_back(cur);
        for (uint32_t next : adj[cur]) {
            if (--in_degree[next] == 0) ready.push(next);
        }
    }

    if (order.size() != pipeline_.nodes().size()) {
        return Result<std::string, std::string>::error("Cycle detected during code generation");
    }

    std::ostringstream out;
    out << "// Generated by StrayLight Pipe — Visual Dataflow Pipeline Builder\n";
    out << "// Do not edit manually.\n\n";
    out << emit_includes(order);
    out << "\n";
    out << "// Error handling\n";
    out << "template <typename T, typename E>\n";
    out << "struct Result {\n";
    out << "    bool ok;\n";
    out << "    T value;\n";
    out << "    E error_msg;\n";
    out << "    static Result success(T v) { return {true, std::move(v), {}}; }\n";
    out << "    static Result fail(E e) { return {false, {}, std::move(e)}; }\n";
    out << "};\n\n";
    out << emit_buffers(order);
    out << "int main() {\n";
    out << emit_body(order);
    out << "    std::cout << \"Pipeline execution complete.\" << std::endl;\n";
    out << "    return 0;\n";
    out << "}\n";

    return Result<std::string, std::string>::ok(out.str());
}

} // namespace straylight::pipe
