// apps/settings/main.cpp
// StrayLight Settings — sidebar-navigation ImGui settings application
// Wayland xdg-toplevel + EGL + OpenGL ES 3.0
#include "settings_page.h"
#include "pages/general.h"
#include "pages/display.h"
#include "pages/audio.h"
#include "pages/users.h"
#include "pages/about.h"
#include "pages/appearance.h"
#include "pages/network.h"
#include "pages/input.h"

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
#include <memory>
#include <vector>

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

    int  width        = 960;
    int  height       = 680;
    bool configured   = false;
    bool needs_resize = false;
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version);
void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

void xdg_wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* ws = static_cast<WaylandState*>(data);
    xdg_surface_ack_configure(surface, serial);
    ws->configured = true;
}

const xdg_surface_listener xdg_surf_listener = {
    .configure = xdg_surface_configure,
};

void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h,
                        wl_array*) {
    auto* ws = static_cast<WaylandState*>(data);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width        = w;
        ws->height       = h;
        ws->needs_resize = true;
    }
}

void toplevel_close(void*, xdg_toplevel*) {
    g_running.store(false, std::memory_order_relaxed);
}

void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

const xdg_toplevel_listener tl_listener = {
    .configure        = toplevel_configure,
    .close            = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities  = toplevel_wm_capabilities,
};

void seat_capabilities(void*, wl_seat*, uint32_t) {}
void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener seat_listener_impl = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
    auto* ws = static_cast<WaylandState*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        ws->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min(version, 4u)));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface,
                             std::min(version, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, ws);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        ws->seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface,
                             std::min(version, 5u)));
        wl_seat_add_listener(ws->seat, &seat_listener_impl, ws);
    }
}

// ---------------------------------------------------------------------------
// Settings app state
// ---------------------------------------------------------------------------

/// Apply a consistent StrayLight dark theme for the settings app.
void apply_settings_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = 0.0f;
    s.FrameRounding   = 3.0f;
    s.ItemSpacing     = ImVec2(8.0f, 6.0f);
    s.FramePadding    = ImVec2(6.0f, 4.0f);
    s.WindowPadding   = ImVec2(12.0f, 12.0f);
    s.IndentSpacing   = 16.0f;
    s.ScrollbarSize   = 12.0f;

    ImVec4* colors = s.Colors;
    colors[ImGuiCol_WindowBg]          = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    colors[ImGuiCol_ChildBg]           = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    colors[ImGuiCol_PopupBg]           = ImVec4(0.12f, 0.12f, 0.18f, 0.97f);
    colors[ImGuiCol_Border]            = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    colors[ImGuiCol_FrameBg]           = ImVec4(0.14f, 0.14f, 0.22f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]    = ImVec4(0.20f, 0.20f, 0.30f, 1.0f);
    colors[ImGuiCol_FrameBgActive]     = ImVec4(0.25f, 0.25f, 0.38f, 1.0f);
    colors[ImGuiCol_TitleBg]           = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    colors[ImGuiCol_TitleBgActive]     = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    colors[ImGuiCol_MenuBarBg]         = ImVec4(0.10f, 0.10f, 0.16f, 1.0f);
    colors[ImGuiCol_ScrollbarBg]       = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab]     = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    colors[ImGuiCol_CheckMark]         = ImVec4(0.0f, 1.0f, 0.67f, 1.0f);
    colors[ImGuiCol_SliderGrab]        = ImVec4(0.0f, 0.8f, 0.55f, 1.0f);
    colors[ImGuiCol_SliderGrabActive]  = ImVec4(0.0f, 1.0f, 0.67f, 1.0f);
    colors[ImGuiCol_Button]            = ImVec4(0.0f, 0.55f, 0.38f, 0.8f);
    colors[ImGuiCol_ButtonHovered]     = ImVec4(0.0f, 0.8f, 0.55f, 1.0f);
    colors[ImGuiCol_ButtonActive]      = ImVec4(0.0f, 1.0f, 0.67f, 1.0f);
    colors[ImGuiCol_Header]            = ImVec4(0.0f, 0.55f, 0.38f, 0.6f);
    colors[ImGuiCol_HeaderHovered]     = ImVec4(0.0f, 0.8f, 0.55f, 0.8f);
    colors[ImGuiCol_HeaderActive]      = ImVec4(0.0f, 1.0f, 0.67f, 1.0f);
    colors[ImGuiCol_Separator]         = ImVec4(0.20f, 0.20f, 0.32f, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.25f, 0.25f, 0.38f, 1.0f);
    colors[ImGuiCol_TableBorderLight]  = ImVec4(0.15f, 0.15f, 0.25f, 1.0f);
    colors[ImGuiCol_TableRowBg]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt]     = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    colors[ImGuiCol_Text]              = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
    colors[ImGuiCol_TextDisabled]      = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::settings;

    Log::init("straylight-settings");
    SL_INFO("StrayLight Settings starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // --- Wayland --------------------------------------------------------
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
        SL_CRITICAL("Missing required Wayland globals (compositor or xdg_wm_base)");
        wl_display_disconnect(ws.display);
        return EXIT_FAILURE;
    }

    ws.surface = wl_compositor_create_surface(ws.compositor);

    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr,
                                                        ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);

    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Settings");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-settings");

    // Set a minimum size
    xdg_toplevel_set_min_size(ws.toplevel, 800, 560);

    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // --- EGL ------------------------------------------------------------
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
        EGL_STENCIL_SIZE,    8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    EGLConfig egl_config  = nullptr;
    EGLint    num_configs  = 0;
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

    // --- ImGui ----------------------------------------------------------
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                            static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    apply_settings_theme();

    // --- Instantiate settings pages ------------------------------------
    std::vector<std::unique_ptr<SettingsPage>> pages;
    pages.emplace_back(std::make_unique<GeneralPage>());
    pages.emplace_back(std::make_unique<DisplayPage>());
    pages.emplace_back(std::make_unique<AppearancePage>());
    pages.emplace_back(std::make_unique<AudioPage>());
    pages.emplace_back(std::make_unique<NetworkPage>());
    pages.emplace_back(std::make_unique<InputPage>());
    pages.emplace_back(std::make_unique<UsersPage>());
    pages.emplace_back(std::make_unique<AboutPage>());

    // Load all pages
    for (auto& p : pages) {
        p->load();
    }

    size_t selected_page = 0;

    SL_INFO("Settings app initialized with {} pages", pages.size());

    // --- Main loop -------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(ws.display);
        wl_display_flush(ws.display);

        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                                    static_cast<float>(ws.height));
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // Full-screen window
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);

        constexpr ImGuiWindowFlags kWindowFlags =
            ImGuiWindowFlags_NoTitleBar  |
            ImGuiWindowFlags_NoResize    |
            ImGuiWindowFlags_NoMove      |
            ImGuiWindowFlags_NoCollapse  |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin("##Settings", nullptr, kWindowFlags)) {

            // Top bar — branding + title
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
            ImGui::Text("STRAYLIGHT SETTINGS");
            ImGui::PopStyleColor();
            ImGui::SameLine(io.DisplaySize.x - 60.0f);
            if (ImGui::SmallButton("Close")) {
                g_running.store(false, std::memory_order_relaxed);
            }
            ImGui::Separator();

            float content_h   = ImGui::GetContentRegionAvail().y;
            constexpr float kSidebarW = 170.0f;

            // Left sidebar — navigation list
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  ImVec4(0.06f, 0.06f, 0.10f, 1.0f));
            if (ImGui::BeginChild("##sidebar", ImVec2(kSidebarW, content_h),
                                  false)) {
                ImGui::Spacing();

                for (size_t i = 0; i < pages.size(); ++i) {
                    bool selected = (i == selected_page);

                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header,
                                              ImVec4(0.0f, 0.5f, 0.35f, 0.9f));
                    }

                    // Page icon + label
                    const char* icon = "";
                    switch (i) {
                        case 0: icon = " [G] "; break; // General
                        case 1: icon = " [D] "; break; // Display
                        case 2: icon = " [T] "; break; // Appearance/Theme
                        case 3: icon = " [S] "; break; // Sound
                        case 4: icon = " [A] "; break; // Audio
                        case 5: icon = " [N] "; break; // Network
                        case 6: icon = " [I] "; break; // Input
                        case 7: icon = " [U] "; break; // Users
                        case 8: icon = " [?] "; break; // About
                        default: icon = "     "; break;
                    }

                    char nav_label[64];
                    snprintf(nav_label, sizeof(nav_label), "%s%s##nav%zu",
                             icon, pages[i]->label(), i);

                    if (ImGui::Selectable(nav_label, selected,
                                          ImGuiSelectableFlags_None,
                                          ImVec2(0, 32))) {
                        selected_page = i;
                    }

                    if (selected) {
                        ImGui::PopStyleColor();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextDisabled("  Settings v1.0");
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::SameLine();

            // Thin accent separator between sidebar and content
            ImDrawList* draw   = ImGui::GetWindowDrawList();
            ImVec2      sep_p0 = ImGui::GetCursorScreenPos();
            ImVec2      sep_p1 = ImVec2(sep_p0.x, sep_p0.y + content_h);
            draw->AddLine(sep_p0, sep_p1,
                          IM_COL32(0, 200, 130, 80), 1.0f);

            // Right content panel
            if (ImGui::BeginChild("##content",
                                  ImVec2(0.0f, content_h), false,
                                  ImGuiWindowFlags_None)) {
                ImGui::Spacing();
                pages[selected_page]->render();
            }
            ImGui::EndChild();
        }
        ImGui::End();

        // Render frame
        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.08f, 0.08f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);

        // Yield to avoid spinning at 100% CPU
        usleep(16000); // ~60 fps cap
    }

    // --- Cleanup --------------------------------------------------------
    SL_INFO("Settings app shutting down");

    pages.clear();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();

    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
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

    SL_INFO("Settings exited cleanly");
    return EXIT_SUCCESS;
}
