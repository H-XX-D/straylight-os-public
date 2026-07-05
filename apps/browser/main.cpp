// apps/browser/main.cpp
// StrayLight Browser — Wayland + EGL + ImGui + WebKitGTK
#include "engine.h"
#include "tab_manager.h"
#include "downloads.h"

#include <straylight/log.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <xdg-shell-client-protocol.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Wayland state
// ---------------------------------------------------------------------------

struct WaylandState {
    wl_display*    display       = nullptr;
    wl_registry*   registry      = nullptr;
    wl_compositor* compositor    = nullptr;
    wl_seat*       seat          = nullptr;
    wl_keyboard*   keyboard      = nullptr;
    wl_pointer*    pointer       = nullptr;
    xdg_wm_base*   xdg_wm_base_ptr = nullptr;
    wl_surface*    surface       = nullptr;
    xdg_surface*   xdg_surface_ptr = nullptr;
    xdg_toplevel*  toplevel      = nullptr;
    wl_egl_window* egl_window    = nullptr;

    int  width         = 1280;
    int  height        = 720;
    bool configured    = false;
    bool needs_resize  = false;

    uint32_t modifiers = 0;

    // ImGui input state
    float  mouse_x = 0, mouse_y = 0;
    bool   mouse_lmb = false, mouse_rmb = false;
    float  scroll_delta = 0;
};

// ---------------------------------------------------------------------------
// XDG-shell listeners
// ---------------------------------------------------------------------------

void xdg_wm_base_ping(void* /*d*/, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_base_listener = { .ping = xdg_wm_base_ping };

void xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial) {
    auto* ws = static_cast<WaylandState*>(data);
    xdg_surface_ack_configure(surf, serial);
    ws->configured = true;
}
const xdg_surface_listener xdg_surf_listener = {
    .configure = xdg_surface_configure
};

void toplevel_configure(void* data, xdg_toplevel* /*tl*/,
                        int32_t w, int32_t h, wl_array* /*states*/) {
    auto* ws = static_cast<WaylandState*>(data);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width = w; ws->height = h; ws->needs_resize = true;
    }
}
void toplevel_close(void* /*d*/, xdg_toplevel* /*tl*/) {
    g_running.store(false, std::memory_order_relaxed);
}
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener toplevel_listener = {
    .configure        = toplevel_configure,
    .close            = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities  = toplevel_wm_capabilities,
};

// ---------------------------------------------------------------------------
// Keyboard listener
// ---------------------------------------------------------------------------

void keyboard_keymap(void*, wl_keyboard*, uint32_t, int fd, uint32_t) { close(fd); }
void keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
void keyboard_key(void* /*data*/, wl_keyboard*, uint32_t, uint32_t,
                  uint32_t key, uint32_t state_val) {
    // Key translation is handled by ImGui's native backend via XKB in a
    // complete integration.  Here we map a minimal set.
    if (state_val != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    (void)key;
}
void keyboard_modifiers(void* data, wl_keyboard*, uint32_t,
                        uint32_t mods_dep, uint32_t, uint32_t, uint32_t) {
    static_cast<WaylandState*>(data)->modifiers = mods_dep;
}
void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// ---------------------------------------------------------------------------
// Pointer listener
// ---------------------------------------------------------------------------

void pointer_enter(void*, wl_pointer*, uint32_t, wl_surface*,
                   wl_fixed_t, wl_fixed_t) {}
void pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void pointer_motion(void* data, wl_pointer*, uint32_t,
                    wl_fixed_t sx, wl_fixed_t sy) {
    auto* ws = static_cast<WaylandState*>(data);
    ws->mouse_x = static_cast<float>(wl_fixed_to_double(sx));
    ws->mouse_y = static_cast<float>(wl_fixed_to_double(sy));
}
void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t,
                    uint32_t button, uint32_t state_val) {
    auto* ws = static_cast<WaylandState*>(data);
    bool pressed = (state_val == WL_POINTER_BUTTON_STATE_PRESSED);
    if (button == 0x110) ws->mouse_lmb = pressed;
    if (button == 0x111) ws->mouse_rmb = pressed;
}
void pointer_axis(void* data, wl_pointer*, uint32_t,
                  uint32_t /*axis*/, wl_fixed_t value) {
    auto* ws = static_cast<WaylandState*>(data);
    ws->scroll_delta -= static_cast<float>(wl_fixed_to_double(value)) / 10.0f;
}
void pointer_frame(void*, wl_pointer*) {}
void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener pointer_listener = {
    .enter         = pointer_enter,
    .leave         = pointer_leave,
    .motion        = pointer_motion,
    .button        = pointer_button,
    .axis          = pointer_axis,
    .frame         = pointer_frame,
    .axis_source   = pointer_axis_source,
    .axis_stop     = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

// ---------------------------------------------------------------------------
// Seat listener
// ---------------------------------------------------------------------------

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* ws = static_cast<WaylandState*>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ws->keyboard) {
        ws->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ws->keyboard, &keyboard_listener, ws);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ws->pointer) {
        ws->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ws->pointer, &pointer_listener, ws);
    }
}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

// ---------------------------------------------------------------------------
// Registry listener
// ---------------------------------------------------------------------------

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* iface, uint32_t version) {
    auto* ws = static_cast<WaylandState*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        ws->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min(version, 4u)));
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface,
                             std::min(version, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, nullptr);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface,
                             std::min(version, 5u)));
        wl_seat_add_listener(ws->seat, &seat_listener, ws);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::browser;

    Log::init("straylight-browser");
    SL_INFO("StrayLight Browser starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // ---- Wayland ----
    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) {
        SL_CRITICAL("Failed to connect to Wayland display");
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

    ws.surface        = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);

    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &toplevel_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Browser");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-browser");

    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // ---- EGL ----
    auto egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major = 0, minor = 0;
    eglInitialize(egl_display, &major, &minor);
    eglBindAPI(EGL_OPENGL_ES_API);

    constexpr EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8,  EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,  EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg; EGLint n_cfg = 0;
    eglChooseConfig(egl_display, cfg_attribs, &egl_cfg, 1, &n_cfg);

    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);
    auto egl_surf = eglCreateWindowSurface(
        egl_display, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);

    constexpr EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };
    auto egl_ctx = eglCreateContext(egl_display, egl_cfg,
                                    EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(egl_display, egl_surf, egl_surf, egl_ctx);

    // ---- ImGui ----
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                            static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    // Cyberpunk dark palette
    style.Colors[ImGuiCol_WindowBg]  = ImVec4(0.07f, 0.07f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_Header]    = ImVec4(0.20f, 0.10f, 0.40f, 1.0f);
    style.Colors[ImGuiCol_Button]    = ImVec4(0.15f, 0.15f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_FrameBg]   = ImVec4(0.12f, 0.12f, 0.20f, 1.0f);

    // ---- Browser state ----
    TabManager tabs;
    Downloads  downloads;
    downloads.load_history();

    char url_buf[2048] = "https://duckduckgo.com";
    bool show_downloads = false;
    bool url_bar_focused = false;

    {
        auto r = tabs.new_tab(url_buf, ws.width, ws.height - 60);
        if (!r.has_value()) {
            SL_ERROR("Failed to create initial tab: {}", r.error().message());
        }
    }

    SL_INFO("Browser initialised — entering frame loop");

    while (g_running.load(std::memory_order_relaxed)) {
        // ---- Event polling ----
        struct pollfd fds[1];
        fds[0].fd     = wl_display_get_fd(ws.display);
        fds[0].events = POLLIN;
        wl_display_flush(ws.display);
        poll(fds, 1, 16);

        if (fds[0].revents & POLLIN)
            wl_display_dispatch(ws.display);
        else
            wl_display_dispatch_pending(ws.display);

        // ---- Resize ----
        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                                    static_cast<float>(ws.height));
        }

        // ---- Feed mouse state into ImGui ----
        io.MousePos    = ImVec2(ws.mouse_x, ws.mouse_y);
        io.MouseDown[0] = ws.mouse_lmb;
        io.MouseDown[1] = ws.mouse_rmb;
        io.MouseWheel  += ws.scroll_delta;
        ws.scroll_delta = 0.0f;

        // ---- ImGui frame ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        ImGui::Begin("##BrowserRoot", nullptr,
                     ImGuiWindowFlags_NoTitleBar   |
                     ImGuiWindowFlags_NoResize     |
                     ImGuiWindowFlags_NoMove       |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Global keyboard shortcuts
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_T))
            tabs.new_tab("", ws.width, ws.height - 60);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W) && !tabs.empty())
            tabs.close_tab(tabs.active_index());
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L))
            url_bar_focused = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q))
            g_running.store(false, std::memory_order_relaxed);

        // Tab bar
        tabs.draw_tab_bar();

        if (!tabs.empty()) {
            Tab& tab  = tabs.active_tab();
            auto info = tab.engine->page_info();

            // ---- Navigation bar ----
            ImGui::BeginChild("##NavBar", ImVec2(0.0f, 40.0f),
                              false, ImGuiWindowFlags_NoScrollbar);

            if (ImGui::Button("<##back")) {
                if (info.can_go_back) tab.engine->go_back();
            }
            ImGui::SameLine();
            if (ImGui::Button(">##fwd")) {
                if (info.can_go_forward) tab.engine->go_forward();
            }
            ImGui::SameLine();
            if (ImGui::Button(info.is_loading ? "X##stop" : "R##reload")) {
                if (info.is_loading) tab.engine->stop();
                else                 tab.engine->reload();
            }
            ImGui::SameLine();

            // Sync URL bar with current page URL (unless user is typing)
            if (!url_bar_focused && !info.url.empty()) {
                auto n = std::min(info.url.size(), sizeof(url_buf) - 1);
                std::memcpy(url_buf, info.url.data(), n);
                url_buf[n] = '\0';
            }

            float url_w = ImGui::GetContentRegionAvail().x - 110.0f;
            ImGui::SetNextItemWidth(url_w);
            ImGuiInputTextFlags url_flags = ImGuiInputTextFlags_EnterReturnsTrue;
            if (url_bar_focused) { ImGui::SetKeyboardFocusHere(); url_bar_focused = false; }
            if (ImGui::InputText("##url", url_buf, sizeof(url_buf), url_flags)) {
                tab.engine->navigate(url_buf);
            }
            ImGui::SameLine();
            if (ImGui::Button(show_downloads ? "DL*" : "DL"))
                show_downloads = !show_downloads;

            // Loading progress stripe
            if (info.is_loading && info.load_progress > 0.0f) {
                ImGui::ProgressBar(info.load_progress,
                                   ImVec2(-1.0f, 3.0f), "");
            }

            ImGui::EndChild();

            // ---- Web content ----
            uint32_t tex  = tab.engine->render_to_texture();
            ImVec2 avail  = ImGui::GetContentRegionAvail();
            if (show_downloads) avail.x -= 310.0f;

            if (tex) {
                ImGui::Image(
                    reinterpret_cast<ImTextureID>(
                        static_cast<uintptr_t>(tex)),
                    avail);

                // Forward mouse to WebKit when hovering over web content
                if (ImGui::IsItemHovered()) {
                    ImVec2 item_pos = ImGui::GetItemRectMin();
                    tab.engine->inject_mouse(
                        ws.mouse_x - item_pos.x,
                        ws.mouse_y - item_pos.y,
                        ws.mouse_lmb, ws.mouse_rmb,
                        ws.scroll_delta);
                }
            } else {
                ImGui::Dummy(avail);
                ImGui::SetCursorPos({avail.x * 0.5f - 60.0f,
                                     avail.y * 0.5f - 8.0f});
                ImGui::TextDisabled("Loading WebKit...");
            }

            // Update tab metadata
            tab.title = info.title.empty()
                        ? (info.url.empty() ? "New Tab" : info.url)
                        : info.title;

            // ---- Downloads sidebar ----
            if (show_downloads) {
                ImGui::SameLine();
                ImGui::BeginChild("##DLPanel", ImVec2(300.0f, 0.0f), true);
                downloads.draw_panel();
                ImGui::EndChild();
            }
        } else {
            // Empty state
            ImVec2 centre = {io.DisplaySize.x * 0.5f - 80.0f,
                             io.DisplaySize.y * 0.5f - 8.0f};
            ImGui::SetCursorPos(centre);
            ImGui::TextDisabled("Press Ctrl+T to open a new tab");
        }

        ImGui::End();
        ImGui::PopStyleVar();

        // ---- Render ----
        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.07f, 0.07f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surf);
    }

    // ---- Cleanup ----
    downloads.save_history();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_display, egl_surf);
    eglDestroyContext(egl_display, egl_ctx);
    eglTerminate(egl_display);

    wl_egl_window_destroy(ws.egl_window);
    xdg_toplevel_destroy(ws.toplevel);
    xdg_surface_destroy(ws.xdg_surface_ptr);
    wl_surface_destroy(ws.surface);
    if (ws.pointer)  wl_pointer_destroy(ws.pointer);
    if (ws.keyboard) wl_keyboard_destroy(ws.keyboard);
    if (ws.seat)     wl_seat_destroy(ws.seat);
    xdg_wm_base_destroy(ws.xdg_wm_base_ptr);
    wl_compositor_destroy(ws.compositor);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);

    SL_INFO("Browser exited cleanly");
    return EXIT_SUCCESS;
}
