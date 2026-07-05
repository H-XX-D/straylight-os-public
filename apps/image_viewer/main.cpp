// apps/image_viewer/main.cpp
// StrayLight Image Viewer — Wayland + EGL + ImGui
#include "loader.h"
#include "viewer.h"
#include "thumbnails.h"

#include <straylight/log.h>

#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>
#include <xdg-shell-client-protocol.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::viewer;

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Wayland state
// ---------------------------------------------------------------------------

struct WaylandState {
    wl_display*    display     = nullptr;
    wl_registry*   registry    = nullptr;
    wl_compositor* compositor  = nullptr;
    wl_seat*       seat        = nullptr;
    wl_pointer*    pointer     = nullptr;
    wl_keyboard*   keyboard    = nullptr;
    xdg_wm_base*   xdg_wm_base_ptr = nullptr;
    wl_surface*    surface     = nullptr;
    xdg_surface*   xdg_surface_ptr = nullptr;
    xdg_toplevel*  toplevel    = nullptr;

    int   width      = 1100;
    int   height     = 720;
    bool  configured = false;
    float mouse_x    = 0.0f;
    float mouse_y    = 0.0f;
    bool  mouse_buttons[5] = {};
    float mouse_wheel = 0.0f;
};

WaylandState g_wl;

static const wl_pointer_listener pointer_listener_impl = {
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        g_wl.mouse_x = wl_fixed_to_double(x);
        g_wl.mouse_y = wl_fixed_to_double(y);
    },
    [](void*, wl_pointer*, uint32_t, wl_surface*) {},
    [](void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        g_wl.mouse_x = wl_fixed_to_double(x);
        g_wl.mouse_y = wl_fixed_to_double(y);
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t btn, uint32_t state) {
        int idx = (btn == 272) ? 0 : (btn == 273) ? 1 : (btn == 274) ? 2 : -1;
        if (idx >= 0) g_wl.mouse_buttons[idx] = (state == 1);
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t val) {
        g_wl.mouse_wheel -= wl_fixed_to_double(val) / 10.0;
    },
    nullptr, nullptr, nullptr, nullptr, nullptr,
};

static const wl_keyboard_listener keyboard_listener_impl = {
    [](void*, wl_keyboard*, uint32_t, int32_t, uint32_t) {},
    [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},
    [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t) {},
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    nullptr,
};

static const wl_seat_listener seat_listener_impl = {
    [](void*, wl_seat* seat, uint32_t caps) {
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl.pointer) {
            g_wl.pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(g_wl.pointer, &pointer_listener_impl, nullptr);
        }
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl.keyboard) {
            g_wl.keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(g_wl.keyboard, &keyboard_listener_impl, nullptr);
        }
    },
    [](void*, wl_seat*, const char*) {},
};

static const xdg_surface_listener xdg_surface_listener_impl = {
    [](void*, xdg_surface* xs, uint32_t serial) {
        xdg_surface_ack_configure(xs, serial);
        g_wl.configured = true;
    },
};

static const xdg_toplevel_listener xdg_toplevel_listener_impl = {
    [](void*, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        if (w > 0 && h > 0) { g_wl.width = w; g_wl.height = h; }
    },
    [](void*, xdg_toplevel*) { g_running.store(false, std::memory_order_relaxed); },
    nullptr, nullptr,
};

static const xdg_wm_base_listener xdg_wm_base_listener_impl = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); },
};

static const wl_registry_listener registry_listener_impl = {
    [](void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
        if (!std::strcmp(iface, wl_compositor_interface.name))
            g_wl.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, 4));
        else if (!std::strcmp(iface, xdg_wm_base_interface.name)) {
            g_wl.xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
                wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
            xdg_wm_base_add_listener(g_wl.xdg_wm_base_ptr, &xdg_wm_base_listener_impl, nullptr);
        } else if (!std::strcmp(iface, wl_seat_interface.name)) {
            g_wl.seat = static_cast<wl_seat*>(
                wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
            wl_seat_add_listener(g_wl.seat, &seat_listener_impl, nullptr);
        }
    },
    [](void*, wl_registry*, uint32_t) {},
};

// ---------------------------------------------------------------------------
// EGL state
// ---------------------------------------------------------------------------

struct EGLState {
    EGLDisplay display    = EGL_NO_DISPLAY;
    EGLContext context    = EGL_NO_CONTEXT;
    EGLSurface surface    = EGL_NO_SURFACE;
    wl_egl_window* window = nullptr;
};

EGLState g_egl;

bool egl_init() {
    g_egl.display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, g_wl.display, nullptr);
    if (g_egl.display == EGL_NO_DISPLAY) return false;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_egl.display, &major, &minor)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLConfig config = nullptr; EGLint nc = 0;
    if (!eglChooseConfig(g_egl.display, cfg_attribs, &config, 1, &nc) || nc < 1) return false;
    const EGLint ctx_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    g_egl.context = eglCreateContext(g_egl.display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (g_egl.context == EGL_NO_CONTEXT) return false;
    g_egl.window  = wl_egl_window_create(g_wl.surface, g_wl.width, g_wl.height);
    g_egl.surface = eglCreateWindowSurface(g_egl.display, config,
                       reinterpret_cast<EGLNativeWindowType>(g_egl.window), nullptr);
    return eglMakeCurrent(g_egl.display, g_egl.surface, g_egl.surface, g_egl.context);
}

void egl_resize() {
    if (g_egl.window) wl_egl_window_resize(g_egl.window, g_wl.width, g_wl.height, 0, 0);
}

void egl_shutdown() {
    if (g_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl.surface != EGL_NO_SURFACE) eglDestroySurface(g_egl.display, g_egl.surface);
        if (g_egl.context != EGL_NO_CONTEXT) eglDestroyContext(g_egl.display, g_egl.context);
        eglTerminate(g_egl.display);
    }
    if (g_egl.window) wl_egl_window_destroy(g_egl.window);
}

bool wayland_init() {
    g_wl.display = wl_display_connect(nullptr);
    if (!g_wl.display) return false;
    g_wl.registry = wl_display_get_registry(g_wl.display);
    wl_registry_add_listener(g_wl.registry, &registry_listener_impl, nullptr);
    wl_display_roundtrip(g_wl.display);
    if (!g_wl.compositor || !g_wl.xdg_wm_base_ptr) return false;
    g_wl.surface = wl_compositor_create_surface(g_wl.compositor);
    g_wl.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(g_wl.xdg_wm_base_ptr, g_wl.surface);
    xdg_surface_add_listener(g_wl.xdg_surface_ptr, &xdg_surface_listener_impl, nullptr);
    g_wl.toplevel = xdg_surface_get_toplevel(g_wl.xdg_surface_ptr);
    xdg_toplevel_add_listener(g_wl.toplevel, &xdg_toplevel_listener_impl, nullptr);
    xdg_toplevel_set_title(g_wl.toplevel, "StrayLight Image Viewer");
    xdg_toplevel_set_app_id(g_wl.toplevel, "straylight.imageviewer");
    wl_surface_commit(g_wl.surface);
    wl_display_roundtrip(g_wl.display);
    return true;
}

void wayland_shutdown() {
    if (g_wl.toplevel)        xdg_toplevel_destroy(g_wl.toplevel);
    if (g_wl.xdg_surface_ptr) xdg_surface_destroy(g_wl.xdg_surface_ptr);
    if (g_wl.surface)         wl_surface_destroy(g_wl.surface);
    if (g_wl.pointer)         wl_pointer_destroy(g_wl.pointer);
    if (g_wl.keyboard)        wl_keyboard_destroy(g_wl.keyboard);
    if (g_wl.seat)            wl_seat_destroy(g_wl.seat);
    if (g_wl.xdg_wm_base_ptr) xdg_wm_base_destroy(g_wl.xdg_wm_base_ptr);
    if (g_wl.compositor)      wl_compositor_destroy(g_wl.compositor);
    if (g_wl.registry)        wl_registry_destroy(g_wl.registry);
    if (g_wl.display)         wl_display_disconnect(g_wl.display);
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!wayland_init()) { SL_ERROR("Wayland init failed"); return 1; }
    if (!egl_init())     { SL_ERROR("EGL init failed");     return 1; }

    while (!g_wl.configured) wl_display_dispatch(g_wl.display);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_wl.width), static_cast<float>(g_wl.height));
    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg]      = ImVec4(0.07f, 0.07f, 0.09f, 0.95f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.0f,  0.22f, 1.0f);
    colors[ImGuiCol_Button]        = ImVec4(0.14f, 0.0f,  0.22f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.0f,  0.44f, 1.0f);

    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Viewer state
    ImageCanvas    canvas;
    ThumbnailGrid  thumb_grid;
    std::optional<ImageAsset> current_image;
    std::filesystem::path current_dir;
    char open_path_buf[512] = {};
    bool show_open_dialog   = false;
    bool fit_mode           = true;  // true = fit-to-window, false = free pan/zoom
    int  sidebar_w          = 170;

    // Auto-open argument
    if (argc > 1) {
        std::filesystem::path p(argv[1]);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            current_dir = p;
            thumb_grid.scan(p);
        } else if (ImageLoader::is_supported(p)) {
            auto res = ImageLoader::load(p);
            if (res.has_value()) {
                current_image = std::move(res).value();
                current_dir   = p.parent_path();
                thumb_grid.scan(current_dir);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Main loop
    // ---------------------------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(g_wl.display);
        if (wl_display_flush(g_wl.display) < 0) break;

        // Generate pending thumbnails (lazy, 2/frame)
        thumb_grid.generate_pending(2);

        egl_resize();

        // Feed ImGui input
        io.DisplaySize = ImVec2(static_cast<float>(g_wl.width),
                                static_cast<float>(g_wl.height));
        io.DeltaTime   = 1.0f / 60.0f;
        io.MousePos    = ImVec2(g_wl.mouse_x, g_wl.mouse_y);
        for (int b = 0; b < 5; ++b) io.MouseDown[b] = g_wl.mouse_buttons[b];
        io.MouseWheel  = g_wl.mouse_wheel;
        g_wl.mouse_wheel = 0.0f;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // Main window
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_wl.width),
                                        static_cast<float>(g_wl.height)), ImGuiCond_Always);
        ImGui::Begin("##viewer_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_MenuBar);

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open...")) show_open_dialog = true;
                if (ImGui::MenuItem("Quit"))
                    g_running.store(false, std::memory_order_relaxed);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Fit to Window", nullptr, fit_mode)) {
                    fit_mode = true;
                    if (current_image)
                        canvas.fit_to_window(ImVec2(
                            static_cast<float>(g_wl.width - sidebar_w),
                            static_cast<float>(g_wl.height - 40)));
                }
                if (ImGui::MenuItem("Actual Size", nullptr, !fit_mode)) {
                    fit_mode = false;
                    canvas.actual_size(ImVec2(
                        static_cast<float>(g_wl.width - sidebar_w),
                        static_cast<float>(g_wl.height - 40)));
                }
                if (ImGui::MenuItem("Zoom In"))  canvas.set_zoom(canvas.zoom() * 1.25f);
                if (ImGui::MenuItem("Zoom Out")) canvas.set_zoom(canvas.zoom() / 1.25f);
                ImGui::EndMenu();
            }
            // Info
            if (current_image) {
                ImGui::Text("  %s  [%ux%u]  Zoom: %.0f%%",
                    current_image->path.filename().c_str(),
                    current_image->width, current_image->height,
                    (fit_mode ? 100.0f : canvas.zoom() * 100.0f));
            }
            ImGui::EndMenuBar();
        }

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y;

        // --- Sidebar: thumbnail strip ---
        ImGui::BeginChild("##sidebar", ImVec2(static_cast<float>(sidebar_w), avail_h), true,
                           ImGuiWindowFlags_HorizontalScrollbar);
        if (!current_dir.empty()) {
            ImGui::TextUnformatted("Thumbnails");
            ImGui::Separator();
            int clicked = thumb_grid.draw_grid(static_cast<float>(sidebar_w) - 16.0f, 80.0f);
            if (clicked >= 0 && clicked < thumb_grid.count()) {
                const auto& entry = thumb_grid.entries()[static_cast<size_t>(clicked)];
                if (current_image) ImageLoader::unload(*current_image);
                auto res = ImageLoader::load(entry.path);
                if (res.has_value()) {
                    current_image = std::move(res).value();
                    fit_mode = true;
                }
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // --- Main canvas ---
        const float cv_w = avail_w - static_cast<float>(sidebar_w) - 4.0f;
        const float cv_h = avail_h;
        ImVec2 cv_pos = ImGui::GetCursorScreenPos();

        ImGui::BeginChild("##canvas", ImVec2(cv_w, cv_h), false,
                           ImGuiWindowFlags_NoScrollbar);

        if (current_image && current_image->valid()) {
            ImVec2 cv_screen = ImGui::GetCursorScreenPos();
            ImVec2 cv_size   = ImVec2(cv_w, cv_h);

            if (fit_mode) {
                // Auto-fit: reset pan to 0 and let draw() compute zoom
                canvas.actual_size(cv_size); // sets zoom=1, pan=0
                // canvas.draw() with zoom=1 at pan=0 triggers fit behaviour
            } else {
                canvas.process_input(cv_screen, cv_size);
            }

            canvas.draw(ImGui::GetWindowDrawList(), cv_screen, cv_size, *current_image);
            ImGui::Dummy(cv_size);

            // Status bar at bottom of canvas
            ImGui::SetCursorPos(ImVec2(4, cv_h - 20.0f));
            ImGui::Text("EXIF orient: %d | Channels: %d",
                         current_image->exif_orient, current_image->channels);
        } else {
            ImGui::SetCursorPosY(cv_h * 0.45f);
            ImGui::SetCursorPosX((cv_w - ImGui::CalcTextSize("Open an image to begin").x) * 0.5f);
            ImGui::TextDisabled("Open an image to begin");
        }

        ImGui::EndChild();

        // Open dialog
        if (show_open_dialog) { ImGui::OpenPopup("Open##dlg"); show_open_dialog = false; }
        if (ImGui::BeginPopupModal("Open##dlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter file or directory path:");
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputText("##opath", open_path_buf, sizeof(open_path_buf));
            if (ImGui::Button("Open", ImVec2(80, 0))) {
                std::filesystem::path p(open_path_buf);
                std::error_code ec;
                if (std::filesystem::is_directory(p, ec)) {
                    current_dir = p;
                    thumb_grid.scan(p);
                } else if (ImageLoader::is_supported(p)) {
                    if (current_image) ImageLoader::unload(*current_image);
                    auto res = ImageLoader::load(p);
                    if (res.has_value()) {
                        current_image = std::move(res).value();
                        current_dir   = p.parent_path();
                        thumb_grid.scan(current_dir);
                        fit_mode = true;
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::End();

        // Render
        ImGui::Render();
        glViewport(0, 0, g_wl.width, g_wl.height);
        glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(g_egl.display, g_egl.surface);
    }

    if (current_image) ImageLoader::unload(*current_image);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    egl_shutdown();
    wayland_shutdown();
    return 0;
}
