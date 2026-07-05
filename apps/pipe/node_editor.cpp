// apps/pipe/node_editor.cpp
// StrayLight Pipe — ImGui node graph renderer with full interaction.
#include "node_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace straylight::pipe {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NodeEditor::NodeEditor(Pipeline& pipeline)
    : pipeline_(pipeline) {}

// ---------------------------------------------------------------------------
// Coordinate helpers
// ---------------------------------------------------------------------------

ImVec2 NodeEditor::screen_pos(ImVec2 origin, ImVec2 node_pos) const {
    return ImVec2(origin.x + (node_pos.x + scroll_offset_.x) * zoom_,
                  origin.y + (node_pos.y + scroll_offset_.y) * zoom_);
}

ImVec2 NodeEditor::canvas_pos(ImVec2 origin, ImVec2 screen) const {
    return ImVec2((screen.x - origin.x) / zoom_ - scroll_offset_.x,
                  (screen.y - origin.y) / zoom_ - scroll_offset_.y);
}

ImVec2 NodeEditor::port_screen_pos(ImVec2 origin, const PipeNode& node,
                                    const Port& port, bool is_input) const {
    float x_offset = is_input ? 0.0f : NODE_WIDTH;

    // Find port index
    int idx = 0;
    const auto& ports = is_input ? node.inputs : node.outputs;
    for (const auto& p : ports) {
        if (p.id == port.id) break;
        idx++;
    }

    float y_offset = NODE_HEADER_H + PORT_SPACING * (0.5f + idx);

    ImVec2 np = screen_pos(origin, node.position);
    return ImVec2(np.x + x_offset * zoom_, np.y + y_offset * zoom_);
}

// ---------------------------------------------------------------------------
// Undo / Redo
// ---------------------------------------------------------------------------

void NodeEditor::push_undo(UndoAction action) {
    if (undo_stack_.size() >= MAX_UNDO) {
        undo_stack_.pop_front();
    }
    undo_stack_.push_back(std::move(action));
    redo_stack_.clear();
}

void NodeEditor::undo() {
    if (undo_stack_.empty()) return;

    auto action = std::move(undo_stack_.back());
    undo_stack_.pop_back();

    std::visit([&](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, UndoAddNode>) {
            pipeline_.remove_node(a.node.id);
            redo_stack_.push_back(UndoRemoveNode{a.node, {}});
        } else if constexpr (std::is_same_v<T, UndoRemoveNode>) {
            // Re-add the node (manually, since add_node auto-assigns ID)
            // We push the saved node back directly
            // For simplicity, we re-add and adjust
            uint32_t new_id = pipeline_.add_node(a.node.type, a.node.position);
            auto* n = pipeline_.find_node(new_id);
            if (n) {
                n->label  = a.node.label;
                n->config = a.node.config;
            }
            // Re-add severed connections
            for (const auto& c : a.severed) {
                uint32_t fn = (c.from_node == a.node.id) ? new_id : c.from_node;
                uint32_t tn = (c.to_node == a.node.id) ? new_id : c.to_node;
                pipeline_.connect(fn, c.from_port, tn, c.to_port);
            }
            redo_stack_.push_back(UndoAddNode{a.node});
        } else if constexpr (std::is_same_v<T, UndoAddConn>) {
            pipeline_.disconnect(a.conn.from_node, a.conn.from_port);
            redo_stack_.push_back(UndoRemoveConn{a.conn});
        } else if constexpr (std::is_same_v<T, UndoRemoveConn>) {
            pipeline_.connect(a.conn.from_node, a.conn.from_port,
                              a.conn.to_node, a.conn.to_port);
            redo_stack_.push_back(UndoAddConn{a.conn});
        } else if constexpr (std::is_same_v<T, UndoMoveNodes>) {
            UndoMoveNodes reverse;
            for (const auto& [nid, old_pos] : a.old_positions) {
                auto* n = pipeline_.find_node(nid);
                if (n) {
                    reverse.old_positions.push_back({nid, n->position});
                    n->position = old_pos;
                }
            }
            redo_stack_.push_back(std::move(reverse));
        }
    }, action);
}

void NodeEditor::redo() {
    if (redo_stack_.empty()) return;

    auto action = std::move(redo_stack_.back());
    redo_stack_.pop_back();

    std::visit([&](auto&& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, UndoAddNode>) {
            pipeline_.remove_node(a.node.id);
            undo_stack_.push_back(UndoRemoveNode{a.node, {}});
        } else if constexpr (std::is_same_v<T, UndoRemoveNode>) {
            uint32_t new_id = pipeline_.add_node(a.node.type, a.node.position);
            auto* n = pipeline_.find_node(new_id);
            if (n) {
                n->label  = a.node.label;
                n->config = a.node.config;
            }
            undo_stack_.push_back(UndoAddNode{a.node});
        } else if constexpr (std::is_same_v<T, UndoAddConn>) {
            pipeline_.disconnect(a.conn.from_node, a.conn.from_port);
            undo_stack_.push_back(UndoRemoveConn{a.conn});
        } else if constexpr (std::is_same_v<T, UndoRemoveConn>) {
            pipeline_.connect(a.conn.from_node, a.conn.from_port,
                              a.conn.to_node, a.conn.to_port);
            undo_stack_.push_back(UndoAddConn{a.conn});
        } else if constexpr (std::is_same_v<T, UndoMoveNodes>) {
            UndoMoveNodes reverse;
            for (const auto& [nid, old_pos] : a.old_positions) {
                auto* n = pipeline_.find_node(nid);
                if (n) {
                    reverse.old_positions.push_back({nid, n->position});
                    n->position = old_pos;
                }
            }
            undo_stack_.push_back(std::move(reverse));
        }
    }, action);
}

// ---------------------------------------------------------------------------
// Delete selected
// ---------------------------------------------------------------------------

void NodeEditor::delete_selected() {
    for (uint32_t nid : selected_nodes_) {
        const PipeNode* node = pipeline_.find_node(nid);
        if (!node) continue;

        // Collect severed connections for undo
        std::vector<PipeConnection> severed;
        for (const auto& c : pipeline_.connections()) {
            if (c.from_node == nid || c.to_node == nid) {
                severed.push_back(c);
            }
        }

        push_undo(UndoRemoveNode{*node, severed});
        pipeline_.remove_node(nid);
    }
    selected_nodes_.clear();
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

void NodeEditor::render_grid(ImDrawList* draw, ImVec2 origin, ImVec2 size) {
    float grid_step = 32.0f * zoom_;
    ImU32 grid_color      = IM_COL32(40, 40, 60, 200);
    ImU32 grid_color_bold = IM_COL32(50, 50, 80, 200);

    float ox = fmodf(scroll_offset_.x * zoom_, grid_step);
    float oy = fmodf(scroll_offset_.y * zoom_, grid_step);

    int line_x = 0;
    for (float x = origin.x + ox; x < origin.x + size.x; x += grid_step, line_x++) {
        ImU32 col = (line_x % 4 == 0) ? grid_color_bold : grid_color;
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), col);
    }

    int line_y = 0;
    for (float y = origin.y + oy; y < origin.y + size.y; y += grid_step, line_y++) {
        ImU32 col = (line_y % 4 == 0) ? grid_color_bold : grid_color;
        draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), col);
    }
}

// ---------------------------------------------------------------------------
// Connection rendering
// ---------------------------------------------------------------------------

void NodeEditor::render_connections(ImDrawList* draw, ImVec2 origin) {
    for (const auto& conn : pipeline_.connections()) {
        const PipeNode* from_node = pipeline_.find_node(conn.from_node);
        const PipeNode* to_node   = pipeline_.find_node(conn.to_node);
        if (!from_node || !to_node) continue;

        // Find the actual ports
        const Port* from_port = nullptr;
        for (const auto& p : from_node->outputs) {
            if (p.id == conn.from_port) { from_port = &p; break; }
        }
        const Port* to_port = nullptr;
        for (const auto& p : to_node->inputs) {
            if (p.id == conn.to_port) { to_port = &p; break; }
        }
        if (!from_port || !to_port) continue;

        ImVec2 p1 = port_screen_pos(origin, *from_node, *from_port, false);
        ImVec2 p4 = port_screen_pos(origin, *to_node, *to_port, true);

        // Control points for cubic Bezier
        float dx = std::abs(p4.x - p1.x) * 0.5f;
        if (dx < 50.0f * zoom_) dx = 50.0f * zoom_;
        ImVec2 p2(p1.x + dx, p1.y);
        ImVec2 p3(p4.x - dx, p4.y);

        ImVec4 color4 = port_data_type_color(from_port->data_type);
        ImU32 color = ImGui::ColorConvertFloat4ToU32(color4);

        draw->AddBezierCubic(p1, p2, p3, p4, color, 2.5f * zoom_);
    }
}

// ---------------------------------------------------------------------------
// Pending connection (while dragging)
// ---------------------------------------------------------------------------

void NodeEditor::render_pending_connection(ImDrawList* draw, ImVec2 origin) {
    if (!connecting_) return;

    const PipeNode* from_node = pipeline_.find_node(connect_from_node_);
    if (!from_node) return;

    const Port* from_port = nullptr;
    for (const auto& p : from_node->outputs) {
        if (p.id == connect_from_port_) { from_port = &p; break; }
    }
    if (!from_port) return;

    ImVec2 p1 = port_screen_pos(origin, *from_node, *from_port, false);
    ImVec2 p4 = connect_end_;

    float dx = std::abs(p4.x - p1.x) * 0.5f;
    if (dx < 50.0f * zoom_) dx = 50.0f * zoom_;
    ImVec2 p2(p1.x + dx, p1.y);
    ImVec2 p3(p4.x - dx, p4.y);

    ImU32 color = IM_COL32(255, 255, 255, 150);
    draw->AddBezierCubic(p1, p2, p3, p4, color, 2.0f * zoom_);
}

// ---------------------------------------------------------------------------
// Node rendering
// ---------------------------------------------------------------------------

float node_height(const PipeNode& node) {
    int port_count = std::max(static_cast<int>(node.inputs.size()),
                               static_cast<int>(node.outputs.size()));
    return 28.0f + 24.0f * std::max(port_count, 1) + 8.0f;
}

void NodeEditor::render_node(ImDrawList* draw, ImVec2 origin, PipeNode& node) {
    ImVec2 node_screen = screen_pos(origin, node.position);
    float w = NODE_WIDTH * zoom_;
    float h = node_height(node) * zoom_;

    ImVec2 tl = node_screen;
    ImVec2 br(tl.x + w, tl.y + h);

    bool selected = selected_nodes_.count(node.id) > 0;

    // Background
    ImU32 bg_color = selected ? IM_COL32(50, 50, 80, 230) : IM_COL32(35, 35, 55, 230);
    draw->AddRectFilled(tl, br, bg_color, 6.0f * zoom_);

    // Header bar
    ImVec4 hdr_color4 = category_color(category_of(node.type));
    ImU32 hdr_color = ImGui::ColorConvertFloat4ToU32(hdr_color4);
    ImVec2 hdr_br(tl.x + w, tl.y + NODE_HEADER_H * zoom_);
    draw->AddRectFilled(tl, hdr_br, hdr_color, 6.0f * zoom_, ImDrawFlags_RoundCornersTop);

    // Title text
    float font_size = 13.0f * zoom_;
    draw->AddText(nullptr, font_size,
                  ImVec2(tl.x + 8.0f * zoom_, tl.y + 6.0f * zoom_),
                  IM_COL32(255, 255, 255, 255), node.label.c_str());

    // Executed indicator
    if (node.executed) {
        draw->AddCircleFilled(
            ImVec2(br.x - 10.0f * zoom_, tl.y + NODE_HEADER_H * 0.5f * zoom_),
            4.0f * zoom_, IM_COL32(0, 255, 100, 255));
    }

    // Input ports
    for (int i = 0; i < static_cast<int>(node.inputs.size()); i++) {
        const auto& port = node.inputs[i];
        ImVec2 pp = port_screen_pos(origin, node, port, true);
        ImVec4 pc4 = port_data_type_color(port.data_type);
        ImU32 pc = ImGui::ColorConvertFloat4ToU32(pc4);
        draw->AddCircleFilled(pp, PORT_RADIUS * zoom_, pc);
        draw->AddCircle(pp, PORT_RADIUS * zoom_, IM_COL32(200, 200, 200, 200), 12, 1.0f);
        draw->AddText(nullptr, 11.0f * zoom_,
                      ImVec2(pp.x + (PORT_RADIUS + 4.0f) * zoom_,
                             pp.y - 5.0f * zoom_),
                      IM_COL32(200, 200, 200, 255), port.label.c_str());
    }

    // Output ports
    for (int i = 0; i < static_cast<int>(node.outputs.size()); i++) {
        const auto& port = node.outputs[i];
        ImVec2 pp = port_screen_pos(origin, node, port, false);
        ImVec4 pc4 = port_data_type_color(port.data_type);
        ImU32 pc = ImGui::ColorConvertFloat4ToU32(pc4);
        draw->AddCircleFilled(pp, PORT_RADIUS * zoom_, pc);
        draw->AddCircle(pp, PORT_RADIUS * zoom_, IM_COL32(200, 200, 200, 200), 12, 1.0f);

        // Right-aligned label
        const char* lbl = port.label.c_str();
        ImVec2 txt_size = ImGui::CalcTextSize(lbl);
        draw->AddText(nullptr, 11.0f * zoom_,
                      ImVec2(pp.x - (PORT_RADIUS + 4.0f) * zoom_ - txt_size.x * zoom_ * 0.85f,
                             pp.y - 5.0f * zoom_),
                      IM_COL32(200, 200, 200, 255), lbl);
    }

    // Border
    ImU32 border_color = selected ? IM_COL32(0, 255, 170, 255) : IM_COL32(80, 80, 120, 200);
    draw->AddRect(tl, br, border_color, 6.0f * zoom_, 0, selected ? 2.0f : 1.0f);
}

void NodeEditor::render_nodes(ImDrawList* draw, ImVec2 origin) {
    // We need a mutable reference to set executed flags etc.
    // pipeline_.nodes() returns const, so we iterate by ID
    for (const auto& node : pipeline_.nodes()) {
        PipeNode* mutable_node = pipeline_.find_node(node.id);
        if (mutable_node) {
            render_node(draw, origin, *mutable_node);
        }
    }
}

// ---------------------------------------------------------------------------
// Mini-map
// ---------------------------------------------------------------------------

void NodeEditor::render_minimap(ImDrawList* draw, ImVec2 canvas_min, ImVec2 canvas_size) {
    if (pipeline_.nodes().empty()) return;

    float mm_size = MINIMAP_SIZE;
    ImVec2 mm_pos(canvas_min.x + canvas_size.x - mm_size - MINIMAP_MARGIN,
                  canvas_min.y + canvas_size.y - mm_size - MINIMAP_MARGIN);

    // Background
    draw->AddRectFilled(mm_pos, ImVec2(mm_pos.x + mm_size, mm_pos.y + mm_size),
                        IM_COL32(20, 20, 35, 200), 4.0f);
    draw->AddRect(mm_pos, ImVec2(mm_pos.x + mm_size, mm_pos.y + mm_size),
                  IM_COL32(60, 60, 100, 200), 4.0f);

    // Compute bounds of all nodes
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    for (const auto& n : pipeline_.nodes()) {
        min_x = std::min(min_x, n.position.x);
        min_y = std::min(min_y, n.position.y);
        max_x = std::max(max_x, n.position.x + NODE_WIDTH);
        max_y = std::max(max_y, n.position.y + node_height(n));
    }

    float range_x = max_x - min_x;
    float range_y = max_y - min_y;
    if (range_x < 1.0f) range_x = 1.0f;
    if (range_y < 1.0f) range_y = 1.0f;

    float padding = 20.0f;
    float scale = std::min((mm_size - padding * 2) / range_x,
                            (mm_size - padding * 2) / range_y);

    auto mm_transform = [&](ImVec2 pos) -> ImVec2 {
        return ImVec2(mm_pos.x + padding + (pos.x - min_x) * scale,
                      mm_pos.y + padding + (pos.y - min_y) * scale);
    };

    // Draw nodes as small rectangles
    for (const auto& n : pipeline_.nodes()) {
        ImVec2 ntl = mm_transform(n.position);
        ImVec2 nbr = mm_transform(ImVec2(n.position.x + NODE_WIDTH,
                                          n.position.y + node_height(n)));
        ImVec4 col4 = category_color(category_of(n.type));
        ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(col4.x, col4.y, col4.z, 0.7f));
        draw->AddRectFilled(ntl, nbr, col, 1.0f);
    }

    // Draw connections as lines
    for (const auto& conn : pipeline_.connections()) {
        const PipeNode* fn = pipeline_.find_node(conn.from_node);
        const PipeNode* tn = pipeline_.find_node(conn.to_node);
        if (!fn || !tn) continue;
        ImVec2 fp = mm_transform(ImVec2(fn->position.x + NODE_WIDTH,
                                         fn->position.y + node_height(*fn) * 0.5f));
        ImVec2 tp = mm_transform(ImVec2(tn->position.x,
                                         tn->position.y + node_height(*tn) * 0.5f));
        draw->AddLine(fp, tp, IM_COL32(150, 150, 200, 120), 1.0f);
    }

    // Draw viewport rectangle
    float vw = canvas_size.x / zoom_;
    float vh = canvas_size.y / zoom_;
    ImVec2 vtl = mm_transform(ImVec2(-scroll_offset_.x, -scroll_offset_.y));
    ImVec2 vbr = mm_transform(ImVec2(-scroll_offset_.x + vw, -scroll_offset_.y + vh));
    draw->AddRect(vtl, vbr, IM_COL32(0, 255, 170, 200), 1.0f, 0, 1.5f);
}

// ---------------------------------------------------------------------------
// Palette sidebar
// ---------------------------------------------------------------------------

void NodeEditor::render_palette() {
    if (!palette_open_) return;

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(PALETTE_WIDTH, ImGui::GetIO().DisplaySize.y),
                              ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("Node Palette", &palette_open_, flags)) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "NODE PALETTE");
        ImGui::Separator();

        struct Category {
            const char*           name;
            NodeCategory          cat;
            std::vector<NodeType> types;
        };

        Category categories[] = {
            {"Sources", NodeCategory::Source,
             {NodeType::FileInput, NodeType::CameraInput, NodeType::MicInput,
              NodeType::NetworkInput, NodeType::GpuBuffer}},
            {"Processors", NodeCategory::Processor,
             {NodeType::Resize, NodeType::Crop, NodeType::ColorConvert,
              NodeType::FFT, NodeType::Filter, NodeType::Normalize,
              NodeType::NeuralNet, NodeType::Tokenize, NodeType::Encode,
              NodeType::Decode, NodeType::Compress}},
            {"Sinks", NodeCategory::Sink,
             {NodeType::FileOutput, NodeType::Display, NodeType::NetworkOutput,
              NodeType::GpuOutput, NodeType::AudioOutput}},
            {"Control", NodeCategory::Control,
             {NodeType::Split, NodeType::Merge, NodeType::Switch,
              NodeType::Loop, NodeType::Delay}},
            {"StrayLight", NodeCategory::StrayLight,
             {NodeType::AliceAnalyze, NodeType::VpuAlloc,
              NodeType::BusTransfer, NodeType::SwarmDistribute}},
        };

        for (auto& cat : categories) {
            ImVec4 col = category_color(cat.cat);
            ImGui::PushStyleColor(ImGuiCol_Header, col);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                   ImVec4(col.x * 1.2f, col.y * 1.2f, col.z * 1.2f, 1.0f));

            if (ImGui::CollapsingHeader(cat.name, ImGuiTreeNodeFlags_DefaultOpen)) {
                for (NodeType nt : cat.types) {
                    if (ImGui::Button(node_type_name(nt), ImVec2(PALETTE_WIDTH - 30, 0))) {
                        // Place new node at center of current view
                        ImVec2 center(-scroll_offset_.x + 400.0f / zoom_,
                                      -scroll_offset_.y + 300.0f / zoom_);
                        uint32_t nid = pipeline_.add_node(nt, center);
                        const PipeNode* n = pipeline_.find_node(nid);
                        if (n) push_undo(UndoAddNode{*n});
                    }
                }
            }

            ImGui::PopStyleColor(2);
        }

        ImGui::Separator();

        // Pipeline actions
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "ACTIONS");

        if (ImGui::Button("Validate", ImVec2(PALETTE_WIDTH - 30, 0))) {
            auto result = pipeline_.validate();
            if (result.has_value()) {
                ImGui::OpenPopup("ValidateOK");
            } else {
                ImGui::OpenPopup("ValidateErr");
            }
        }

        if (ImGui::Button("Execute", ImVec2(PALETTE_WIDTH - 30, 0))) {
            pipeline_.execute();
        }

        if (ImGui::Button("Generate C++", ImVec2(PALETTE_WIDTH - 30, 0))) {
            auto code = pipeline_.generate_code();
            if (code.has_value()) {
                ImGui::SetClipboardText(code.value().c_str());
            }
        }

        if (ImGui::Button("Clear All", ImVec2(PALETTE_WIDTH - 30, 0))) {
            pipeline_.clear();
            undo_stack_.clear();
            redo_stack_.clear();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void NodeEditor::render_context_menu() {
    if (context_menu_open_) {
        ImGui::OpenPopup("NodeContextMenu");
        context_menu_open_ = false;
    }

    if (ImGui::BeginPopup("NodeContextMenu")) {
        if (!selected_nodes_.empty()) {
            if (ImGui::MenuItem("Delete Selected")) {
                delete_selected();
            }
            if (ImGui::MenuItem("Duplicate Selected")) {
                std::set<uint32_t> new_selection;
                for (uint32_t nid : selected_nodes_) {
                    const PipeNode* orig = pipeline_.find_node(nid);
                    if (!orig) continue;
                    ImVec2 offset_pos(orig->position.x + 40.0f, orig->position.y + 40.0f);
                    uint32_t new_id = pipeline_.add_node(orig->type, offset_pos);
                    PipeNode* n = pipeline_.find_node(new_id);
                    if (n) {
                        n->label  = orig->label;
                        n->config = orig->config;
                        push_undo(UndoAddNode{*n});
                        new_selection.insert(new_id);
                    }
                }
                selected_nodes_ = new_selection;
            }
        }
        ImGui::Separator();

        if (ImGui::BeginMenu("Add Node")) {
            struct { const char* name; NodeType type; } items[] = {
                {"File Input",        NodeType::FileInput},
                {"Camera Input",      NodeType::CameraInput},
                {"Resize",            NodeType::Resize},
                {"Neural Net",        NodeType::NeuralNet},
                {"File Output",       NodeType::FileOutput},
                {"Display",           NodeType::Display},
                {"Alice Analyze",     NodeType::AliceAnalyze},
                {"Swarm Distribute",  NodeType::SwarmDistribute},
            };
            for (auto& item : items) {
                if (ImGui::MenuItem(item.name)) {
                    uint32_t nid = pipeline_.add_node(item.type, context_menu_pos_);
                    const PipeNode* n = pipeline_.find_node(nid);
                    if (n) push_undo(UndoAddNode{*n});
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Fit View")) {
            // Reset scroll and zoom to show all nodes
            if (!pipeline_.nodes().empty()) {
                float min_x = 1e9f, min_y = 1e9f;
                for (const auto& n : pipeline_.nodes()) {
                    min_x = std::min(min_x, n.position.x);
                    min_y = std::min(min_y, n.position.y);
                }
                scroll_offset_ = ImVec2(-min_x + 50.0f, -min_y + 50.0f);
                zoom_ = 1.0f;
            }
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Canvas input handling
// ---------------------------------------------------------------------------

void NodeEditor::handle_canvas_input(ImVec2 canvas_min, ImVec2 canvas_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;

    bool hovered = (mouse.x >= canvas_min.x && mouse.x < canvas_min.x + canvas_size.x &&
                    mouse.y >= canvas_min.y && mouse.y < canvas_min.y + canvas_size.y);
    if (!hovered) return;

    // Zoom with scroll wheel
    if (std::abs(io.MouseWheel) > 0.0f) {
        float old_zoom = zoom_;
        zoom_ *= (io.MouseWheel > 0) ? 1.1f : (1.0f / 1.1f);
        zoom_ = std::max(0.15f, std::min(zoom_, 4.0f));

        // Zoom toward mouse position
        ImVec2 mouse_canvas = canvas_pos(canvas_min, mouse);
        scroll_offset_.x += mouse_canvas.x * (1.0f - old_zoom / zoom_);
        scroll_offset_.y += mouse_canvas.y * (1.0f - old_zoom / zoom_);
    }

    // Right-click context menu
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        context_menu_open_ = true;
        context_menu_pos_  = canvas_pos(canvas_min, mouse);
    }

    // Keyboard shortcuts
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (io.KeyShift) {
            redo();
        } else {
            undo();
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (!selected_nodes_.empty()) {
            delete_selected();
        }
    }

    // Check if mouse is over a port (for connection dragging)
    auto find_port_at = [&](ImVec2 pos, uint32_t& out_node, uint32_t& out_port, bool& out_input) -> bool {
        float hit_radius = (PORT_RADIUS + 4.0f) * zoom_;
        for (const auto& node : pipeline_.nodes()) {
            for (const auto& port : node.outputs) {
                ImVec2 pp = port_screen_pos(canvas_min, node, port, false);
                float dx = pos.x - pp.x;
                float dy = pos.y - pp.y;
                if (dx * dx + dy * dy < hit_radius * hit_radius) {
                    out_node  = node.id;
                    out_port  = port.id;
                    out_input = false;
                    return true;
                }
            }
            for (const auto& port : node.inputs) {
                ImVec2 pp = port_screen_pos(canvas_min, node, port, true);
                float dx = pos.x - pp.x;
                float dy = pos.y - pp.y;
                if (dx * dx + dy * dy < hit_radius * hit_radius) {
                    out_node  = node.id;
                    out_port  = port.id;
                    out_input = true;
                    return true;
                }
            }
        }
        return false;
    };

    // Mouse down on left button
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        uint32_t hit_node = 0, hit_port = 0;
        bool hit_input = false;

        if (find_port_at(mouse, hit_node, hit_port, hit_input)) {
            if (!hit_input) {
                // Start connection from output port
                connecting_       = true;
                connect_from_node_ = hit_node;
                connect_from_port_ = hit_port;
                connect_end_      = mouse;
            }
        } else {
            // Check if we clicked on a node body
            bool clicked_on_node = false;
            for (const auto& node : pipeline_.nodes()) {
                ImVec2 ns = screen_pos(canvas_min, node.position);
                float w = NODE_WIDTH * zoom_;
                float h = node_height(node) * zoom_;
                if (mouse.x >= ns.x && mouse.x < ns.x + w &&
                    mouse.y >= ns.y && mouse.y < ns.y + h) {
                    clicked_on_node = true;
                    if (!io.KeyCtrl) {
                        selected_nodes_.clear();
                    }
                    selected_nodes_.insert(node.id);
                    dragging_nodes_ = true;
                    drag_start_     = mouse;
                    break;
                }
            }

            if (!clicked_on_node) {
                // Canvas pan or deselect
                if (!io.KeyCtrl) {
                    selected_nodes_.clear();
                }
            }
        }
    }

    // Mouse dragging
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (connecting_) {
            connect_end_ = mouse;
        } else if (dragging_nodes_ && !selected_nodes_.empty()) {
            ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            for (uint32_t nid : selected_nodes_) {
                PipeNode* n = pipeline_.find_node(nid);
                if (n) {
                    n->position.x += delta.x / zoom_;
                    n->position.y += delta.y / zoom_;
                }
            }
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        } else if (!connecting_ && !dragging_nodes_) {
            // Pan canvas
            ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            scroll_offset_.x += delta.x / zoom_;
            scroll_offset_.y += delta.y / zoom_;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }
    }

    // Mouse release
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (connecting_) {
            // Check if released on an input port
            uint32_t hit_node = 0, hit_port = 0;
            bool hit_input = false;
            if (find_port_at(mouse, hit_node, hit_port, hit_input) && hit_input) {
                auto result = pipeline_.connect(connect_from_node_, connect_from_port_,
                                                 hit_node, hit_port);
                if (result.has_value()) {
                    // Find the connection we just made for undo
                    const auto& conns = pipeline_.connections();
                    if (!conns.empty()) {
                        push_undo(UndoAddConn{conns.back()});
                    }
                }
            }
            connecting_ = false;
        }
        dragging_nodes_ = false;
    }

    // Middle mouse pan
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        scroll_offset_.x += delta.x / zoom_;
        scroll_offset_.y += delta.y / zoom_;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    }
}

// ---------------------------------------------------------------------------
// Main render entry point
// ---------------------------------------------------------------------------

void NodeEditor::render() {
    // Draw palette sidebar
    render_palette();

    // Canvas area (offset by palette width if open)
    float canvas_x = palette_open_ ? PALETTE_WIDTH : 0.0f;
    ImVec2 canvas_min(canvas_x, 0.0f);
    ImVec2 canvas_size(ImGui::GetIO().DisplaySize.x - canvas_x,
                        ImGui::GetIO().DisplaySize.y);

    ImGui::SetNextWindowPos(canvas_min);
    ImGui::SetNextWindowSize(canvas_size);

    ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##Canvas", nullptr, wflags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();

        render_grid(draw, canvas_min, canvas_size);
        render_connections(draw, canvas_min);
        render_nodes(draw, canvas_min);
        render_pending_connection(draw, canvas_min);
        render_minimap(draw, canvas_min, canvas_size);
        render_context_menu();

        handle_canvas_input(canvas_min, canvas_size);

        // Status bar
        char status[256];
        std::snprintf(status, sizeof(status),
                      "Nodes: %zu | Connections: %zu | Zoom: %.0f%% | Selected: %zu",
                      pipeline_.nodes().size(), pipeline_.connections().size(),
                      zoom_ * 100.0f, selected_nodes_.size());
        ImVec2 status_pos(canvas_min.x + 8.0f,
                          canvas_min.y + canvas_size.y - 22.0f);
        draw->AddText(status_pos, IM_COL32(150, 150, 180, 255), status);
    }
    ImGui::End();
}

} // namespace straylight::pipe
