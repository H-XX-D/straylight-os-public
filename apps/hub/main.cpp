// apps/hub/main.cpp
// StrayLight Hub — Central dashboard and control panel for StrayLight OS.
// Full ImGui app with Wayland + EGL rendering.

#include "dashboard.h"
#include "service_panel.h"
#include "gpu_panel.h"
#include "network_panel.h"

#include <straylight/log.h>
#include <straylight/imgui_theme.h>

#include <wayland-client.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

#include <xdg-shell-client-protocol.h>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

struct WaylandState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* xdg_wm_base_ptr = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdg_surface_ptr = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_egl_window* egl_window = nullptr;

    int width = 1400;
    int height = 900;
    bool configured = false;
    bool needs_resize = false;
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version);
void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

void xdg_wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* s = static_cast<WaylandState*>(data);
    xdg_surface_ack_configure(surface, serial);
    s->configured = true;
}

const xdg_surface_listener xdg_surf_listener = {
    .configure = xdg_surface_configure,
};

void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h,
                        wl_array*) {
    auto* s = static_cast<WaylandState*>(data);
    if (w > 0 && h > 0 && (w != s->width || h != s->height)) {
        s->width = w;
        s->height = h;
        s->needs_resize = true;
    }
}

void toplevel_close(void*, xdg_toplevel*) {
    g_running.store(false, std::memory_order_relaxed);
}

void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

const xdg_toplevel_listener tl_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

void seat_capabilities(void*, wl_seat*, uint32_t) {}
void seat_name(void*, wl_seat*, const char*) {}

const wl_seat_listener seat_listener_impl = {
    .capabilities = seat_capabilities,
    .name = seat_name,
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

} // anonymous namespace

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::hub;

    Log::init("straylight-hub");
    SL_INFO("StrayLight Hub starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    // Wayland connection
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

    ws.surface = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr,
                                                       ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Hub");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-sl-hub");
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // EGL
    auto egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major, minor;
    eglInitialize(egl_display, &major, &minor);

    constexpr EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };

    EGLConfig egl_config;
    EGLint num_configs;
    eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs);

    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);
    auto egl_surface = eglCreateWindowSurface(
        egl_display, egl_config,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);

    constexpr EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };
    auto egl_context = eglCreateContext(egl_display, egl_config,
                                        EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);

    // ImGui
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                            static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    auto active_theme = ui::apply_straylight_theme();

    // Panels
    Dashboard dashboard;
    ServicePanel service_panel;
    GpuPanel gpu_panel;
    NetworkPanel network_panel;

    auto theme_options = ui::available_themes();
    std::vector<const char*> theme_names;
    theme_names.reserve(theme_options.size());
    int selected_theme = 0;
    for (int i = 0; i < static_cast<int>(theme_options.size()); ++i) {
        theme_names.push_back(theme_options[i].name.c_str());
        if (theme_options[i].id == active_theme.id) selected_theme = i;
    }

    // Timing: 1Hz update
    auto last_update = std::chrono::steady_clock::now();
    constexpr auto update_interval = std::chrono::seconds(1);

    // Initial sample
    dashboard.sample();
    service_panel.refresh();
    gpu_panel.sample();
    network_panel.sample();

    SL_INFO("Hub initialized");

    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(ws.display);
        wl_display_flush(ws.display);

        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                                    static_cast<float>(ws.height));
        }

        // Update at 1Hz
        auto now = std::chrono::steady_clock::now();
        if (now - last_update >= update_interval) {
            last_update = now;
            dashboard.sample();
            gpu_panel.sample();
            network_panel.sample();
        }

        // Render
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);

        ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse;

        if (ImGui::Begin("Hub", nullptr, wflags)) {
            ImGui::TextColored(active_theme.accent, "STRAYLIGHT HUB");
            ImGui::SameLine();
            ImGui::TextColored(active_theme.muted, "Central Dashboard & Control Panel");
            ImGui::Separator();

            if (ImGui::BeginTabBar("HubTabs")) {
                if (ImGui::BeginTabItem("Dashboard")) {
                    dashboard.render();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Services")) {
                    service_panel.render();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("GPU")) {
                    gpu_panel.render();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Network")) {
                    network_panel.render();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Storage")) {
                    // Reuse dashboard's disk data
                    const auto& health = dashboard.health();
                    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f), "Storage Overview");
                    ImGui::Separator();

                    for (const auto& disk : health.disks) {
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, active_theme.panel);
                        ImGui::BeginChild(("disk_" + disk.mount_point).c_str(),
                                           ImVec2(0, 80), true);

                        ImGui::Text("%s (%s)", disk.mount_point.c_str(), disk.device.c_str());

                        float used_gb = static_cast<float>(disk.used_bytes) / (1024.0f * 1024.0f * 1024.0f);
                        float total_gb = static_cast<float>(disk.total_bytes) / (1024.0f * 1024.0f * 1024.0f);
                        float free_gb = total_gb - used_gb;

                        ImVec4 color;
                        if (disk.usage_pct >= 0.9f) color = ImVec4(1, 0.2f, 0.2f, 1);
                        else if (disk.usage_pct >= 0.7f) color = ImVec4(1, 0.8f, 0.2f, 1);
                        else color = ImVec4(0, 0.9f, 0.6f, 1);

                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
                        char overlay[64];
                        std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f GB (%.0f%%)",
                                      used_gb, total_gb, disk.usage_pct * 100.0f);
                        ImGui::ProgressBar(disk.usage_pct, ImVec2(-1, 20), overlay);
                        ImGui::PopStyleColor();

                        ImGui::Text("Free: %.1f GB", free_gb);

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                        ImGui::Spacing();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Alice")) {
                    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f), "Alice AI Assistant");
                    ImGui::Separator();

                    // Chat-style interface
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.1f, 1.0f));
                    ImGui::BeginChild("alice_chat", ImVec2(0, -60), true);

                    ImGui::TextWrapped("Alice is your AI-powered system health monitor. "
                                       "She continuously analyzes system telemetry, logs, and "
                                       "hardware status to detect issues before they become problems.");
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Show any active alerts from the dashboard
                    const auto& alerts = dashboard.health().recent_alerts;
                    if (alerts.empty()) {
                        ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f),
                                          "All systems nominal. No active alerts.");
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                          "%zu active alert(s):", alerts.size());
                        for (const auto& alert : alerts) {
                            ImVec4 acolor;
                            if (alert.severity == "critical") acolor = ImVec4(1, 0.2f, 0.2f, 1);
                            else if (alert.severity == "warning") acolor = ImVec4(1, 0.8f, 0.2f, 1);
                            else acolor = ImVec4(0.4f, 0.6f, 1.0f, 1);

                            ImGui::TextColored(acolor, "  [%s] %s: %s",
                                              alert.severity.c_str(),
                                              alert.category.c_str(),
                                              alert.title.c_str());
                            if (!alert.detail.empty()) {
                                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                                  "    %s", alert.detail.c_str());
                            }
                        }
                    }

                    ImGui::EndChild();
                    ImGui::PopStyleColor();

                    // Input area
                    static char query_buf[512]{};
                    ImGui::InputText("##alice_query", query_buf, sizeof(query_buf));
                    ImGui::SameLine();
                    if (ImGui::Button("Ask Alice")) {
                        // Would connect to Alice daemon via IPC
                        query_buf[0] = '\0';
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                      "(connects to straylight-alice daemon)");

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings")) {
                    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f), "Settings");
                    ImGui::Separator();

                    // Theme
                    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (!theme_options.empty()) {
                            if (ImGui::Combo("Theme", &selected_theme,
                                             theme_names.data(),
                                             static_cast<int>(theme_names.size()))) {
                                active_theme = theme_options[selected_theme];
                                ui::apply_theme(active_theme);
                                ui::persist_user_theme(active_theme.id);
                            }
                            ImGui::TextColored(active_theme.muted, "Active: %s", active_theme.id.c_str());
                        }

                        static float ui_scale = 1.0f;
                        ImGui::SliderFloat("UI Scale", &ui_scale, 0.5f, 2.0f);
                    }

                    // Autotune
                    if (ImGui::CollapsingHeader("Performance Profile")) {
                        static int profile_idx = 1;
                        const char* profiles[] = {"Power Save", "Balanced", "Performance", "Maximum"};
                        ImGui::Combo("Profile", &profile_idx, profiles, 4);

                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                          "(applies via straylight-autotune)");
                    }

                    // Keybinds
                    if (ImGui::CollapsingHeader("Keyboard Shortcuts")) {
                        ImGui::Text("Super+H        Open Hub");
                        ImGui::Text("Super+T        Open Terminal");
                        ImGui::Text("Super+F        Open File Manager");
                        ImGui::Text("Super+L        Lock Screen");
                        ImGui::Text("Super+Space    App Launcher");
                        ImGui::Text("Ctrl+Alt+Del   System Monitor");

                        if (ImGui::Button("Edit Keybinds...")) {
                            // Would open keybind editor
                        }
                    }

                    // Refresh intervals
                    if (ImGui::CollapsingHeader("Dashboard Settings")) {
                        static float refresh_rate = 1.0f;
                        ImGui::SliderFloat("Update Rate (Hz)", &refresh_rate, 0.1f, 5.0f);

                        static bool show_gpu_panel = true;
                        ImGui::Checkbox("Show GPU Panel", &show_gpu_panel);

                        static bool auto_refresh_services = true;
                        ImGui::Checkbox("Auto-refresh Services", &auto_refresh_services);
                    }

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(active_theme.bg.x, active_theme.bg.y, active_theme.bg.z, active_theme.bg.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);

        usleep(16000); // ~60fps
    }

    // Cleanup
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

    SL_INFO("Hub exited cleanly");
    return EXIT_SUCCESS;
}
