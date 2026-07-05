// apps/clipboard/main.cpp
// StrayLight Clipboard Manager — Wayland + EGL + ImGui
#include "history.h"
#include "watcher.h"

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
#include <mutex>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::clipboard;

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Wayland / EGL boilerplate (shared pattern)
// ---------------------------------------------------------------------------

struct WaylandState {
    wl_display*    display    = nullptr;
    wl_registry*   registry   = nullptr;
    wl_compositor* compositor = nullptr;
    wl_seat*       seat       = nullptr;
    wl_pointer*    pointer    = nullptr;
    wl_keyboard*   keyboard   = nullptr;
    xdg_wm_base*   xdg_wm    = nullptr;
    wl_surface*    surface    = nullptr;
    xdg_surface*   xdg_surf  = nullptr;
    xdg_toplevel*  toplevel  = nullptr;

    int   width      = 420;
    int   height     = 640;
    bool  configured = false;
    float mouse_x    = 0.0f;
    float mouse_y    = 0.0f;
    bool  mouse_buttons[5] = {};
};

WaylandState g_wl;

static const wl_pointer_listener ptr_l = {
    [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
        g_wl.mouse_x = wl_fixed_to_double(x); g_wl.mouse_y = wl_fixed_to_double(y);
    },
    [](void*, wl_pointer*, uint32_t, wl_surface*) {},
    [](void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        g_wl.mouse_x = wl_fixed_to_double(x); g_wl.mouse_y = wl_fixed_to_double(y);
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t btn, uint32_t s) {
        int i = (btn==272)?0:(btn==273)?1:(btn==274)?2:-1;
        if (i>=0) g_wl.mouse_buttons[i] = (s==1);
    },
    [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t v) {
        ImGui::GetIO().MouseWheel -= wl_fixed_to_double(v) / 10.0;
    },
    nullptr,nullptr,nullptr,nullptr,nullptr,
};

static const wl_keyboard_listener kbd_l = {
    [](void*,wl_keyboard*,uint32_t,int32_t,uint32_t){},
    [](void*,wl_keyboard*,uint32_t,wl_surface*,wl_array*){},
    [](void*,wl_keyboard*,uint32_t,wl_surface*){},
    [](void*,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t){},
    [](void*,wl_keyboard*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t){},
    nullptr,
};

static const wl_seat_listener seat_l = {
    [](void*, wl_seat* s, uint32_t caps) {
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl.pointer) {
            g_wl.pointer = wl_seat_get_pointer(s);
            wl_pointer_add_listener(g_wl.pointer, &ptr_l, nullptr);
        }
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl.keyboard) {
            g_wl.keyboard = wl_seat_get_keyboard(s);
            wl_keyboard_add_listener(g_wl.keyboard, &kbd_l, nullptr);
        }
    },
    [](void*,wl_seat*,const char*){},
};

static const xdg_surface_listener xdg_surf_l = {
    [](void*, xdg_surface* xs, uint32_t serial) {
        xdg_surface_ack_configure(xs, serial); g_wl.configured = true;
    },
};

static const xdg_toplevel_listener toplevel_l = {
    [](void*, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
        if (w>0&&h>0){g_wl.width=w;g_wl.height=h;}
    },
    [](void*, xdg_toplevel*) { g_running.store(false, std::memory_order_relaxed); },
    nullptr, nullptr,
};

static const xdg_wm_base_listener wm_l = {
    [](void*, xdg_wm_base* wm, uint32_t s){ xdg_wm_base_pong(wm,s); },
};

static const wl_registry_listener reg_l = {
    [](void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
        if (!std::strcmp(iface, wl_compositor_interface.name))
            g_wl.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(reg, name, &wl_compositor_interface, 4));
        else if (!std::strcmp(iface, xdg_wm_base_interface.name)) {
            g_wl.xdg_wm = static_cast<xdg_wm_base*>(
                wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
            xdg_wm_base_add_listener(g_wl.xdg_wm, &wm_l, nullptr);
        } else if (!std::strcmp(iface, wl_seat_interface.name)) {
            g_wl.seat = static_cast<wl_seat*>(
                wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
            wl_seat_add_listener(g_wl.seat, &seat_l, nullptr);
        }
    },
    [](void*, wl_registry*, uint32_t){},
};

struct EGLState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    wl_egl_window* window = nullptr;
};
EGLState g_egl;

bool egl_init() {
    g_egl.display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, g_wl.display, nullptr);
    if (g_egl.display == EGL_NO_DISPLAY) return false;
    EGLint major=0,minor=0;
    if (!eglInitialize(g_egl.display,&major,&minor)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {
        EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE
    };
    EGLConfig cfg=nullptr; EGLint nc=0;
    if (!eglChooseConfig(g_egl.display,ca,&cfg,1,&nc)||nc<1) return false;
    const EGLint cxa[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_NONE};
    g_egl.context = eglCreateContext(g_egl.display,cfg,EGL_NO_CONTEXT,cxa);
    if (g_egl.context==EGL_NO_CONTEXT) return false;
    g_egl.window  = wl_egl_window_create(g_wl.surface, g_wl.width, g_wl.height);
    g_egl.surface = eglCreateWindowSurface(g_egl.display,cfg,
        reinterpret_cast<EGLNativeWindowType>(g_egl.window), nullptr);
    return eglMakeCurrent(g_egl.display,g_egl.surface,g_egl.surface,g_egl.context);
}

void egl_shutdown() {
    if (g_egl.display!=EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if (g_egl.surface!=EGL_NO_SURFACE) eglDestroySurface(g_egl.display,g_egl.surface);
        if (g_egl.context!=EGL_NO_CONTEXT) eglDestroyContext(g_egl.display,g_egl.context);
        eglTerminate(g_egl.display);
    }
    if (g_egl.window) wl_egl_window_destroy(g_egl.window);
}

bool wayland_init() {
    g_wl.display = wl_display_connect(nullptr);
    if (!g_wl.display) return false;
    g_wl.registry = wl_display_get_registry(g_wl.display);
    wl_registry_add_listener(g_wl.registry, &reg_l, nullptr);
    wl_display_roundtrip(g_wl.display);
    if (!g_wl.compositor || !g_wl.xdg_wm) return false;
    g_wl.surface  = wl_compositor_create_surface(g_wl.compositor);
    g_wl.xdg_surf = xdg_wm_base_get_xdg_surface(g_wl.xdg_wm, g_wl.surface);
    xdg_surface_add_listener(g_wl.xdg_surf, &xdg_surf_l, nullptr);
    g_wl.toplevel = xdg_surface_get_toplevel(g_wl.xdg_surf);
    xdg_toplevel_add_listener(g_wl.toplevel, &toplevel_l, nullptr);
    xdg_toplevel_set_title(g_wl.toplevel,  "StrayLight Clipboard");
    xdg_toplevel_set_app_id(g_wl.toplevel, "straylight.clipboard");
    wl_surface_commit(g_wl.surface);
    wl_display_roundtrip(g_wl.display);
    return true;
}

void wayland_shutdown() {
    if (g_wl.toplevel)    xdg_toplevel_destroy(g_wl.toplevel);
    if (g_wl.xdg_surf)    xdg_surface_destroy(g_wl.xdg_surf);
    if (g_wl.surface)     wl_surface_destroy(g_wl.surface);
    if (g_wl.pointer)     wl_pointer_destroy(g_wl.pointer);
    if (g_wl.keyboard)    wl_keyboard_destroy(g_wl.keyboard);
    if (g_wl.seat)        wl_seat_destroy(g_wl.seat);
    if (g_wl.xdg_wm)     xdg_wm_base_destroy(g_wl.xdg_wm);
    if (g_wl.compositor)  wl_compositor_destroy(g_wl.compositor);
    if (g_wl.registry)    wl_registry_destroy(g_wl.registry);
    if (g_wl.display)     wl_display_disconnect(g_wl.display);
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int /*argc*/, char* /*argv*/[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!wayland_init()) { SL_LOG_ERROR("clipboard", "Wayland init failed"); return 1; }
    if (!egl_init())     { SL_LOG_ERROR("clipboard", "EGL init failed");     return 1; }

    while (!g_wl.configured) wl_display_dispatch(g_wl.display);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_wl.width), static_cast<float>(g_wl.height));
    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.07f, 0.09f, 0.97f);
    colors[ImGuiCol_TitleBgActive]  = ImVec4(0.18f, 0.0f,  0.30f, 1.0f);
    colors[ImGuiCol_Button]         = ImVec4(0.18f, 0.0f,  0.28f, 1.0f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.32f, 0.0f,  0.50f, 1.0f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.12f, 0.12f, 0.16f, 1.0f);

    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Clipboard state
    ClipHistory  history;
    history.load();

    ClipboardWatcher watcher;
    {
        auto res = watcher.connect();
        if (!res.has_value()) {
            SL_LOG_WARN("clipboard", "Watcher connect failed: {}",
                         res.error().message());
        } else {
            watcher.start([&](ClipEntry e) {
                if (e.kind == EntryKind::Text)
                    history.push_text(std::move(e.text), e.mime);
                else
                    history.push_image(std::move(e.image_data), e.mime);
            });
        }
    }

    char search_buf[256] = {};
    bool show_confirm_clear = false;

    // ---------------------------------------------------------------------------
    // Main loop
    // ---------------------------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(g_wl.display);
        if (wl_display_flush(g_wl.display) < 0) break;

        if (g_wl.width != static_cast<int>(io.DisplaySize.x) ||
            g_wl.height != static_cast<int>(io.DisplaySize.y)) {
            wl_egl_window_resize(g_egl.window, g_wl.width, g_wl.height, 0, 0);
        }

        io.DisplaySize = ImVec2(static_cast<float>(g_wl.width),
                                static_cast<float>(g_wl.height));
        io.DeltaTime   = 1.0f / 60.0f;
        io.MousePos    = ImVec2(g_wl.mouse_x, g_wl.mouse_y);
        for (int b = 0; b < 5; ++b) io.MouseDown[b] = g_wl.mouse_buttons[b];

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_wl.width),
                                        static_cast<float>(g_wl.height)), ImGuiCond_Always);
        ImGui::Begin("##clip_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_MenuBar);

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Clear unpinned")) history.clear_unpinned();
                if (ImGui::MenuItem("Clear all..."))   show_confirm_clear = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save history")) history.save();
                if (ImGui::MenuItem("Quit")) g_running.store(false, std::memory_order_relaxed);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Title
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.5f, 1.0f, 1.0f));
        ImGui::Text("Clipboard History (%zu entries)", history.size());
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Search bar
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "Search...", search_buf, sizeof(search_buf));
        ImGui::Spacing();

        // Entry list
        const std::string filter(search_buf);
        auto entries = history.entries();

        ImGui::BeginChild("##entries", ImVec2(0, -28.0f), false);
        for (size_t i = 0; i < entries.size(); ++i) {
            const ClipEntry& e = entries[i];
            const std::string preview = e.preview(60);

            // Apply search filter
            if (!filter.empty()) {
                if (e.kind != EntryKind::Text) continue; // Can't search images
                std::string lc_preview = preview;
                std::string lc_filter  = filter;
                for (auto& c : lc_preview) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (auto& c : lc_filter)  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lc_preview.find(lc_filter) == std::string::npos) continue;
            }

            ImGui::PushID(static_cast<int>(i));

            // Pin indicator
            if (e.pinned) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.0f, 1.0f));
                ImGui::TextUnformatted("[PIN]");
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            // Entry text
            const bool is_img = (e.kind == EntryKind::Image);
            if (is_img) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
            if (ImGui::Selectable(preview.c_str(), false,
                                   ImGuiSelectableFlags_AllowDoubleClick)) {
                // Single-click: copy to clipboard (conceptually — we use a log here)
                // In a real Wayland app you'd set a wl_data_source; for now log intent
                SL_LOG_INFO("clipboard", "Selected entry {}: {}", i, preview);
            }
            if (is_img) ImGui::PopStyleColor();

            // Context menu
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (ImGui::MenuItem(e.pinned ? "Unpin" : "Pin"))
                    history.toggle_pin(i);
                if (ImGui::MenuItem("Remove"))
                    history.remove(i);
                ImGui::EndPopup();
            }

            if (ImGui::IsItemHovered() && e.kind == EntryKind::Text && e.text.size() > 60) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(400.0f);
                ImGui::TextUnformatted(e.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }
        ImGui::EndChild();

        // Bottom status
        ImGui::Separator();
        ImGui::Text("%zu entries  |  %s",
                    history.size(),
                    watcher.running() ? "Monitoring" : "Not monitoring");

        // Confirm clear dialog
        if (show_confirm_clear) { ImGui::OpenPopup("Confirm##clear"); show_confirm_clear = false; }
        if (ImGui::BeginPopupModal("Confirm##clear", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Clear ALL clipboard history (including pinned)?");
            ImGui::Spacing();
            if (ImGui::Button("Yes, clear all", ImVec2(140, 0))) {
                history.clear_all();
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

    watcher.stop();
    history.save();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    egl_shutdown();
    wayland_shutdown();
    return 0;
}
