// apps/file_manager/main.cpp
// StrayLight file manager — Wayland + EGL + ImGui three-column browser
#include "bookmarks.h"
#include "browser.h"
#include "operations.h"
#include "preview.h"

#include <straylight/log.h>

#include <wayland-client.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <atomic>
#include <cstring>
#include <csignal>
#include <cstdlib>

// Wayland xdg-shell protocol
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

    int width = 1280;
    int height = 800;
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
    using namespace straylight::file_manager;

    Log::init("straylight-files");
    SL_INFO("StrayLight File Manager starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    // Connect to Wayland
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

    // Create surface
    ws.surface = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(ws.xdg_wm_base_ptr,
                                                       ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Files");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-files");
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // EGL setup
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

    // ImGui init
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                            static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Cyberpunk style
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.18f, 0.95f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.2f, 0.2f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.8f, 0.53f, 0.5f);

    // Initialize components
    Browser browser;
    Bookmarks bookmarks;
    Preview preview;

    bookmarks.load_or_defaults();
    browser.navigate(browser.current_path());

    // UI state
    static char filter_buf[256] = {};
    bool show_confirm_delete = false;
    static char rename_buf[256] = {};
    bool show_rename = false;

    // Async operation tracking
    std::vector<AsyncOperation> active_ops;

    SL_INFO("File Manager initialized at: {}",
            browser.current_path().string());

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

        // Full-window layout
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);

        ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_MenuBar;

        if (ImGui::Begin("FileManager", nullptr, wflags)) {
            // Menu bar
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("New File", "Ctrl+N")) {
                        auto r = Operations::create_file(
                            browser.current_path() / "untitled");
                        if (r.has_value()) browser.refresh();
                    }
                    if (ImGui::MenuItem("New Folder", "Ctrl+Shift+N")) {
                        auto r = Operations::create_directory(
                            browser.current_path() / "New Folder");
                        if (r.has_value()) browser.refresh();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
                        g_running.store(false);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    bool hidden = browser.show_hidden();
                    if (ImGui::Checkbox("Show Hidden Files", &hidden)) {
                        browser.set_show_hidden(hidden);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Sort by Name")) {
                        browser.set_sort(SortBy::Name, SortDir::Ascending);
                    }
                    if (ImGui::MenuItem("Sort by Size")) {
                        browser.set_sort(SortBy::Size, SortDir::Descending);
                    }
                    if (ImGui::MenuItem("Sort by Date")) {
                        browser.set_sort(SortBy::DateModified, SortDir::Descending);
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            // Toolbar: navigation + path + filter
            bool back_disabled = !browser.can_go_back();
            bool fwd_disabled = !browser.can_go_forward();

            if (back_disabled) ImGui::BeginDisabled();
            if (ImGui::Button("<")) browser.navigate_back();
            if (back_disabled) ImGui::EndDisabled();

            ImGui::SameLine();
            if (fwd_disabled) ImGui::BeginDisabled();
            if (ImGui::Button(">")) browser.navigate_forward();
            if (fwd_disabled) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("^")) browser.navigate_up();

            ImGui::SameLine();
            if (ImGui::Button("Refresh")) browser.refresh();

            // Breadcrumb path
            ImGui::SameLine();
            auto crumbs = browser.breadcrumbs();
            for (size_t i = 0; i < crumbs.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine(0, 0);
                    ImGui::TextDisabled("/");
                    ImGui::SameLine(0, 0);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(crumbs[i].first.c_str())) {
                    browser.navigate(crumbs[i].second);
                }
            }

            // Filter
            ImGui::SameLine(ImGui::GetWindowWidth() - 250);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Filter", filter_buf, sizeof(filter_buf))) {
                browser.set_filter(filter_buf);
            }

            ImGui::Separator();

            // Three-column layout
            float sidebar_width = 200.0f;
            float preview_width = 300.0f;
            float content_width = ImGui::GetContentRegionAvail().x -
                                   sidebar_width - preview_width - 10.0f;

            // Left sidebar: bookmarks
            if (ImGui::BeginChild("Sidebar", ImVec2(sidebar_width, 0), true)) {
                auto clicked = bookmarks.render();
                if (!clicked.empty()) {
                    browser.navigate(clicked);
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // Center: file list
            if (ImGui::BeginChild("FileList", ImVec2(content_width, 0), true)) {
                // Column headers
                ImGui::Columns(4, "fileColumns", true);
                ImGui::SetColumnWidth(0, content_width * 0.45f);
                ImGui::SetColumnWidth(1, content_width * 0.2f);
                ImGui::SetColumnWidth(2, content_width * 0.2f);
                ImGui::SetColumnWidth(3, content_width * 0.15f);

                // Sortable headers
                if (ImGui::Selectable("Name")) {
                    auto dir = (browser.sort_by() == SortBy::Name &&
                                browser.sort_dir() == SortDir::Ascending)
                                   ? SortDir::Descending : SortDir::Ascending;
                    browser.set_sort(SortBy::Name, dir);
                }
                ImGui::NextColumn();
                if (ImGui::Selectable("Size")) {
                    auto dir = (browser.sort_by() == SortBy::Size &&
                                browser.sort_dir() == SortDir::Descending)
                                   ? SortDir::Ascending : SortDir::Descending;
                    browser.set_sort(SortBy::Size, dir);
                }
                ImGui::NextColumn();
                if (ImGui::Selectable("Modified")) {
                    auto dir = (browser.sort_by() == SortBy::DateModified &&
                                browser.sort_dir() == SortDir::Descending)
                                   ? SortDir::Ascending : SortDir::Descending;
                    browser.set_sort(SortBy::DateModified, dir);
                }
                ImGui::NextColumn();
                ImGui::Text("Perms");
                ImGui::NextColumn();
                ImGui::Separator();

                // File entries
                const auto& entries = browser.entries();
                for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                    const auto& entry = entries[static_cast<size_t>(i)];
                    bool selected = (i == browser.selected_index());

                    // Icon + name
                    std::string label;
                    if (entry.is_directory) {
                        label = "[DIR] " + entry.name;
                    } else if (entry.is_symlink) {
                        label = "[LNK] " + entry.name;
                    } else {
                        label = "      " + entry.name;
                    }

                    if (ImGui::Selectable(label.c_str(), selected,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        browser.set_selected(i);
                        preview.generate(entry.full_path);

                        if (ImGui::IsMouseDoubleClicked(0)) {
                            if (entry.is_directory) {
                                browser.navigate(entry.full_path);
                                preview.clear();
                            }
                        }
                    }

                    // Context menu
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Open")) {
                            if (entry.is_directory) {
                                browser.navigate(entry.full_path);
                            }
                        }
                        if (ImGui::MenuItem("Rename")) {
                            strncpy(rename_buf, entry.name.c_str(),
                                    sizeof(rename_buf) - 1);
                            show_rename = true;
                        }
                        if (ImGui::MenuItem("Delete")) {
                            show_confirm_delete = true;
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Add to Bookmarks")) {
                            if (entry.is_directory) {
                                bookmarks.add(entry.name, entry.full_path);
                            }
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::NextColumn();
                    ImGui::Text("%s", entry.size_string().c_str());
                    ImGui::NextColumn();
                    ImGui::Text("%s", entry.time_string().c_str());
                    ImGui::NextColumn();
                    ImGui::Text("%s", entry.perms_string().c_str());
                    ImGui::NextColumn();
                }

                ImGui::Columns(1);
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // Right: preview panel
            if (ImGui::BeginChild("Preview", ImVec2(preview_width, 0), true)) {
                preview.render(preview_width - 10.0f,
                              ImGui::GetContentRegionAvail().y);
            }
            ImGui::EndChild();

            // Delete confirmation dialog
            if (show_confirm_delete) {
                ImGui::OpenPopup("Confirm Delete");
                show_confirm_delete = false;
            }
            if (ImGui::BeginPopupModal("Confirm Delete", nullptr,
                                        ImGuiWindowFlags_AlwaysAutoResize)) {
                auto* sel = browser.selected_entry();
                if (sel) {
                    ImGui::Text("Delete \"%s\"?", sel->name.c_str());
                    ImGui::Separator();
                    if (ImGui::Button("Delete", ImVec2(120, 0))) {
                        Operations::remove(sel->full_path);
                        browser.refresh();
                        preview.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }

            // Rename dialog
            if (show_rename) {
                ImGui::OpenPopup("Rename");
                show_rename = false;
            }
            if (ImGui::BeginPopupModal("Rename", nullptr,
                                        ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::InputText("New name", rename_buf, sizeof(rename_buf));
                if (ImGui::Button("Rename", ImVec2(120, 0))) {
                    auto* sel = browser.selected_entry();
                    if (sel) {
                        Operations::rename(sel->full_path, rename_buf);
                        browser.refresh();
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Clean up completed async ops
            active_ops.erase(
                std::remove_if(active_ops.begin(), active_ops.end(),
                               [](const AsyncOperation& op) {
                                   return op.is_done();
                               }),
                active_ops.end());

            // Status bar
            ImGui::Separator();
            ImGui::Text("%d items  |  %s",
                       static_cast<int>(browser.entries().size()),
                       browser.current_path().c_str());
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.1f, 0.1f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);
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

    SL_INFO("File Manager exited cleanly");
    return EXIT_SUCCESS;
}
