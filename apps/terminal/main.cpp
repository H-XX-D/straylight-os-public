// apps/terminal/main.cpp
// StrayLight terminal emulator — Wayland + EGL + ImGui + PTY
#include "config.h"
#include "pty.h"
#include "renderer.h"
#include "vte.h"

#include <straylight/log.h>

#include <wayland-client.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <unistd.h>

// Wayland xdg-shell protocol
#include <xdg-shell-client-protocol.h>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

// Wayland globals
struct WaylandState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    xdg_wm_base* xdg_wm_base_ptr = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdg_surface_ptr = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_egl_window* egl_window = nullptr;

    int width = 1024;
    int height = 768;
    bool configured = false;
    bool needs_resize = false;

    // Input state
    std::string pending_input;
    uint32_t modifiers = 0;
};

// Forward declarations for Wayland listeners
void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version);
void registry_global_remove(void* data, wl_registry* registry, uint32_t name);

const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

void xdg_wm_base_ping(void* /*data*/, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* state = static_cast<WaylandState*>(data);
    xdg_surface_ack_configure(surface, serial);
    state->configured = true;
}

const xdg_surface_listener xdg_surface_listener_impl = {
    .configure = xdg_surface_configure,
};

void toplevel_configure(void* data, xdg_toplevel* /*tl*/,
                        int32_t width, int32_t height,
                        wl_array* /*states*/) {
    auto* state = static_cast<WaylandState*>(data);
    if (width > 0 && height > 0) {
        if (width != state->width || height != state->height) {
            state->width = width;
            state->height = height;
            state->needs_resize = true;
        }
    }
}

void toplevel_close(void* /*data*/, xdg_toplevel* /*tl*/) {
    g_running.store(false, std::memory_order_relaxed);
}

void toplevel_configure_bounds(void* /*data*/, xdg_toplevel* /*tl*/,
                               int32_t /*width*/, int32_t /*height*/) {}

void toplevel_wm_capabilities(void* /*data*/, xdg_toplevel* /*tl*/,
                              wl_array* /*caps*/) {}

const xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

// Keyboard handling
void keyboard_keymap(void* /*data*/, wl_keyboard* /*kb*/,
                     uint32_t /*format*/, int fd, uint32_t /*size*/) {
    close(fd); // We'd normally use libxkbcommon here
}

void keyboard_enter(void* /*data*/, wl_keyboard* /*kb*/,
                    uint32_t /*serial*/, wl_surface* /*surface*/,
                    wl_array* /*keys*/) {}

void keyboard_leave(void* /*data*/, wl_keyboard* /*kb*/,
                    uint32_t /*serial*/, wl_surface* /*surface*/) {}

void keyboard_key(void* data, wl_keyboard* /*kb*/, uint32_t /*serial*/,
                  uint32_t /*time*/, uint32_t key, uint32_t state_val) {
    auto* ws = static_cast<WaylandState*>(data);
    if (state_val != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    // Basic key translation (evdev keycodes)
    // In production, use libxkbcommon for proper keymap handling
    uint32_t linux_key = key + 8; // evdev to X11
    (void)linux_key;

    bool ctrl = (ws->modifiers & 4) != 0;

    // Map common keys to terminal sequences
    switch (key) {
    case 1: // ESC
        ws->pending_input += '\x1b';
        break;
    case 14: // Backspace
        ws->pending_input += '\x7f';
        break;
    case 15: // Tab
        ws->pending_input += '\t';
        break;
    case 28: // Enter
        ws->pending_input += '\r';
        break;
    case 103: // Up
        ws->pending_input += "\x1b[A";
        break;
    case 108: // Down
        ws->pending_input += "\x1b[B";
        break;
    case 106: // Right
        ws->pending_input += "\x1b[C";
        break;
    case 105: // Left
        ws->pending_input += "\x1b[D";
        break;
    case 110: // Home
        ws->pending_input += "\x1b[H";
        break;
    case 115: // End
        ws->pending_input += "\x1b[F";
        break;
    case 112: // Delete
        ws->pending_input += "\x1b[3~";
        break;
    case 104: // Page Up
        ws->pending_input += "\x1b[5~";
        break;
    case 109: // Page Down
        ws->pending_input += "\x1b[6~";
        break;
    default:
        // Map printable characters
        if (key >= 2 && key <= 11) {
            // Number keys 1-9, 0
            char c = (key == 11) ? '0' : static_cast<char>('1' + key - 2);
            if (ctrl) {
                // Ctrl+number not standard, ignore
            } else {
                ws->pending_input += c;
            }
        } else if (key >= 16 && key <= 25) {
            // q w e r t y u i o p
            static const char row1[] = "qwertyuiop";
            char c = row1[key - 16];
            if (ctrl) {
                ws->pending_input += static_cast<char>(c - 'a' + 1);
            } else {
                ws->pending_input += c;
            }
        } else if (key >= 30 && key <= 38) {
            // a s d f g h j k l
            static const char row2[] = "asdfghjkl";
            char c = row2[key - 30];
            if (ctrl) {
                ws->pending_input += static_cast<char>(c - 'a' + 1);
            } else {
                ws->pending_input += c;
            }
        } else if (key >= 44 && key <= 50) {
            // z x c v b n m
            static const char row3[] = "zxcvbnm";
            char c = row3[key - 44];
            if (ctrl) {
                ws->pending_input += static_cast<char>(c - 'a' + 1);
            } else {
                ws->pending_input += c;
            }
        } else if (key == 57) {
            // Space
            ws->pending_input += ' ';
        }
        break;
    }
}

void keyboard_modifiers(void* data, wl_keyboard* /*kb*/,
                        uint32_t /*serial*/, uint32_t mods_depressed,
                        uint32_t /*mods_latched*/, uint32_t /*mods_locked*/,
                        uint32_t /*group*/) {
    auto* ws = static_cast<WaylandState*>(data);
    ws->modifiers = mods_depressed;
}

void keyboard_repeat_info(void* /*data*/, wl_keyboard* /*kb*/,
                          int32_t /*rate*/, int32_t /*delay*/) {}

const wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// Seat capabilities
void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* ws = static_cast<WaylandState*>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ws->keyboard) {
        ws->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ws->keyboard, &keyboard_listener, ws);
    }
}

void seat_name(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/) {}

const wl_seat_listener seat_listener = {
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
        wl_seat_add_listener(ws->seat, &seat_listener, ws);
    }
}

void registry_global_remove(void* /*data*/, wl_registry* /*registry*/,
                            uint32_t /*name*/) {}

} // anonymous namespace

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::terminal;

    Log::init("straylight-terminal");
    SL_INFO("StrayLight Terminal starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    // Load configuration
    auto config = TerminalConfig::load_or_defaults();

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
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surface_listener_impl, &ws);

    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &toplevel_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Terminal");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-terminal");

    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // Set up EGL
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

    // Init ImGui
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                            static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Apply cyberpunk style
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.18f, config.opacity);

    // Create terminal components
    int term_cols = config.initial_cols;
    int term_rows = config.initial_rows;

    Vte vte(term_cols, term_rows);
    vte.set_max_scrollback(config.scrollback_lines);
    vte.set_default_fg(config.color_scheme.foreground);
    vte.set_default_bg(config.color_scheme.background);

    Renderer renderer;
    renderer.init(config.font_family, config.font_size);

    Pty pty;
    auto spawn_result = pty.spawn(term_cols, term_rows, config.shell);
    if (!spawn_result.has_value()) {
        SL_CRITICAL("Failed to spawn PTY: {}", spawn_result.error());
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        eglTerminate(egl_display);
        wl_display_disconnect(ws.display);
        return EXIT_FAILURE;
    }

    SL_INFO("Terminal initialized: {}x{}", term_cols, term_rows);

    // Read buffer
    char read_buf[4096];

    while (g_running.load(std::memory_order_relaxed) && pty.is_alive()) {
        // Poll Wayland + PTY
        struct pollfd fds[2];
        fds[0].fd = wl_display_get_fd(ws.display);
        fds[0].events = POLLIN;
        fds[1].fd = pty.master_fd();
        fds[1].events = POLLIN;

        wl_display_flush(ws.display);
        int ret = poll(fds, 2, 16); // ~60fps max
        (void)ret;

        // Dispatch Wayland events
        if (fds[0].revents & POLLIN) {
            wl_display_dispatch(ws.display);
        } else {
            wl_display_dispatch_pending(ws.display);
        }

        // Handle resize
        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_window, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                                    static_cast<float>(ws.height));

            // Recalculate terminal dimensions
            int new_cols = static_cast<int>(
                (ws.width - renderer.cell_width()) / renderer.cell_width());
            int new_rows = static_cast<int>(
                (ws.height - renderer.cell_height()) / renderer.cell_height());

            new_cols = std::max(new_cols, 2);
            new_rows = std::max(new_rows, 1);

            if (new_cols != term_cols || new_rows != term_rows) {
                term_cols = new_cols;
                term_rows = new_rows;
                vte.resize(term_cols, term_rows);
                pty.resize(term_cols, term_rows);
            }
        }

        // Read PTY output
        if (fds[1].revents & POLLIN) {
            auto read_result = pty.read(read_buf, sizeof(read_buf));
            if (read_result.has_value() && read_result.value() > 0) {
                vte.feed(read_buf, static_cast<size_t>(read_result.value()));
            }
        }

        // Send pending input to PTY
        if (!ws.pending_input.empty()) {
            pty.write(ws.pending_input.data(), ws.pending_input.size());
            ws.pending_input.clear();
            // Snap scroll to bottom on input
            renderer.set_scroll_offset(0);
        }

        // Render
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // Full-window terminal
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin("Terminal", nullptr, flags)) {
            // Handle mouse for selection and scrolling
            renderer.handle_mouse(vte, 0.0f, 0.0f);

            // Render terminal grid
            renderer.render(vte,
                           static_cast<float>(ws.width),
                           static_cast<float>(ws.height));

            // Copy selection to clipboard on Ctrl+Shift+C
            if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C)) {
                std::string sel = renderer.get_selection_text(vte);
                if (!sel.empty()) {
                    ImGui::SetClipboardText(sel.c_str());
                }
            }

            // Paste from clipboard on Ctrl+Shift+V
            if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V)) {
                const char* clipboard = ImGui::GetClipboardText();
                if (clipboard && clipboard[0] != '\0') {
                    pty.write(clipboard, strlen(clipboard));
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleVar(2);

        // Update title
        if (!vte.title().empty()) {
            xdg_toplevel_set_title(ws.toplevel, vte.title().c_str());
        }

        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.1f, 0.1f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);
    }

    // Cleanup
    pty.close();

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
    if (ws.keyboard) wl_keyboard_destroy(ws.keyboard);
    if (ws.seat) wl_seat_destroy(ws.seat);
    xdg_wm_base_destroy(ws.xdg_wm_base_ptr);
    wl_compositor_destroy(ws.compositor);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);

    SL_INFO("Terminal exited cleanly");
    return EXIT_SUCCESS;
}
