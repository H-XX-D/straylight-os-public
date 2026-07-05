// apps/backup/main.cpp
// StrayLight Backup — Wayland xdg-toplevel + EGL + OpenGL ES + ImGui
#include "ui.h"

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
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Wayland state
// ---------------------------------------------------------------------------

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

    int  width        = 900;
    int  height       = 680;
    bool configured   = false;
    bool needs_resize = false;
};

// Forward declaration
void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* interface, uint32_t version);
void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

void xdg_wm_base_ping_cb(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_base_listener = { .ping = xdg_wm_base_ping_cb };

void xdg_surface_configure_cb(void* data, xdg_surface* surf, uint32_t serial) {
    auto* ws = static_cast<WaylandState*>(data);
    xdg_surface_ack_configure(surf, serial);
    ws->configured = true;
}
const xdg_surface_listener xdg_surf_listener = { .configure = xdg_surface_configure_cb };

void toplevel_configure_cb(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* ws = static_cast<WaylandState*>(data);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width = w; ws->height = h; ws->needs_resize = true;
    }
}
void toplevel_close_cb(void*, xdg_toplevel*) {
    g_running.store(false, std::memory_order_relaxed);
}
void toplevel_configure_bounds_cb(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities_cb(void*, xdg_toplevel*, wl_array*) {}

const xdg_toplevel_listener tl_listener = {
    .configure        = toplevel_configure_cb,
    .close            = toplevel_close_cb,
    .configure_bounds = toplevel_configure_bounds_cb,
    .wm_capabilities  = toplevel_wm_capabilities_cb,
};

void seat_capabilities_cb(void*, wl_seat*, uint32_t) {}
void seat_name_cb(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener_impl = {
    .capabilities = seat_capabilities_cb,
    .name         = seat_name_cb,
};

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* interface, uint32_t version) {
    auto* ws = static_cast<WaylandState*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        ws->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, std::min(version, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, ws);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(ws->seat, &seat_listener_impl, ws);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    using namespace straylight;
    using namespace straylight::backup;

    Log::init("straylight-backup");
    SL_INFO("StrayLight Backup starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // --- Wayland ----------------------------------------------------------
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

    ws.surface          = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr  = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Backup");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-backup");
    xdg_toplevel_set_min_size(ws.toplevel, 800, 600);
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // --- EGL --------------------------------------------------------------
    EGLDisplay egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major = 0, minor = 0;
    eglInitialize(egl_display, &major, &minor);

    constexpr EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig egl_config  = nullptr;
    EGLint    num_configs = 0;
    eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs);

    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);
    EGLSurface egl_surface = eglCreateWindowSurface(
        egl_display, egl_config,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);

    constexpr EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };
    EGLContext egl_context = eglCreateContext(egl_display, egl_config,
                                               EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);

    // --- ImGui ------------------------------------------------------------
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize  = ImVec2(float(ws.width), float(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // --- App --------------------------------------------------------------
    BackupApp app;
    app.apply_style();
    app.run(argc, argv);

    // Start scheduler in background
    // (Engine is owned by BackupApp; we pass a reference to the internal engine
    //  via the scheduler's start() which takes an Engine& — we re-expose it
    //  by starting it inside BackupApp after construction.)

    static const char* kTabs[] = { "Profiles", "Schedule", "History" };
    int active_tab = 0;

    SL_INFO("Backup UI ready");

    // --- Main loop --------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(ws.display);
        wl_display_flush(ws.display);

        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(float(ws.width), float(ws.height));
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);

        constexpr ImGuiWindowFlags kWinFlags =
            ImGuiWindowFlags_NoTitleBar  |
            ImGuiWindowFlags_NoResize    |
            ImGuiWindowFlags_NoMove      |
            ImGuiWindowFlags_NoCollapse  |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin("##Backup", nullptr, kWinFlags)) {
            // Title bar
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
            ImGui::Text("STRAYLIGHT BACKUP");
            ImGui::PopStyleColor();
            ImGui::SameLine(io.DisplaySize.x - 60.0f);
            if (ImGui::SmallButton("Close"))
                g_running.store(false, std::memory_order_relaxed);
            ImGui::Separator();
            ImGui::Spacing();

            // Tab bar
            for (int i = 0; i < 3; ++i) {
                if (i > 0) ImGui::SameLine();
                bool sel = (active_tab == i);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button,
                                               ImVec4(0.0f, 0.55f, 0.38f, 1.0f));
                if (ImGui::Button(kTabs[i], ImVec2(110.0f, 0.0f))) active_tab = i;
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::Spacing();

            switch (active_tab) {
                case 0: app.render_profiles(); break;
                case 1: app.render_schedule(); break;
                case 2: app.render_history();  break;
                default: break;
            }
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.08f, 0.08f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);

        usleep(16000); // ~60 fps
    }

    // --- Cleanup ----------------------------------------------------------
    SL_INFO("Backup shutting down");
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

    SL_INFO("Backup exited cleanly");
    return EXIT_SUCCESS;
}
