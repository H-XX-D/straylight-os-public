// apps/pipe/node_editor.h
// StrayLight Pipe — ImGui node graph renderer with pan/zoom, selection,
// Bezier connections, palette sidebar, and mini-map.
#pragma once

#include "pipeline.h"

#include <imgui.h>

#include <deque>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace straylight::pipe {

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

struct UndoAddNode      { PipeNode node; };
struct UndoRemoveNode   { PipeNode node; std::vector<PipeConnection> severed; };
struct UndoAddConn      { PipeConnection conn; };
struct UndoRemoveConn   { PipeConnection conn; };
struct UndoMoveNodes    { std::vector<std::pair<uint32_t, ImVec2>> old_positions; };

using UndoAction = std::variant<UndoAddNode, UndoRemoveNode,
                                UndoAddConn, UndoRemoveConn,
                                UndoMoveNodes>;

// ---------------------------------------------------------------------------
// NodeEditor
// ---------------------------------------------------------------------------

class NodeEditor {
public:
    explicit NodeEditor(Pipeline& pipeline);

    /// Draw the full editor UI into the current ImGui window region.
    void render();

    /// Check if any node is selected.
    bool has_selection() const { return !selected_nodes_.empty(); }

    /// Delete all selected nodes.
    void delete_selected();

    /// Undo the last action.
    void undo();

    /// Redo the last undone action.
    void redo();

private:
    // ---- Sub-renderers ----
    void render_grid(ImDrawList* draw, ImVec2 origin, ImVec2 size);
    void render_connections(ImDrawList* draw, ImVec2 origin);
    void render_nodes(ImDrawList* draw, ImVec2 origin);
    void render_node(ImDrawList* draw, ImVec2 origin, PipeNode& node);
    void render_pending_connection(ImDrawList* draw, ImVec2 origin);
    void render_palette();
    void render_minimap(ImDrawList* draw, ImVec2 canvas_min, ImVec2 canvas_size);
    void render_context_menu();

    // ---- Interaction ----
    void handle_canvas_input(ImVec2 canvas_min, ImVec2 canvas_size);

    // ---- Helpers ----
    ImVec2 screen_pos(ImVec2 origin, ImVec2 node_pos) const;
    ImVec2 canvas_pos(ImVec2 origin, ImVec2 screen) const;

    ImVec2 port_screen_pos(ImVec2 origin, const PipeNode& node,
                            const Port& port, bool is_input) const;

    /// Push an undo action.
    void push_undo(UndoAction action);

    Pipeline& pipeline_;

    // Canvas state
    ImVec2 scroll_offset_ = {0.0f, 0.0f};
    float  zoom_          = 1.0f;

    // Selection
    std::set<uint32_t> selected_nodes_;
    bool dragging_nodes_  = false;
    ImVec2 drag_start_    = {0.0f, 0.0f};

    // Connection dragging
    bool     connecting_       = false;
    uint32_t connect_from_node_ = 0;
    uint32_t connect_from_port_ = 0;
    ImVec2   connect_end_      = {0.0f, 0.0f};

    // Context menu
    bool   context_menu_open_  = false;
    ImVec2 context_menu_pos_   = {0.0f, 0.0f};

    // Palette sidebar
    bool palette_open_ = true;
    static constexpr float PALETTE_WIDTH = 220.0f;

    // Node sizing
    static constexpr float NODE_WIDTH     = 180.0f;
    static constexpr float NODE_HEADER_H  = 28.0f;
    static constexpr float PORT_RADIUS    = 6.0f;
    static constexpr float PORT_SPACING   = 24.0f;

    // Minimap
    static constexpr float MINIMAP_SIZE   = 160.0f;
    static constexpr float MINIMAP_MARGIN = 12.0f;

    // Undo / redo
    std::deque<UndoAction> undo_stack_;
    std::deque<UndoAction> redo_stack_;
    static constexpr size_t MAX_UNDO = 200;
};

} // namespace straylight::pipe
