// apps/audio_mixer/main.cpp
// StrayLight Audio Mixer — Wayland + EGL + ImGui PipeWire mixer UI
#include "mixer.h"
#include "pipewire_client.h"

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
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::mixer;

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Wayland / EGL boilerplate
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

    int   width      = 700;
    int   height     = 480;
    bool  configured = false;
    float mouse_x = 0.0f, mouse_y = 0.0f;
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
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
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
    xdg_toplevel_set_title(g_wl.toplevel,  "StrayLight Audio Mixer");
    xdg_toplevel_set_app_id(g_wl.toplevel, "straylight.audiomixer");
    wl_surface_commit(g_wl.surface);
    wl_display_roundtrip(g_wl.display);
    return true;
}

void wayland_shutdown() {
    if (g_wl.toplevel)   xdg_toplevel_destroy(g_wl.toplevel);
    if (g_wl.xdg_surf)   xdg_surface_destroy(g_wl.xdg_surf);
    if (g_wl.surface)    wl_surface_destroy(g_wl.surface);
    if (g_wl.pointer)    wl_pointer_destroy(g_wl.pointer);
    if (g_wl.keyboard)   wl_keyboard_destroy(g_wl.keyboard);
    if (g_wl.seat)       wl_seat_destroy(g_wl.seat);
    if (g_wl.xdg_wm)    xdg_wm_base_destroy(g_wl.xdg_wm);
    if (g_wl.compositor) wl_compositor_destroy(g_wl.compositor);
    if (g_wl.registry)   wl_registry_destroy(g_wl.registry);
    if (g_wl.display)    wl_display_disconnect(g_wl.display);
}

// ---------------------------------------------------------------------------
// Draw a vertical slider + peak meter for one audio node
// ---------------------------------------------------------------------------

void draw_channel_strip(AudioMixer& mixer, const PwNodeInfo& node,
                         float strip_w, float strip_h) {
    ImGui::PushID(static_cast<int>(node.id));
    ImGui::BeginGroup();

    const float slider_h  = strip_h - 60.0f;
    const float meter_w   = 8.0f;
    const float slider_w  = strip_w - meter_w - 6.0f;

    // Peak meter (drawn via draw list)
    ImVec2 meter_pos = ImGui::GetCursorScreenPos();
    meter_pos.x += slider_w + 4.0f;
    const float peak = mixer.get_peak(node.id);
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    // Background
    dl->AddRectFilled(meter_pos,
                      ImVec2(meter_pos.x + meter_w, meter_pos.y + slider_h),
                      IM_COL32(20, 20, 30, 220));
    // Filled level
    const float filled_h = peak * slider_h;
    const float fy0      = meter_pos.y + slider_h - filled_h;
    // Green→yellow→red gradient
    uint32_t meter_col = (peak < 0.6f) ? IM_COL32(40, 220, 80, 255)
                       : (peak < 0.85f) ? IM_COL32(220, 200, 30, 255)
                                        : IM_COL32(220, 40, 40, 255);
    dl->AddRectFilled(ImVec2(meter_pos.x, fy0),
                      ImVec2(meter_pos.x + meter_w, meter_pos.y + slider_h),
                      meter_col);

    // Vertical volume slider
    float vol = mixer.get_volume(node.id);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     ImVec4(0.80f, 0.10f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.10f, 0.10f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.16f, 0.16f, 0.22f, 1.0f));
    if (ImGui::VSliderFloat("##vol", ImVec2(slider_w, slider_h), &vol, 0.0f, 1.0f, "")) {
        mixer.set_volume(node.id, vol);
    }
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.0f%%", vol * 100.0f);
    }

    ImGui::Spacing();

    // Mute button
    bool muted = mixer.get_muted(node.id);
    if (muted)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.05f, 0.05f, 1.0f));
    if (ImGui::Button(muted ? "M##mute" : " ##mute", ImVec2(strip_w, 20))) {
        mixer.set_mute(node.id, !muted);
    }
    if (muted) ImGui::PopStyleColor();

    // Volume percentage
    ImGui::SetNextItemWidth(strip_w);
    char pct_buf[8];
    std::snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", vol * 100.0f);
    ImGui::TextUnformatted(pct_buf);

    // Application name (truncated)
    const std::string& label_src = node.nick.empty() ? node.name : node.nick;
    const std::string  label = label_src.size() > 9
        ? label_src.substr(0, 7) + ".."
        : label_src;
    ImGui::TextUnformatted(label.c_str());

    ImGui::EndGroup();
    ImGui::PopID();
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    PipeWireClient::pw_init_once(&argc, &argv);

    if (!wayland_init()) { SL_ERROR("mixer: Wayland init failed"); return 1; }
    if (!egl_init())     { SL_ERROR("mixer: EGL init failed");     return 1; }

    while (!g_wl.configured) wl_display_dispatch(g_wl.display);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_wl.width), static_cast<float>(g_wl.height));
    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.07f, 0.07f, 0.09f, 0.97f);
    colors[ImGuiCol_TitleBgActive]  = ImVec4(0.20f, 0.0f,  0.35f, 1.0f);
    colors[ImGuiCol_Button]         = ImVec4(0.14f, 0.0f,  0.22f, 1.0f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.28f, 0.0f,  0.48f, 1.0f);

    ImGui_ImplOpenGL3_Init("#version 300 es");

    // PipeWire client + mixer
    PipeWireClient pw_client;
    {
        auto res = pw_client.connect();
        if (!res.has_value()) {
            SL_WARN("mixer: PipeWire connect failed: {}", res.error().message());
        }
    }
    AudioMixer mixer(pw_client);
    // Initial node sync (PipeWire events may not have fired yet)
    pw_client.nodes(); // trigger registry roundtrip
    mixer.sync_nodes();

    int selected_sink_idx = 0;

    // ---------------------------------------------------------------------------
    // Main loop
    // ---------------------------------------------------------------------------
    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(g_wl.display);
        if (wl_display_flush(g_wl.display) < 0) break;

        mixer.update_meters();
        mixer.sync_nodes();

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
        ImGui::Begin("##mixer_root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_MenuBar);

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.4f, 1.0f, 1.0f));
            ImGui::TextUnformatted("StrayLight Audio Mixer");
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 20);
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Quit"))
                    g_running.store(false, std::memory_order_relaxed);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float avail_h = ImGui::GetContentRegionAvail().y;

        // Output device selector
        {
            const auto sink_names = mixer.sink_names();
            const auto sink_ids   = mixer.sink_ids();

            ImGui::Text("Output Device:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(250.0f);

            // Build combo items string
            std::string combo_items;
            for (const auto& s : sink_names) { combo_items += s; combo_items += '\0'; }
            combo_items += '\0';

            if (sink_names.empty()) {
                ImGui::TextDisabled("(no sinks detected)");
            } else {
                if (selected_sink_idx >= static_cast<int>(sink_names.size()))
                    selected_sink_idx = 0;
                ImGui::Combo("##sink_sel", &selected_sink_idx, combo_items.c_str());
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Channel strips
        const auto& nodes = mixer.node_infos();
        if (nodes.empty()) {
            ImGui::SetCursorPosY(avail_h * 0.4f);
            ImGui::SetCursorPosX((avail_w - ImGui::CalcTextSize("No audio streams detected").x) * 0.5f);
            ImGui::TextDisabled("No audio streams detected");
            if (ImGui::Button("Refresh")) mixer.sync_nodes();
        } else {
            const float strip_h = avail_h - 80.0f;
            const float strip_w = std::max(55.0f,
                std::min(90.0f, (avail_w - 20.0f) / static_cast<float>(nodes.size())));

            ImGui::BeginChild("##strips", ImVec2(0, strip_h), false,
                               ImGuiWindowFlags_HorizontalScrollbar);
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (i > 0) ImGui::SameLine(0, 4);
                draw_channel_strip(mixer, nodes[i], strip_w, strip_h - 4.0f);
            }
            ImGui::EndChild();
        }

        // Bottom: master volume
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Master");
        ImGui::SameLine();
        // Master fader — controls the default sink if available
        const auto sink_ids = mixer.sink_ids();
        if (!sink_ids.empty()) {
            float master_vol = mixer.get_volume(sink_ids[0]);
            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::SliderFloat("##master", &master_vol, 0.0f, 1.0f,
                                    "%.0f%%", ImGuiSliderFlags_None)) {
                mixer.set_volume(sink_ids[0], master_vol);
            }
            bool master_mute = mixer.get_muted(sink_ids[0]);
            ImGui::SameLine();
            if (ImGui::Checkbox("Mute##master", &master_mute))
                mixer.set_mute(sink_ids[0], master_mute);
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

    pw_client.disconnect();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    egl_shutdown();
    wayland_shutdown();
    return 0;
}
