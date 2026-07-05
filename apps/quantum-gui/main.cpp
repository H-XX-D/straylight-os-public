// apps/quantum-gui/main.cpp
// StrayLight Quantum — Wayland + EGL + ImGui application
#include "quantum_panel.h"

#include <straylight/log.h>
#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <xdg-shell-client-protocol.h>

#include <atomic>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

struct WaylandState {
    wl_display*    display          = nullptr;
    wl_registry*   registry         = nullptr;
    wl_compositor* compositor       = nullptr;
    wl_seat*       seat             = nullptr;
    xdg_wm_base*   xdg_wm_base_ptr = nullptr;
    wl_surface*    surface          = nullptr;
    xdg_surface*   xdg_surface_ptr  = nullptr;
    xdg_toplevel*  toplevel         = nullptr;
    wl_egl_window* egl_window       = nullptr;
    int  width        = 1280;
    int  height       = 800;
    bool configured   = false;
    bool needs_resize = false;
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version);
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_global_remove };

void xdg_wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener wm_base_listener = { .ping = xdg_wm_base_ping };

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* ws = static_cast<WaylandState*>(data);
    xdg_surface_ack_configure(surface, serial);
    ws->configured = true;
}
const xdg_surface_listener xdg_surf_listener = { .configure = xdg_surface_configure };

void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* ws = static_cast<WaylandState*>(data);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width = w; ws->height = h; ws->needs_resize = true;
    }
}
void toplevel_close(void*, xdg_toplevel*) { g_running.store(false, std::memory_order_relaxed); }
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener tl_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds, .wm_capabilities = toplevel_wm_capabilities
};

void seat_capabilities(void*, wl_seat*, uint32_t) {}
void seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener_impl = { .capabilities = seat_capabilities, .name = seat_name };

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
    auto* ws = static_cast<WaylandState*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        ws->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 2));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, nullptr);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 7));
        wl_seat_add_listener(ws->seat, &seat_listener_impl, nullptr);
    }
}

} // namespace

int main() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) { SL_CRITICAL("Cannot connect to Wayland display"); return 1; }

    ws.registry = wl_display_get_registry(ws.display);
    wl_registry_add_listener(ws.registry, &registry_listener, &ws);
    wl_display_roundtrip(ws.display);

    ws.surface = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Quantum");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight.quantum-gui");
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);

    EGLDisplay egl_dpy = eglGetDisplay(static_cast<EGLNativeDisplayType>(ws.display));
    eglInitialize(egl_dpy, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig egl_cfg; EGLint num_cfg;
    eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &num_cfg);

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    EGLSurface egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width), static_cast<float>(ws.height));

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.0f;
    style.FrameRounding     = 8.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 8.0f;
    style.WindowPadding     = ImVec2(16.0f, 16.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ScrollbarSize     = 10.0f;
    ImGui::StyleColorsDark();
    auto& c = style.Colors;
    c[ImGuiCol_WindowBg]         = {0.024f, 0.035f, 0.078f, 1.0f};
    c[ImGuiCol_ChildBg]          = {0.035f, 0.055f, 0.110f, 0.80f};
    c[ImGuiCol_FrameBg]          = {0.050f, 0.075f, 0.150f, 1.0f};
    c[ImGuiCol_FrameBgHovered]   = {0.070f, 0.100f, 0.200f, 1.0f};
    c[ImGuiCol_FrameBgActive]    = {0.090f, 0.130f, 0.250f, 1.0f};
    c[ImGuiCol_TitleBg]          = {0.024f, 0.035f, 0.078f, 1.0f};
    c[ImGuiCol_TitleBgActive]    = {0.035f, 0.055f, 0.110f, 1.0f};
    c[ImGuiCol_Tab]              = {0.040f, 0.065f, 0.130f, 1.0f};
    c[ImGuiCol_TabHovered]       = {0.098f, 0.906f, 1.000f, 0.25f};
    c[ImGuiCol_TabActive]        = {0.098f, 0.906f, 1.000f, 0.15f};
#if IMGUI_VERSION_NUM >= 19100
    c[ImGuiCol_TabSelectedOverline] = {0.098f, 0.906f, 1.000f, 1.0f};
#endif
    c[ImGuiCol_Header]           = {0.098f, 0.906f, 1.000f, 0.10f};
    c[ImGuiCol_HeaderHovered]    = {0.098f, 0.906f, 1.000f, 0.18f};
    c[ImGuiCol_HeaderActive]     = {0.098f, 0.906f, 1.000f, 0.25f};
    c[ImGuiCol_Button]           = {0.098f, 0.906f, 1.000f, 0.12f};
    c[ImGuiCol_ButtonHovered]    = {0.098f, 0.906f, 1.000f, 0.22f};
    c[ImGuiCol_ButtonActive]     = {0.098f, 0.906f, 1.000f, 0.35f};
    c[ImGuiCol_CheckMark]        = {0.098f, 0.906f, 1.000f, 1.0f};
    c[ImGuiCol_SliderGrab]       = {0.098f, 0.906f, 1.000f, 0.80f};
    c[ImGuiCol_SliderGrabActive] = {0.098f, 0.906f, 1.000f, 1.0f};
    c[ImGuiCol_Separator]        = {1.0f, 1.0f, 1.0f, 0.08f};
    c[ImGuiCol_PlotLines]        = {0.098f, 0.906f, 1.000f, 1.0f};
    c[ImGuiCol_PlotHistogram]    = {0.545f, 0.361f, 0.965f, 1.0f};
    c[ImGuiCol_ScrollbarBg]      = {0.024f, 0.035f, 0.078f, 1.0f};
    c[ImGuiCol_ScrollbarGrab]    = {1.0f, 1.0f, 1.0f, 0.12f};

    ImGui_ImplOpenGL3_Init("#version 300 es");

    straylight::quantum::QuantumPanel panel;
    panel.init();

    while (g_running.load()) {
        wl_display_dispatch_pending(ws.display);
        if (wl_display_flush(ws.display) < 0) break;

        if (ws.needs_resize) {
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width), static_cast<float>(ws.height));
            ws.needs_resize = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("StrayLight Quantum", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        panel.render();

        ImGui::End();
        ImGui::Render();

        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.024f, 0.035f, 0.078f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_dpy, egl_surf);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_dpy, egl_surf);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
    if (ws.egl_window)      wl_egl_window_destroy(ws.egl_window);
    if (ws.toplevel)        xdg_toplevel_destroy(ws.toplevel);
    if (ws.xdg_surface_ptr) xdg_surface_destroy(ws.xdg_surface_ptr);
    if (ws.surface)         wl_surface_destroy(ws.surface);
    if (ws.xdg_wm_base_ptr) xdg_wm_base_destroy(ws.xdg_wm_base_ptr);
    if (ws.registry)        wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);
    return 0;
}
