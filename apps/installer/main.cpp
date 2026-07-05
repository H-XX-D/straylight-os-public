// apps/installer/main.cpp
// StrayLight Installer — disk selection + live install runner + reboot
//
// This app is launched from the live ISO when the user selects
// "Install StrayLight OS" from the GRUB menu (kernel param straylight.mode=install).
#include "disk_select.h"

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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <mutex>

namespace {

std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Wayland plumbing (identical pattern to boot-gui/main.cpp) ──────────────

struct WaylandState {
    wl_display*    display          = nullptr;
    wl_registry*   registry         = nullptr;
    wl_compositor* compositor       = nullptr;
    xdg_wm_base*   xdg_wm_base_ptr = nullptr;
    wl_surface*    surface          = nullptr;
    xdg_surface*   xdg_surface_ptr  = nullptr;
    xdg_toplevel*  toplevel         = nullptr;
    wl_egl_window* egl_window       = nullptr;
    int  width        = 1024;
    int  height       = 720;
    bool configured   = false;
    bool needs_resize = false;
};

void xdg_wm_base_ping_handler(void*, xdg_wm_base* b, uint32_t s) {
    xdg_wm_base_pong(b, s);
}
const xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping_handler
};

void xdg_surf_configure(void* d, xdg_surface* s, uint32_t serial) {
    static_cast<WaylandState*>(d)->configured = true;
    xdg_surface_ack_configure(s, serial);
}
const xdg_surface_listener xdg_surf_listener = { .configure = xdg_surf_configure };

void toplevel_configure(void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* ws = static_cast<WaylandState*>(d);
    if (w > 0 && h > 0 && (w != ws->width || h != ws->height)) {
        ws->width = w; ws->height = h; ws->needs_resize = true;
    }
}
void toplevel_close(void*, xdg_toplevel*) { g_running.store(false); }
void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener tl_listener = {
    .configure = toplevel_configure,
    .close     = toplevel_close,
    .configure_bounds     = toplevel_configure_bounds,
    .wm_capabilities      = toplevel_wm_capabilities,
};

void registry_global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t ver) {
    auto* ws = static_cast<WaylandState*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        ws->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface,
                             std::min(ver, 4u)));
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        ws->xdg_wm_base_ptr = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface,
                             std::min(ver, 2u)));
        xdg_wm_base_add_listener(ws->xdg_wm_base_ptr, &wm_base_listener, nullptr);
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove
};

// ── Install runner ─────────────────────────────────────────────────────────

struct InstallState {
    std::atomic<float>       progress{0.0f};
    std::string              status_msg;
    std::mutex               msg_mutex;
    std::atomic<bool>        done{false};
    std::atomic<bool>        success{false};

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lk(msg_mutex);
        status_msg = s;
    }
    std::string get_status() {
        std::lock_guard<std::mutex> lk(msg_mutex);
        return status_msg;
    }
};

/// Run the installation script in a background thread.
/// The script handles partitioning, formatting, decompressing the squashfs
/// rootfs, installing GRUB, and copying the machine config.
void run_install_thread(const std::string& target_dev, InstallState* state) {
    using namespace straylight;
    SL_INFO("Starting installation to {}", target_dev);

    auto step = [&](float pct, const std::string& msg) {
        state->progress.store(pct, std::memory_order_relaxed);
        state->set_status(msg);
        SL_INFO("[install {:.0f}%] {}", pct * 100.0f, msg);
    };

    // Each stage calls the shell install helper.  Arguments:
    //   straylight-install-helper <stage> <target>
    auto run_stage = [&](const char* stage) -> bool {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "/usr/lib/straylight/straylight-install-helper %s %s 2>&1",
                 stage, target_dev.c_str());
        FILE* p = popen(cmd, "r");
        if (!p) return false;
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) {
            state->set_status(std::string(buf));
        }
        int rc = pclose(p);
        return (rc == 0);
    };

    step(0.05f, "Preparing disk...");
    if (!run_stage("partition")) {
        state->set_status("Partitioning failed — check logs.");
        state->success.store(false);
        state->done.store(true);
        return;
    }

    step(0.15f, "Creating filesystems...");
    run_stage("format");

    step(0.25f, "Copying system files...");
    run_stage("rootfs");

    step(0.65f, "Configuring system...");
    run_stage("configure");

    step(0.80f, "Installing GRUB bootloader...");
    run_stage("bootloader");

    step(0.95f, "Applying GRUB disk scan config...");
    // Copy os-prober-enabled GRUB defaults
    run_stage("grub-config");

    step(1.00f, "Installation complete!");
    state->success.store(true);
    state->done.store(true);
}

// ── Install progress page ──────────────────────────────────────────────────

void render_install_progress(InstallState& st, bool& reboot_requested) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##progress",  nullptr,
                 ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoCollapse);

    ImGui::SetCursorPos(ImVec2(40.0f, 30.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT INSTALLER");
    ImGui::PopStyleColor();
    ImGui::Separator();

    float pct = st.progress.load(std::memory_order_relaxed);
    std::string msg = st.get_status();

    ImGui::SetCursorPos(ImVec2(40.0f, 100.0f));
    ImGui::TextUnformatted(msg.c_str());

    ImGui::SetCursorPos(ImVec2(40.0f, 140.0f));
    ImGui::ProgressBar(pct, ImVec2(io.DisplaySize.x - 80.0f, 24.0f));

    if (st.done.load(std::memory_order_relaxed)) {
        if (st.success.load()) {
            ImGui::SetCursorPos(ImVec2(40.0f, 200.0f));
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                               "Installation successful!");
            ImGui::SetCursorPos(ImVec2(40.0f, 250.0f));
            ImGui::TextDisabled("Remove the installation medium and reboot to"
                                " start StrayLight OS.");
            ImGui::SetCursorPos(ImVec2(40.0f, 300.0f));
            if (ImGui::Button("Reboot now", ImVec2(160, 40)))
                reboot_requested = true;
        } else {
            ImGui::SetCursorPos(ImVec2(40.0f, 200.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "Installation failed — see /var/log/straylight-installer.log");
        }
    }

    ImGui::End();
}

} // anonymous namespace

// ── main ──────────────────────────────────────────────────────────────────

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::installer;

    Log::init("straylight-installer");
    SL_INFO("StrayLight Installer starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // ── Wayland setup ────────────────────────────────────────────────────
    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) {
        SL_CRITICAL("Cannot connect to Wayland");
        return EXIT_FAILURE;
    }
    ws.registry = wl_display_get_registry(ws.display);
    wl_registry_add_listener(ws.registry, &registry_listener, &ws);
    wl_display_roundtrip(ws.display);

    ws.surface         = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surface_ptr = xdg_wm_base_get_xdg_surface(
        ws.xdg_wm_base_ptr, ws.surface);
    xdg_surface_add_listener(ws.xdg_surface_ptr, &xdg_surf_listener, &ws);
    ws.toplevel = xdg_surface_get_toplevel(ws.xdg_surface_ptr);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "StrayLight Installer");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-installer");

    ws.egl_window = wl_egl_window_create(ws.surface, ws.width, ws.height);

    // ── EGL setup ────────────────────────────────────────────────────────
    EGLDisplay egl_dpy = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint emaj, emin;
    eglInitialize(egl_dpy, &emaj, &emin);

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,     EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,    EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg;
    EGLint n_cfg;
    eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &n_cfg);

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, egl_cfg,
                                         EGL_NO_CONTEXT, ctx_attrs);
    EGLSurface egl_surf = eglCreateWindowSurface(
        egl_dpy, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_window), nullptr);
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    // Commit surface so compositor maps the window
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // ── ImGui setup ──────────────────────────────────────────────────────
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                            static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // ── App state ────────────────────────────────────────────────────────
    enum class Stage { kDiskSelect, kInstalling, kDone };
    Stage stage = Stage::kDiskSelect;

    DiskSelectPage disk_page;
    InstallState   install_state;
    bool           reboot_requested = false;

    SL_INFO("Installer ready — entering render loop");

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

        switch (stage) {
            case Stage::kDiskSelect: {
                bool confirmed = disk_page.render();
                if (confirmed && disk_page.selected()) {
                    std::string dev = disk_page.selected()->path;
                    SL_INFO("User confirmed install to: {}", dev);
                    stage = Stage::kInstalling;
                    std::thread([dev, &install_state]() {
                        run_install_thread(dev, &install_state);
                    }).detach();
                }
                break;
            }
            case Stage::kInstalling:
                render_install_progress(install_state, reboot_requested);
                if (reboot_requested) {
                    g_running.store(false);
                }
                break;
            case Stage::kDone:
                g_running.store(false);
                break;
        }

        ImGui::Render();
        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.04f, 0.04f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_dpy, egl_surf);
    }

    // ── Cleanup ──────────────────────────────────────────────────────────
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_dpy, egl_surf);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
    wl_egl_window_destroy(ws.egl_window);
    xdg_toplevel_destroy(ws.toplevel);
    xdg_surface_destroy(ws.xdg_surface_ptr);
    wl_surface_destroy(ws.surface);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);

    if (reboot_requested) {
        SL_INFO("Rebooting...");
        execlp("systemctl", "systemctl", "reboot", nullptr);
    }

    return EXIT_SUCCESS;
}
