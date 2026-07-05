// apps/player/main.cpp
// StrayLight Media Player — Wayland + EGL + ImGui
#include "playback.h"
#include "playlist.h"
#include "visualizer.h"

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
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::player;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

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

    int width  = 900;
    int height = 600;
    bool configured = false;

    // ImGui input accumulation
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool  mouse_buttons[5] = {};
};

WaylandState g_wl;

// Pointer
static const wl_pointer_listener pointer_listener = {
    // enter
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        g_wl.mouse_x = wl_fixed_to_double(x);
        g_wl.mouse_y = wl_fixed_to_double(y);
    },
    // leave
    [](void*, wl_pointer*, uint32_t, wl_surface*) {},
    // motion
    [](void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        g_wl.mouse_x = wl_fixed_to_double(x);
        g_wl.mouse_y = wl_fixed_to_double(y);
    },
    // button
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t btn, uint32_t state) {
        int idx = (btn == 272) ? 0 : (btn == 273) ? 1 : (btn == 274) ? 2 : -1;
        if (idx >= 0) g_wl.mouse_buttons[idx] = (state == 1);
    },
    // axis
    [](void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t val) {
        ImGuiIO& io = ImGui::GetIO();
        if (axis == 0) io.MouseWheel  -= wl_fixed_to_double(val) / 10.0;
        if (axis == 1) io.MouseWheelH += wl_fixed_to_double(val) / 10.0;
    },
    nullptr, nullptr, nullptr, nullptr, nullptr,
};

// Keyboard
static const wl_keyboard_listener keyboard_listener = {
    [](void*, wl_keyboard*, uint32_t, int32_t, uint32_t) {},
    [](void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {},
    [](void*, wl_keyboard*, uint32_t, wl_surface*) {},
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        (void)key; (void)state;
    },
    [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    nullptr,
};

// Seat
static const wl_seat_listener seat_listener = {
    [](void*, wl_seat* seat, uint32_t caps) {
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl.pointer) {
            g_wl.pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(g_wl.pointer, &pointer_listener, nullptr);
        }
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl.keyboard) {
            g_wl.keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(g_wl.keyboard, &keyboard_listener, nullptr);
        }
    },
    [](void*, wl_seat*, const char*) {},
};

// XDG surface
static const xdg_surface_listener xdg_surface_listener_impl = {
    [](void*, xdg_surface* xdg_surface, uint32_t serial) {
        xdg_surface_ack_configure(xdg_surface, serial);
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

// XDG WM base ping
static const xdg_wm_base_listener xdg_wm_base_listener_impl = {
    [](void*, xdg_wm_base* wm, uint32_t serial) { xdg_wm_base_pong(wm, serial); },
};

// Registry
static const wl_registry_listener registry_listener_impl = {
    [](void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
        if (!std::strcmp(iface, wl_compositor_interface.name)) {
            g_wl.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, 4));
        } else if (!std::strcmp(iface, xdg_wm_base_interface.name)) {
            g_wl.xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
                wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
            xdg_wm_base_add_listener(g_wl.xdg_wm_base_ptr, &xdg_wm_base_listener_impl, nullptr);
        } else if (!std::strcmp(iface, wl_seat_interface.name)) {
            g_wl.seat = static_cast<wl_seat*>(
                wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
            wl_seat_add_listener(g_wl.seat, &seat_listener, nullptr);
        }
    },
    [](void*, wl_registry*, uint32_t) {},
};

// ---------------------------------------------------------------------------
// EGL state
// ---------------------------------------------------------------------------

struct EGLState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    wl_egl_window* wl_window = nullptr;
};

EGLState g_egl;

bool egl_init() {
    g_egl.display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, g_wl.display, nullptr);
    if (g_egl.display == EGL_NO_DISPLAY) return false;

    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_egl.display, &major, &minor)) return false;

    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint n_configs = 0;
    if (!eglChooseConfig(g_egl.display, cfg_attribs, &config, 1, &n_configs) || n_configs < 1)
        return false;

    const EGLint ctx_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    g_egl.context = eglCreateContext(g_egl.display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (g_egl.context == EGL_NO_CONTEXT) return false;

    g_egl.wl_window = wl_egl_window_create(g_wl.surface, g_wl.width, g_wl.height);
    g_egl.surface = eglCreateWindowSurface(g_egl.display, config,
                                            reinterpret_cast<EGLNativeWindowType>(g_egl.wl_window),
                                            nullptr);
    if (g_egl.surface == EGL_NO_SURFACE) return false;

    return eglMakeCurrent(g_egl.display, g_egl.surface, g_egl.surface, g_egl.context);
}

void egl_resize() {
    if (g_egl.wl_window)
        wl_egl_window_resize(g_egl.wl_window, g_wl.width, g_wl.height, 0, 0);
}

void egl_shutdown() {
    if (g_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl.surface != EGL_NO_SURFACE) eglDestroySurface(g_egl.display, g_egl.surface);
        if (g_egl.context != EGL_NO_CONTEXT) eglDestroyContext(g_egl.display, g_egl.context);
        eglTerminate(g_egl.display);
    }
    if (g_egl.wl_window) wl_egl_window_destroy(g_egl.wl_window);
}

// ---------------------------------------------------------------------------
// Wayland init
// ---------------------------------------------------------------------------

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
    xdg_toplevel_set_title(g_wl.toplevel, "StrayLight Player");
    xdg_toplevel_set_app_id(g_wl.toplevel, "straylight.player");
    wl_surface_commit(g_wl.surface);
    wl_display_roundtrip(g_wl.display);
    return true;
}

void wayland_shutdown() {
    if (g_wl.toplevel)       xdg_toplevel_destroy(g_wl.toplevel);
    if (g_wl.xdg_surface_ptr)xdg_surface_destroy(g_wl.xdg_surface_ptr);
    if (g_wl.surface)        wl_surface_destroy(g_wl.surface);
    if (g_wl.pointer)        wl_pointer_destroy(g_wl.pointer);
    if (g_wl.keyboard)       wl_keyboard_destroy(g_wl.keyboard);
    if (g_wl.seat)           wl_seat_destroy(g_wl.seat);
    if (g_wl.xdg_wm_base_ptr)xdg_wm_base_destroy(g_wl.xdg_wm_base_ptr);
    if (g_wl.compositor)     wl_compositor_destroy(g_wl.compositor);
    if (g_wl.registry)       wl_registry_destroy(g_wl.registry);
    if (g_wl.display)        wl_display_disconnect(g_wl.display);
}

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

std::string format_time(double seconds) {
    if (seconds < 0) return "--:--";
    const int total = static_cast<int>(seconds);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    char buf[16];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    PlaybackEngine::gst_init_once(&argc, &argv);

    if (!wayland_init()) {
        SL_ERROR("Wayland init failed");
        return 1;
    }
    if (!egl_init()) {
        SL_ERROR("EGL init failed");
        return 1;
    }

    // Wait for compositor to configure the surface
    while (!g_wl.configured) wl_display_dispatch(g_wl.display);

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_wl.width), static_cast<float>(g_wl.height));
    ImGui::StyleColorsDark();
    // Cyberpunk colour accents
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.07f, 0.09f, 0.95f);
    colors[ImGuiCol_TitleBgActive]  = ImVec4(0.22f, 0.0f,  0.36f, 1.0f);
    colors[ImGuiCol_Button]         = ImVec4(0.18f, 0.0f,  0.28f, 1.0f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.30f, 0.0f,  0.50f, 1.0f);
    colors[ImGuiCol_Header]         = ImVec4(0.18f, 0.0f,  0.28f, 1.0f);
    colors[ImGuiCol_HeaderHovered]  = ImVec4(0.30f, 0.0f,  0.50f, 1.0f);
    colors[ImGuiCol_SliderGrab]     = ImVec4(0.80f, 0.10f, 0.90f, 1.0f);

    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Player state
    PlaybackEngine engine;
    Playlist       playlist;
    AudioVisualizer visualizer;

    engine.on_end_of_stream([&] {
        if (playlist.next()) {
            if (const Track* t = playlist.current_track()) {
                engine.open(t->uri);
                engine.play();
            }
        }
    });
    engine.on_error([](const std::string& msg) {
        SL_ERROR("Playback error: {}", msg);
    });

    // Load files from command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path p(argv[i]);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            playlist.load_directory(p);
        } else if (p.extension() == ".m3u" || p.extension() == ".m3u8") {
            playlist.load_m3u(p);
        } else if (p.extension() == ".pls") {
            playlist.load_pls(p);
        } else {
            Track t;
            t.uri   = "file://" + p.string();
            t.title = p.stem().string();
            playlist.add_track(std::move(t));
        }
    }

    char open_path_buf[512] = {};
    bool show_open_dialog   = false;
    VisMode vis_modes[]     = {VisMode::Spectrum, VisMode::VUMeter, VisMode::Oscilloscope};
    const char* vis_names[] = {"Spectrum", "VU Meter", "Oscilloscope"};
    int vis_mode_idx        = 0;

    // ---------------------------------------------------------------------------
    // Main loop
    // ---------------------------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        // Wayland events
        wl_display_dispatch_pending(g_wl.display);
        if (wl_display_flush(g_wl.display) < 0) break;

        // Pump GStreamer messages
        engine.pump_messages();

        // Update visualizer
        visualizer.set_mode(vis_modes[vis_mode_idx % 3]);
        visualizer.update(engine.level());

        // Resize if needed
        egl_resize();

        // ImGui new frame
        io.DisplaySize = ImVec2(static_cast<float>(g_wl.width),
                                static_cast<float>(g_wl.height));
        io.DeltaTime   = 1.0f / 60.0f;
        io.MousePos    = ImVec2(g_wl.mouse_x, g_wl.mouse_y);
        for (int b = 0; b < 5; ++b) io.MouseDown[b] = g_wl.mouse_buttons[b];

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // ---- Transport window ----
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_wl.width),
                                        static_cast<float>(g_wl.height)), ImGuiCond_Always);
        ImGui::Begin("##player_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_MenuBar);

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open File/Directory...")) show_open_dialog = true;
                if (ImGui::MenuItem("Clear Playlist"))         playlist.clear();
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) g_running.store(false, std::memory_order_relaxed);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Visualizer")) {
                for (int i = 0; i < 3; ++i) {
                    bool sel = (vis_mode_idx == i);
                    if (ImGui::MenuItem(vis_names[i], nullptr, sel)) vis_mode_idx = i;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        const float win_w  = ImGui::GetContentRegionAvail().x;
        const float win_h  = ImGui::GetContentRegionAvail().y;

        // --- Left panel: playlist ---
        const float playlist_w = win_w * 0.38f;
        ImGui::BeginChild("##playlist_panel", ImVec2(playlist_w, win_h), true);
        ImGui::TextUnformatted("Playlist");
        ImGui::Separator();
        for (int i = 0; i < playlist.count(); ++i) {
            const Track& t = playlist.tracks()[i];
            const bool is_current = (i == playlist.current_index());
            if (is_current) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 1.0f, 1.0f));
            char label[256];
            std::snprintf(label, sizeof(label), "%s", t.title.c_str());
            if (ImGui::Selectable(label, is_current)) {
                playlist.jump_to(i);
                if (const Track* ct = playlist.current_track()) {
                    engine.open(ct->uri);
                    engine.play();
                }
            }
            if (is_current) ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // --- Right panel: transport + visualizer ---
        ImGui::BeginChild("##transport_panel", ImVec2(0, win_h), false);

        // Now playing
        const Track* cur = playlist.current_track();
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.5f, 1.0f, 1.0f));
            ImGui::Text("Now Playing: %s", cur ? cur->title.c_str() : "(nothing)");
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();

        // Seek bar
        {
            const double dur = engine.duration();
            const double pos = engine.position();
            float seek_pos = (dur > 0.0) ? static_cast<float>(pos / dur) : 0.0f;
            ImGui::Text("%s / %s", format_time(pos).c_str(), format_time(dur).c_str());
            if (ImGui::SliderFloat("##seek", &seek_pos, 0.0f, 1.0f, "")) {
                if (dur > 0.0) engine.seek(static_cast<double>(seek_pos) * dur);
            }
        }
        ImGui::Spacing();

        // Transport buttons
        const bool playing = (engine.state() == PlaybackState::Playing);
        if (ImGui::Button("|<<", ImVec2(48, 28))) {
            if (playlist.prev()) {
                if (const Track* t = playlist.current_track()) {
                    engine.open(t->uri);
                    engine.play();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(playing ? "||" : " >", ImVec2(48, 28))) {
            if (playing) engine.pause();
            else {
                if (engine.state() == PlaybackState::Stopped && cur) engine.open(cur->uri);
                engine.play();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("[]", ImVec2(48, 28))) engine.stop();
        ImGui::SameLine();
        if (ImGui::Button(">>|", ImVec2(48, 28))) {
            if (playlist.next()) {
                if (const Track* t = playlist.current_track()) {
                    engine.open(t->uri);
                    engine.play();
                }
            }
        }
        ImGui::SameLine(0, 16);

        // Shuffle / Repeat
        bool sh = playlist.shuffle();
        if (ImGui::Checkbox("Shuffle", &sh)) playlist.set_shuffle(sh);
        ImGui::SameLine();
        const char* rep_labels[] = {"No Repeat", "Repeat One", "Repeat All"};
        int rep_idx = static_cast<int>(playlist.repeat());
        if (ImGui::Button(rep_labels[rep_idx])) {
            playlist.set_repeat(static_cast<RepeatMode>((rep_idx + 1) % 3));
        }

        ImGui::Spacing();

        // Volume
        {
            float vol = static_cast<float>(engine.volume());
            bool muted = engine.muted();
            if (ImGui::Checkbox("Mute", &muted)) engine.set_mute(muted);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f)) engine.set_volume(vol);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Visualizer: %s", vis_names[vis_mode_idx]);
        ImGui::SameLine();
        if (ImGui::Button("Next Mode##vis")) vis_mode_idx = (vis_mode_idx + 1) % 3;

        // Visualizer canvas
        const float vis_h = std::max(80.0f, win_h - ImGui::GetCursorPosY() - 4.0f);
        const float vis_w = ImGui::GetContentRegionAvail().x;
        ImVec2 vis_pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(vis_w, vis_h));
        visualizer.draw(ImGui::GetWindowDrawList(), vis_pos, ImVec2(vis_w, vis_h));

        ImGui::EndChild();

        // --- Open file dialog (simple text-input based) ---
        if (show_open_dialog) {
            ImGui::OpenPopup("Open File##open_dlg");
            show_open_dialog = false;
        }
        if (ImGui::BeginPopupModal("Open File##open_dlg", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter path to file or directory:");
            ImGui::SetNextItemWidth(400.0f);
            ImGui::InputText("##open_path", open_path_buf, sizeof(open_path_buf));
            if (ImGui::Button("Open", ImVec2(80, 0))) {
                std::filesystem::path p(open_path_buf);
                std::error_code ec;
                if (std::filesystem::is_directory(p, ec)) {
                    playlist.load_directory(p);
                } else if (p.extension() == ".m3u" || p.extension() == ".m3u8") {
                    playlist.load_m3u(p);
                } else {
                    Track t;
                    t.uri   = "file://" + p.string();
                    t.title = p.stem().string();
                    playlist.add_track(std::move(t));
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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    egl_shutdown();
    wayland_shutdown();
    return 0;
}
