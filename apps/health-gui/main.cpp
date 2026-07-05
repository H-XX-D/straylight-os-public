// StrayLight Health — Wayland + EGL + ImGui application
#include "health_panel.h"

#include <straylight/log.h>
#include <straylight/imgui_theme.h>

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
    int  width        = 960;
    int  height       = 680;
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
        ws->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, ws);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(ws->seat, &seat_listener_impl, ws);
    }
}

void apply_straylight_theme() {
    straylight::ui::apply_straylight_theme();
}

} // anonymous namespace

int main(int, char*[]) {
    using namespace straylight;
    Log::init("straylight-health");
    SL_INFO("StrayLight Health starting");
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) { SL_CRITICAL("Failed to connect to Wayland display"); return EXIT_FAILURE; }
    ws.registry = wl_display_get_registry(ws.display);
    wl_registry_add_listener(ws.registry, &registry_listener, &ws);
    wl_display_roundtrip(ws.display);
    if (!ws.compositor || !ws.xdg_wm_base_ptr) {
        SL_CRITICAL("Missing Wayland globals"); wl_display_disconnect(ws.display); return EXIT_FAILURE;
    }
    ws.surface = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Health");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-sl-health");
    xdg_toplevel_set_min_size(ws.toplevel, 800, 560);
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    EGLDisplay egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major = 0, minor = 0;
    eglInitialize(egl_display, &major, &minor);
    constexpr EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE
    };
    EGLConfig egl_config = nullptr; EGLint num_configs = 0;
    eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs);
    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);
    EGLSurface egl_surface = eglCreateWindowSurface(egl_display, egl_config,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);
    constexpr EGLint ctx_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width), static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");
    apply_straylight_theme();

    straylight::health::HealthState health_state; health_state.init();

    SL_INFO("StrayLight Health initialized");

    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(ws.display);
        wl_display_flush(ws.display);
        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width), static_cast<float>(ws.height));
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        constexpr ImGuiWindowFlags kWinFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##Main", nullptr, kWinFlags)) {
            straylight::health::render_health_panel(health_state);
        }
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.08f, 0.08f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);
        usleep(16000);
    }

    SL_INFO("StrayLight Health shutting down");
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_display, egl_surface);
    eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);
    wl_egl_window_destroy(ws.egl_window);
    xdg_toplevel_destroy(ws.toplevel);
    xdg_surface_destroy(ws.xdg_surface_ptr);
    wl_surface_destroy(ws.surface);
    if (ws.seat) wl_seat_destroy(ws.seat);
    xdg_wm_base_destroy(ws.xdg_wm_base_ptr);
    wl_compositor_destroy(ws.compositor);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);
    SL_INFO("StrayLight Health exited cleanly");
    return EXIT_SUCCESS;
}
