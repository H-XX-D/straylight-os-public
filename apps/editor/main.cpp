// apps/editor/main.cpp
// StrayLight Editor — Wayland + EGL + ImGui code/text editor
#include "buffer.h"
#include "search.h"
#include "syntax.h"

#include <straylight/log.h>

#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <xdg-shell-client-protocol.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::editor;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Wayland boilerplate
// ---------------------------------------------------------------------------

struct WaylandState {
    wl_display*   display    = nullptr;
    wl_registry*  registry   = nullptr;
    wl_compositor* compositor = nullptr;
    wl_seat*      seat        = nullptr;
    wl_keyboard*  keyboard    = nullptr;
    wl_pointer*   pointer     = nullptr;
    xdg_wm_base* xdg_wm_base_ptr = nullptr;
    wl_surface*  surface     = nullptr;
    xdg_surface* xdg_surface_ptr = nullptr;
    xdg_toplevel* toplevel   = nullptr;
    wl_egl_window* egl_window = nullptr;

    int  width        = 1280;
    int  height       = 800;
    bool configured   = false;
    bool needs_resize = false;

    // Text input
    std::string pending_text;
    uint32_t modifiers = 0;  // bit 2 = ctrl, bit 0 = shift, bit 3 = alt

    // Pointer
    float pointer_x = 0.f;
    float pointer_y = 0.f;
    bool  pointer_btn_left  = false;
    bool  pointer_btn_right = false;
};

// -- Wayland listener stubs (keyboard handled below) --

void xdg_wm_base_ping_cb(void* /*d*/, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_base_listener = { .ping = xdg_wm_base_ping_cb };

void xdg_surface_configure_cb(void* data, xdg_surface* surf, uint32_t serial) {
    xdg_surface_ack_configure(surf, serial);
    static_cast<WaylandState*>(data)->configured = true;
}
const xdg_surface_listener xdg_surf_listener = { .configure = xdg_surface_configure_cb };

void toplevel_configure_cb(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* ws = static_cast<WaylandState*>(data);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width = w; ws->height = h; ws->needs_resize = true;
    }
}
void toplevel_close_cb(void* /*d*/, xdg_toplevel*) {
    g_running.store(false, std::memory_order_relaxed);
}
void toplevel_configure_bounds_cb(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities_cb(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener toplevel_listener = {
    .configure        = toplevel_configure_cb,
    .close            = toplevel_close_cb,
    .configure_bounds = toplevel_configure_bounds_cb,
    .wm_capabilities  = toplevel_wm_capabilities_cb,
};

// Keyboard
void kb_keymap(void*, wl_keyboard*, uint32_t, int fd, uint32_t) { close(fd); }
void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void kb_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                  uint32_t /*latched*/, uint32_t /*locked*/, uint32_t) {
    static_cast<WaylandState*>(data)->modifiers = depressed;
}
void kb_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}
void kb_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state_val) {
    if (state_val != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    auto* ws = static_cast<WaylandState*>(data);
    const bool ctrl  = (ws->modifiers & 4) != 0;
    const bool shift = (ws->modifiers & 1) != 0;

    // Arrow keys and special keys are delivered via ImGui directly when
    // using a real xkbcommon setup; here we just route text input.
    (void)ctrl; (void)shift;

    switch (key) {
    case 28: ws->pending_text += '\n'; break;  // Enter
    case 14: ws->pending_text += '\b'; break;  // Backspace
    case 15: ws->pending_text += '\t'; break;  // Tab
    case  1: ws->pending_text += '\x1b'; break; // Esc
    default: break;
    }
}
const wl_keyboard_listener kb_listener = {
    .keymap      = kb_keymap,
    .enter       = kb_enter,
    .leave       = kb_leave,
    .key         = kb_key,
    .modifiers   = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

// Pointer
void ptr_enter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    auto* ws = static_cast<WaylandState*>(data);
    ws->pointer_x = static_cast<float>(wl_fixed_to_double(x));
    ws->pointer_y = static_cast<float>(wl_fixed_to_double(y));
}
void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void ptr_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto* ws = static_cast<WaylandState*>(data);
    ws->pointer_x = static_cast<float>(wl_fixed_to_double(x));
    ws->pointer_y = static_cast<float>(wl_fixed_to_double(y));
}
void ptr_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state_val) {
    auto* ws = static_cast<WaylandState*>(data);
    const bool pressed = (state_val == WL_POINTER_BUTTON_STATE_PRESSED);
    if (button == 0x110) ws->pointer_btn_left  = pressed;
    if (button == 0x111) ws->pointer_btn_right = pressed;
}
void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void ptr_frame(void*, wl_pointer*) {}
void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
void ptr_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void ptr_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener ptr_listener = {
    .enter         = ptr_enter,
    .leave         = ptr_leave,
    .motion        = ptr_motion,
    .button        = ptr_button,
    .axis          = ptr_axis,
    .frame         = ptr_frame,
    .axis_source   = ptr_axis_source,
    .axis_stop     = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* ws = static_cast<WaylandState*>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ws->keyboard) {
        ws->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ws->keyboard, &kb_listener, ws);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ws->pointer) {
        ws->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ws->pointer, &ptr_listener, ws);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = { .capabilities = seat_capabilities, .name = seat_name };

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t version) {
    auto* ws = static_cast<WaylandState*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        ws->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, std::min(version, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, ws);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(ws->seat, &seat_listener, ws);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

// ---------------------------------------------------------------------------
// Editor state
// ---------------------------------------------------------------------------

struct TabState {
    Buffer            buffer;
    std::string       title;          ///< Tab label (filename or "Untitled N")
    Position          cursor;
    int               scroll_line = 0;
    SyntaxHighlighter highlighter;
    bool              show_line_nums = true;
};

struct EditorState {
    std::vector<std::unique_ptr<TabState>> tabs;
    int active_tab = 0;

    // Find/replace bar
    bool          search_open    = false;
    bool          replace_open   = false;
    char          search_buf[512]  = {};
    char          replace_buf[512] = {};
    SearchOptions search_opts;
    SearchEngine  search_engine;
    std::vector<Match> search_matches;
    int           current_match   = -1;

    // UI state
    bool show_minimap     = true;
    bool show_status_bar  = true;
    int  untitled_counter = 1;

    // New-tab / open-file dialog
    bool open_dialog_open = false;
    char open_path_buf[1024] = {};
    bool save_dialog_open = false;
    char save_path_buf[1024] = {};
};

static constexpr float kLineNumWidth  = 52.f;
static constexpr float kMinimapWidth  = 90.f;
static constexpr float kStatusBarH    = 22.f;
static constexpr float kTabBarH       = 28.f;
static constexpr float kSearchBarH    = 30.f;

// Create a new empty tab
void new_tab(EditorState& ed) {
    auto tab = std::make_unique<TabState>();
    tab->title = "Untitled " + std::to_string(ed.untitled_counter++);
    ed.tabs.push_back(std::move(tab));
    ed.active_tab = static_cast<int>(ed.tabs.size()) - 1;
}

// Open a file into a new tab
void open_file(EditorState& ed, const std::filesystem::path& path) {
    auto result = Buffer::from_file(path);
    if (!result.has_value()) {
        SL_ERROR("Failed to open {}: {}", path.string(), result.error().message());
        return;
    }

    auto tab = std::make_unique<TabState>();
    tab->buffer = std::move(result).value();
    tab->title  = path.filename().string();
    tab->highlighter.set_language(SyntaxHighlighter::detect(path.filename().string()));
    ed.tabs.push_back(std::move(tab));
    ed.active_tab = static_cast<int>(ed.tabs.size()) - 1;
}

// ---------------------------------------------------------------------------
// Minimap rendering
// ---------------------------------------------------------------------------

void render_minimap(const TabState& tab, ImVec2 pos, ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                        IM_COL32(20, 20, 30, 220));

    const int total_lines = tab.buffer.line_count();
    if (total_lines == 0) return;

    // Render minimap as tiny coloured bars per line
    const float line_h = std::max(1.f, size.y / static_cast<float>(total_lines));

    for (int li = 0; li < total_lines; ++li) {
        const float y = pos.y + static_cast<float>(li) * line_h;
        if (y > pos.y + size.y) break;

        const auto sv = tab.buffer.line(li);
        if (sv.empty()) continue;

        // Colour based on indent depth — cheap but informative
        int indent = 0;
        for (char c : sv) {
            if (c == ' ')  ++indent;
            else if (c == '\t') indent += 4;
            else break;
        }
        const float depth = static_cast<float>(indent) * 0.1f;
        const uint8_t r = static_cast<uint8_t>(std::min(255.f, 40.f + depth * 80.f));
        const uint8_t g = static_cast<uint8_t>(std::min(255.f, 120.f + depth * 40.f));
        const uint8_t b = static_cast<uint8_t>(std::min(255.f, 180.f - depth * 20.f));

        const float bar_w = std::min(size.x - 4.f,
                                     static_cast<float>(sv.size()) * 0.6f);
        draw->AddRectFilled(
            ImVec2(pos.x + 2.f, y),
            ImVec2(pos.x + 2.f + bar_w, y + std::max(1.f, line_h - 0.5f)),
            IM_COL32(r, g, b, 180));
    }

    // Viewport indicator
    const float visible_lines = size.y / std::max(1.f, line_h * 14.f); // approx
    const float vp_y = pos.y + static_cast<float>(tab.scroll_line) * line_h;
    const float vp_h = visible_lines * line_h;
    draw->AddRect(ImVec2(pos.x, vp_y),
                  ImVec2(pos.x + size.x, vp_y + vp_h),
                  IM_COL32(200, 200, 200, 80));
}

// ---------------------------------------------------------------------------
// Code area rendering
// ---------------------------------------------------------------------------

void render_code_area(TabState& tab, float width, float height,
                       bool show_line_nums) {
    // We draw directly into the current ImGui window using AddText calls.
    ImDrawList* draw    = ImGui::GetWindowDrawList();
    ImFont*     font    = ImGui::GetFont();
    const float fsize   = ImGui::GetFontSize();
    const float line_h  = fsize + 2.f;

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    const float  gutter_w   = show_line_nums ? kLineNumWidth : 0.f;

    const int visible_lines = static_cast<int>(height / line_h) + 2;
    const int total_lines   = tab.buffer.line_count();

    // Clamp scroll
    tab.scroll_line = std::clamp(tab.scroll_line, 0,
                                  std::max(0, total_lines - visible_lines / 2));

    // Invisible full-size widget to capture scrolling
    ImGui::InvisibleButton("##code_area", ImVec2(width, height));
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        tab.scroll_line = std::max(0, tab.scroll_line - static_cast<int>(wheel * 3.f));
    }

    // Clip region
    draw->PushClipRect(canvas_pos, ImVec2(canvas_pos.x + width, canvas_pos.y + height), true);

    bool in_block_comment = false;
    // Precompute block comment state up to scroll_line
    for (int li = 0; li < tab.scroll_line && li < total_lines; ++li) {
        tab.highlighter.tokenise(tab.buffer.line(li), in_block_comment);
    }

    for (int li = tab.scroll_line; li < total_lines; ++li) {
        const float y = canvas_pos.y + static_cast<float>(li - tab.scroll_line) * line_h;
        if (y > canvas_pos.y + height) break;

        const bool is_cursor_line = (li == tab.cursor.line);

        // Cursor-line highlight
        if (is_cursor_line) {
            draw->AddRectFilled(
                ImVec2(canvas_pos.x, y),
                ImVec2(canvas_pos.x + width, y + line_h),
                IM_COL32(60, 60, 90, 120));
        }

        // Line number gutter
        if (show_line_nums) {
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), "%4d", li + 1);
            draw->AddText(font, fsize,
                          ImVec2(canvas_pos.x + 2.f, y),
                          IM_COL32(100, 100, 140, 255),
                          num_buf);
            // Gutter separator
            draw->AddLine(ImVec2(canvas_pos.x + gutter_w - 2.f, y),
                          ImVec2(canvas_pos.x + gutter_w - 2.f, y + line_h),
                          IM_COL32(60, 60, 90, 200));
        }

        // Tokenise and render the line with syntax colours
        const auto sv     = tab.buffer.line(li);
        const auto tokens = tab.highlighter.tokenise(sv, in_block_comment);
        const std::string line_str(sv);

        float char_w = ImGui::CalcTextSize("M").x; // monospace advance
        (void)char_w;

        // Render each token
        for (const auto& tok : tokens) {
            if (tok.col_start >= tok.col_end) continue;
            const int s = std::clamp(tok.col_start, 0, static_cast<int>(sv.size()));
            const int e = std::clamp(tok.col_end,   0, static_cast<int>(sv.size()));
            if (s >= e) continue;

            const std::string_view span = sv.substr(static_cast<size_t>(s),
                                                     static_cast<size_t>(e - s));
            const std::string span_str(span);

            // X position: measure all text before this token
            const std::string before = line_str.substr(0, static_cast<size_t>(s));
            const float x_off = ImGui::CalcTextSize(before.c_str()).x;

            const Color col = tab.highlighter.colour_for(tok.kind);
            const ImU32 imcol = IM_COL32(
                (col >> 24) & 0xFF,
                (col >> 16) & 0xFF,
                (col >>  8) & 0xFF,
                (col >>  0) & 0xFF);

            draw->AddText(font, fsize,
                          ImVec2(canvas_pos.x + gutter_w + x_off, y),
                          imcol,
                          span_str.c_str());
        }

        // Draw cursor caret
        if (is_cursor_line) {
            const int col = std::min(tab.cursor.col, static_cast<int>(sv.size()));
            const std::string before_cursor = line_str.substr(0, static_cast<size_t>(std::max(0, col)));
            const float cx = canvas_pos.x + gutter_w + ImGui::CalcTextSize(before_cursor.c_str()).x;
            draw->AddLine(ImVec2(cx, y), ImVec2(cx, y + line_h),
                          IM_COL32(220, 220, 220, 255), 2.f);
        }
    }

    draw->PopClipRect();
}

// ---------------------------------------------------------------------------
// Search bar
// ---------------------------------------------------------------------------

void render_search_bar(EditorState& ed) {
    if (!ed.search_open) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.20f, 1.f));
    ImGui::BeginChild("##search_bar", ImVec2(0, ed.replace_open ? kSearchBarH * 2.f + 8.f
                                                                 : kSearchBarH + 4.f),
                      false, ImGuiWindowFlags_NoScrollbar);

    bool rerun = false;

    ImGui::SetNextItemWidth(300.f);
    if (ImGui::InputText("##search_input", ed.search_buf, sizeof(ed.search_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        rerun = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Find Next")) rerun = true;
    ImGui::SameLine();
    if (ImGui::Button("Find Prev")) {
        if (!ed.tabs.empty()) {
            const auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
            ed.search_engine.set_query(ed.search_buf, ed.search_opts);
            auto m = ed.search_engine.find_prev(tab.buffer, tab.cursor);
            if (m) {
                auto& active = *ed.tabs[static_cast<size_t>(ed.active_tab)];
                active.cursor = m->start;
            }
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Regex", &ed.search_opts.use_regex);
    ImGui::SameLine();
    ImGui::Checkbox("Case", &ed.search_opts.case_sensitive);
    ImGui::SameLine();
    ImGui::Checkbox("Word", &ed.search_opts.whole_word);
    ImGui::SameLine();
    ImGui::Checkbox("Replace", &ed.replace_open);
    ImGui::SameLine();
    if (ImGui::Button("X")) { ed.search_open = false; ed.replace_open = false; }

    if (ed.replace_open) {
        ImGui::SetNextItemWidth(300.f);
        ImGui::InputText("##replace_input", ed.replace_buf, sizeof(ed.replace_buf));
        ImGui::SameLine();
        if (ImGui::Button("Replace One") && !ed.tabs.empty()) {
            if (ed.current_match >= 0 &&
                ed.current_match < static_cast<int>(ed.search_matches.size())) {
                auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
                ed.search_engine.replace_one(tab.buffer,
                    ed.search_matches[static_cast<size_t>(ed.current_match)],
                    ed.replace_buf);
                rerun = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace All") && !ed.tabs.empty()) {
            auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
            int n = ed.search_engine.replace_all(tab.buffer, ed.replace_buf);
            SL_INFO("Replaced {} occurrences", n);
            rerun = true;
        }
    }

    if (rerun && !ed.tabs.empty()) {
        const auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
        ed.search_engine.set_query(ed.search_buf, ed.search_opts);
        ed.search_matches = ed.search_engine.find_all(tab.buffer);
        ed.current_match  = 0;

        if (!ed.search_matches.empty()) {
            auto& active = *ed.tabs[static_cast<size_t>(ed.active_tab)];
            auto m = ed.search_engine.find_next(active.buffer, active.cursor);
            if (m) {
                active.cursor = m->start;
                // Find its index
                for (int mi = 0; mi < static_cast<int>(ed.search_matches.size()); ++mi) {
                    if (ed.search_matches[static_cast<size_t>(mi)] == *m) {
                        ed.current_match = mi;
                        break;
                    }
                }
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void render_status_bar(const EditorState& ed, float width) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.15f, 1.f));
    ImGui::BeginChild("##status_bar", ImVec2(width, kStatusBarH), false,
                       ImGuiWindowFlags_NoScrollbar);

    if (!ed.tabs.empty()) {
        const auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
        const Language lang = tab.highlighter.language();

        const char* lang_str = "Plain";
        switch (lang) {
        case Language::Cpp:      lang_str = "C++"; break;
        case Language::Python:   lang_str = "Python"; break;
        case Language::Rust:     lang_str = "Rust"; break;
        case Language::Json:     lang_str = "JSON"; break;
        case Language::Markdown: lang_str = "Markdown"; break;
        default: break;
        }

        const bool modified = tab.buffer.modified();
        ImGui::Text("  Ln %d, Col %d  |  %s  |  %s  |  %s  |  %d matches",
                    tab.cursor.line + 1,
                    tab.cursor.col  + 1,
                    lang_str,
                    modified ? "Modified" : "Saved",
                    tab.buffer.file_path().empty() ? "No file" :
                        tab.buffer.file_path().filename().string().c_str(),
                    static_cast<int>(ed.search_matches.size()));
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts for the editor (called each frame inside editor window)
// ---------------------------------------------------------------------------

void handle_editor_keys(EditorState& ed, WaylandState& ws) {
    ImGuiIO& io = ImGui::GetIO();
    if (ed.tabs.empty()) return;
    auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];

    const bool ctrl  = io.KeyCtrl;
    const bool shift = io.KeyShift;

    // File operations
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) { new_tab(ed); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) { ed.open_dialog_open = true; return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (!shift && !tab.buffer.file_path().empty()) {
            auto r = tab.buffer.save();
            if (!r.has_value()) SL_ERROR("Save failed: {}", r.error().message());
        } else {
            ed.save_dialog_open = true;
        }
        return;
    }

    // Undo/redo
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (!shift) tab.buffer.undo();
        else        tab.buffer.redo();
        return;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) { tab.buffer.redo(); return; }

    // Search
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) { ed.search_open = true; return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_H)) { ed.search_open = true; ed.replace_open = true; return; }

    // Tab close
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_W)) {
        if (ed.tabs.size() > 1) {
            ed.tabs.erase(ed.tabs.begin() + ed.active_tab);
            ed.active_tab = std::min(ed.active_tab,
                                     static_cast<int>(ed.tabs.size()) - 1);
        }
        return;
    }

    // Tab navigation
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        ed.active_tab = (ed.active_tab + 1) % static_cast<int>(ed.tabs.size());
        return;
    }

    // Minimap toggle
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_M)) { ed.show_minimap = !ed.show_minimap; return; }

    // Text navigation
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        tab.cursor.line = std::max(0, tab.cursor.line - 1);
        tab.cursor.col  = std::min(tab.cursor.col, tab.buffer.line_length(tab.cursor.line));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        tab.cursor.line = std::min(tab.buffer.line_count() - 1, tab.cursor.line + 1);
        tab.cursor.col  = std::min(tab.cursor.col, tab.buffer.line_length(tab.cursor.line));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (tab.cursor.col > 0) {
            --tab.cursor.col;
        } else if (tab.cursor.line > 0) {
            --tab.cursor.line;
            tab.cursor.col = tab.buffer.line_length(tab.cursor.line);
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        const int ll = tab.buffer.line_length(tab.cursor.line);
        if (tab.cursor.col < ll) {
            ++tab.cursor.col;
        } else if (tab.cursor.line < tab.buffer.line_count() - 1) {
            ++tab.cursor.line;
            tab.cursor.col = 0;
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) { tab.cursor.col = 0; return; }
    if (ImGui::IsKeyPressed(ImGuiKey_End))  {
        tab.cursor.col = tab.buffer.line_length(tab.cursor.line); return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        tab.cursor.line = std::max(0, tab.cursor.line - 20);
        tab.scroll_line = std::max(0, tab.scroll_line - 20);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        tab.cursor.line = std::min(tab.buffer.line_count() - 1, tab.cursor.line + 20);
        tab.scroll_line += 20;
        return;
    }

    // Backspace
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (tab.cursor.col > 0) {
            Position end   = tab.cursor;
            Position start = {tab.cursor.line, tab.cursor.col - 1};
            tab.buffer.erase(start, end);
            tab.cursor = start;
        } else if (tab.cursor.line > 0) {
            Position start = {tab.cursor.line - 1,
                               tab.buffer.line_length(tab.cursor.line - 1)};
            Position end   = {tab.cursor.line, 0};
            tab.buffer.erase(start, end);
            tab.cursor = start;
        }
        return;
    }

    // Delete key
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        const int ll = tab.buffer.line_length(tab.cursor.line);
        if (tab.cursor.col < ll) {
            Position start = tab.cursor;
            Position end   = {tab.cursor.line, tab.cursor.col + 1};
            tab.buffer.erase(start, end);
        } else if (tab.cursor.line < tab.buffer.line_count() - 1) {
            Position start = tab.cursor;
            Position end   = {tab.cursor.line + 1, 0};
            tab.buffer.erase(start, end);
        }
        return;
    }

    // Enter
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        // Auto-indent: detect leading whitespace of current line
        const auto cur_line = tab.buffer.line(tab.cursor.line);
        std::string indent;
        for (char c : cur_line) {
            if (c == ' ' || c == '\t') indent += c;
            else break;
        }
        tab.buffer.insert(tab.cursor, "\n" + indent);
        tab.cursor.line++;
        tab.cursor.col = static_cast<int>(indent.size());
        return;
    }

    // Tab key — insert 4 spaces
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        tab.buffer.insert(tab.cursor, "    ");
        tab.cursor.col += 4;
        return;
    }

    // Regular character input via ImGui input queue
    const ImWchar* chars = io.InputQueueCharacters.Data;
    for (int ci = 0; ci < io.InputQueueCharacters.Size; ++ci) {
        ImWchar wc = chars[ci];
        if (wc < 32 || wc == 127) continue;
        // Convert to UTF-8 (only ASCII handled here; full UTF-8 would use a proper encoder)
        if (wc < 128) {
            const char c = static_cast<char>(wc);
            tab.buffer.insert(tab.cursor, {&c, 1});
            ++tab.cursor.col;
        }
    }

    // Also handle pending text from Wayland keyboard
    for (char c : ws.pending_text) {
        if (c == '\n') {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        } else if (c == '\b') {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
        }
    }
    ws.pending_text.clear();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    Log::init("straylight-editor");
    SL_INFO("StrayLight Editor starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // Connect to Wayland
    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) {
        SL_CRITICAL("Cannot connect to Wayland display");
        return EXIT_FAILURE;
    }

    ws.registry = wl_display_get_registry(ws.display);
    wl_registry_add_listener(ws.registry, &registry_listener, &ws);
    wl_display_roundtrip(ws.display);

    if (!ws.compositor || !ws.xdg_wm_base_ptr) {
        SL_CRITICAL("Missing required Wayland globals");
        wl_display_disconnect(ws.display);
        return EXIT_FAILURE;
    }

    ws.surface = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &toplevel_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Editor");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-editor");
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // EGL
    auto egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major, minor;
    eglInitialize(egl_display, &major, &minor);

    constexpr EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg;
    EGLint num_cfg;
    eglChooseConfig(egl_display, cfg_attrs, &egl_cfg, 1, &num_cfg);

    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);
    auto egl_surface = eglCreateWindowSurface(egl_display, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);

    constexpr EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE
    };
    auto egl_ctx = eglCreateContext(egl_display, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_ctx);

    // ImGui
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width), static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Cyberpunk dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 2.f;
    style.FrameRounding  = 2.f;
    style.Colors[ImGuiCol_WindowBg]  = ImVec4(0.08f, 0.08f, 0.13f, 1.f);
    style.Colors[ImGuiCol_TitleBg]   = ImVec4(0.06f, 0.06f, 0.10f, 1.f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.06f, 0.20f, 1.f);
    style.Colors[ImGuiCol_Header]    = ImVec4(0.20f, 0.10f, 0.35f, 1.f);
    style.Colors[ImGuiCol_Button]    = ImVec4(0.20f, 0.10f, 0.35f, 1.f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.15f, 0.50f, 1.f);
    style.Colors[ImGuiCol_Tab]          = ImVec4(0.10f, 0.10f, 0.18f, 1.f);
    style.Colors[ImGuiCol_TabActive]    = ImVec4(0.20f, 0.10f, 0.35f, 1.f);
    style.Colors[ImGuiCol_TabHovered]   = ImVec4(0.25f, 0.12f, 0.42f, 1.f);

    // Editor state
    EditorState ed;

    // Open files from command-line arguments
    for (int ai = 1; ai < argc; ++ai) {
        open_file(ed, argv[ai]);
    }
    if (ed.tabs.empty()) {
        new_tab(ed);
    }

    // Main loop
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_flush(ws.display);
        wl_display_dispatch_pending(ws.display);
        wl_display_roundtrip(ws.display);

        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                                    static_cast<float>(ws.height));
        }

        // Feed pointer to ImGui
        io.MousePos = ImVec2(ws.pointer_x, ws.pointer_y);
        io.MouseDown[0] = ws.pointer_btn_left;
        io.MouseDown[1] = ws.pointer_btn_right;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // ---- Full-window layout ----
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

        ImGuiWindowFlags main_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar;

        ImGui::Begin("##editor_main", nullptr, main_flags);

        // ---- Menu bar ----
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New",     "Ctrl+N")) new_tab(ed);
                if (ImGui::MenuItem("Open…",   "Ctrl+O")) ed.open_dialog_open = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Save",    "Ctrl+S")) {
                    if (!ed.tabs.empty()) {
                        auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
                        if (!tab.buffer.file_path().empty()) {
                            tab.buffer.save();
                        } else {
                            ed.save_dialog_open = true;
                        }
                    }
                }
                if (ImGui::MenuItem("Save As…","Ctrl+Shift+S")) ed.save_dialog_open = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Close Tab","Ctrl+W") && ed.tabs.size() > 1) {
                    ed.tabs.erase(ed.tabs.begin() + ed.active_tab);
                    ed.active_tab = std::min(ed.active_tab,
                                             static_cast<int>(ed.tabs.size()) - 1);
                }
                if (ImGui::MenuItem("Quit")) g_running.store(false, std::memory_order_relaxed);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                    if (!ed.tabs.empty()) ed.tabs[static_cast<size_t>(ed.active_tab)]->buffer.undo();
                }
                if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                    if (!ed.tabs.empty()) ed.tabs[static_cast<size_t>(ed.active_tab)]->buffer.redo();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Find",         "Ctrl+F")) { ed.search_open = true; }
                if (ImGui::MenuItem("Find & Replace","Ctrl+H")) { ed.search_open = true; ed.replace_open = true; }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Minimap",    "Ctrl+M", &ed.show_minimap);
                ImGui::MenuItem("Status Bar",  nullptr,  &ed.show_status_bar);
                if (!ed.tabs.empty()) {
                    ImGui::MenuItem("Line Numbers", nullptr,
                                    &ed.tabs[static_cast<size_t>(ed.active_tab)]->show_line_nums);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Language")) {
                if (!ed.tabs.empty()) {
                    auto& hl = ed.tabs[static_cast<size_t>(ed.active_tab)]->highlighter;
                    if (ImGui::MenuItem("None"))     hl.set_language(Language::None);
                    if (ImGui::MenuItem("C++"))      hl.set_language(Language::Cpp);
                    if (ImGui::MenuItem("Python"))   hl.set_language(Language::Python);
                    if (ImGui::MenuItem("Rust"))     hl.set_language(Language::Rust);
                    if (ImGui::MenuItem("JSON"))     hl.set_language(Language::Json);
                    if (ImGui::MenuItem("Markdown")) hl.set_language(Language::Markdown);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y;

        // ---- Tab bar ----
        if (!ed.tabs.empty() && ImGui::BeginTabBar("##editor_tabs")) {
            for (int ti = 0; ti < static_cast<int>(ed.tabs.size()); ++ti) {
                auto& tab = *ed.tabs[static_cast<size_t>(ti)];
                std::string label = tab.title;
                if (tab.buffer.modified()) label += " *";
                label += "##tab" + std::to_string(ti);

                bool open = true;
                ImGuiTabItemFlags tab_flags = ImGuiTabItemFlags_None;
                if (ti == ed.active_tab) tab_flags |= ImGuiTabItemFlags_SetSelected;

                if (ImGui::BeginTabItem(label.c_str(), ed.tabs.size() > 1 ? &open : nullptr, tab_flags)) {
                    ed.active_tab = ti;
                    ImGui::EndTabItem();
                }
                if (!open && ed.tabs.size() > 1) {
                    ed.tabs.erase(ed.tabs.begin() + ti);
                    ed.active_tab = std::min(ed.active_tab, static_cast<int>(ed.tabs.size()) - 1);
                    break;
                }
            }
            ImGui::EndTabBar();
        }

        // ---- Search bar ----
        render_search_bar(ed);

        // ---- Compute remaining area ----
        const float code_h = avail_h - kTabBarH -
                             (ed.show_status_bar ? kStatusBarH : 0.f) -
                             (ed.search_open ? (ed.replace_open ? kSearchBarH * 2.f + 8.f
                                                                 : kSearchBarH + 4.f) : 0.f);
        const float minimap_w = (ed.show_minimap && !ed.tabs.empty()) ? kMinimapWidth : 0.f;
        const float code_w    = avail_w - minimap_w;

        if (!ed.tabs.empty()) {
            auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];

            // Code area
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::BeginChild("##code_region", ImVec2(code_w, code_h), false,
                              ImGuiWindowFlags_NoScrollbar);

            handle_editor_keys(ed, ws);
            render_code_area(tab, code_w, code_h, tab.show_line_nums);

            // Keep cursor visible
            {
                const float line_h = ImGui::GetFontSize() + 2.f;
                const int visible  = static_cast<int>(code_h / line_h);
                if (tab.cursor.line < tab.scroll_line)
                    tab.scroll_line = tab.cursor.line;
                else if (tab.cursor.line >= tab.scroll_line + visible - 2)
                    tab.scroll_line = tab.cursor.line - visible + 3;
                tab.scroll_line = std::max(0, tab.scroll_line);
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();

            // Minimap
            if (ed.show_minimap) {
                ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
                ImGui::BeginChild("##minimap", ImVec2(minimap_w, code_h), false,
                                  ImGuiWindowFlags_NoScrollbar);
                const ImVec2 mm_pos = ImGui::GetCursorScreenPos();
                render_minimap(tab, mm_pos, ImVec2(minimap_w, code_h));
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
        }

        // ---- Status bar ----
        if (ed.show_status_bar) {
            render_status_bar(ed, avail_w);
        }

        // ---- Open-file dialog ----
        if (ed.open_dialog_open) {
            ImGui::OpenPopup("Open File");
            ed.open_dialog_open = false;
        }
        if (ImGui::BeginPopupModal("Open File", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Path:");
            ImGui::SetNextItemWidth(500.f);
            ImGui::InputText("##open_path", ed.open_path_buf, sizeof(ed.open_path_buf));
            if (ImGui::Button("Open")) {
                open_file(ed, ed.open_path_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ---- Save-as dialog ----
        if (ed.save_dialog_open) {
            ImGui::OpenPopup("Save As");
            ed.save_dialog_open = false;
        }
        if (ImGui::BeginPopupModal("Save As", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Save path:");
            ImGui::SetNextItemWidth(500.f);
            ImGui::InputText("##save_path", ed.save_path_buf, sizeof(ed.save_path_buf));
            if (ImGui::Button("Save") && !ed.tabs.empty()) {
                auto& tab = *ed.tabs[static_cast<size_t>(ed.active_tab)];
                auto r = tab.buffer.save(ed.save_path_buf);
                if (r.has_value()) {
                    tab.title = std::filesystem::path(ed.save_path_buf).filename().string();
                } else {
                    SL_ERROR("Save failed: {}", r.error().message());
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::End();
        ImGui::PopStyleVar(2);

        // Render
        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.08f, 0.08f, 0.13f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_display, egl_surface);
    eglDestroyContext(egl_display, egl_ctx);
    eglTerminate(egl_display);

    wl_egl_window_destroy(ws.egl_window);
    xdg_toplevel_destroy(ws.toplevel);
    xdg_surface_destroy(ws.xdg_surface_ptr);
    wl_surface_destroy(ws.surface);
    if (ws.keyboard) wl_keyboard_destroy(ws.keyboard);
    if (ws.pointer)  wl_pointer_destroy(ws.pointer);
    if (ws.seat)     wl_seat_destroy(ws.seat);
    xdg_wm_base_destroy(ws.xdg_wm_base_ptr);
    wl_compositor_destroy(ws.compositor);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);

    SL_INFO("StrayLight Editor exited cleanly");
    return EXIT_SUCCESS;
}
