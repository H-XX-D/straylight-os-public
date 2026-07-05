// apps/encryption/ui.cpp
// EncryptionApp — full Wayland+EGL+ImGui render loop
#include "ui.h"

#include <straylight/log.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <xdg-shell-client-protocol.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <future>

namespace straylight::encryption {

// ---------------------------------------------------------------------------
// Wayland / EGL plumbing (identical structure to existing apps)
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

struct WaylandState {
    wl_display*    display   = nullptr;
    wl_registry*   registry  = nullptr;
    wl_compositor* compositor = nullptr;
    wl_seat*       seat      = nullptr;
    xdg_wm_base*   xdg_wm   = nullptr;
    wl_surface*    surface   = nullptr;
    xdg_surface*   xdg_surf  = nullptr;
    xdg_toplevel*  toplevel  = nullptr;
    wl_egl_window* egl_win   = nullptr;
    int width = 900, height = 640;
    bool configured = false;
    bool needs_resize = false;
};

void xdg_wm_base_ping_handler(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_listener = { .ping = xdg_wm_base_ping_handler };

void xdg_surface_configure_handler(void* d, xdg_surface* surf, uint32_t serial) {
    xdg_surface_ack_configure(surf, serial);
    static_cast<WaylandState*>(d)->configured = true;
}
const xdg_surface_listener surf_listener = { .configure = xdg_surface_configure_handler };

void toplevel_configure_handler(void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* s = static_cast<WaylandState*>(d);
    if (w > 0 && h > 0 && (w != s->width || h != s->height)) {
        s->width = w; s->height = h; s->needs_resize = true;
    }
}
void toplevel_close_handler(void*, xdg_toplevel*) {
    g_running.store(false, std::memory_order_relaxed);
}
void toplevel_configure_bounds_handler(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities_handler(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener tl_listener = {
    .configure        = toplevel_configure_handler,
    .close            = toplevel_close_handler,
    .configure_bounds = toplevel_configure_bounds_handler,
    .wm_capabilities  = toplevel_wm_capabilities_handler,
};

void registry_global(void* d, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t ver) {
    auto* s = static_cast<WaylandState*>(d);
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        s->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(ver, 4u)));
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        s->xdg_wm = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, std::min(ver, 2u)));
        xdg_wm_base_add_listener(s->xdg_wm, &wm_listener, s);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        s->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(ver, 5u)));
    }
}
void registry_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener reg_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

} // namespace

// ---------------------------------------------------------------------------
// UI rendering helpers
// ---------------------------------------------------------------------------

void EncryptionApp::apply_style() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 4.f; st.FrameRounding = 3.f;
    st.Colors[ImGuiCol_WindowBg]       = ImVec4(0.10f, 0.10f, 0.18f, 0.96f);
    st.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.15f, 0.05f, 0.35f, 1.00f);
    st.Colors[ImGuiCol_Button]         = ImVec4(0.20f, 0.10f, 0.45f, 1.00f);
    st.Colors[ImGuiCol_ButtonHovered]  = ImVec4(0.00f, 0.80f, 0.53f, 0.70f);
    st.Colors[ImGuiCol_Header]         = ImVec4(0.20f, 0.20f, 0.35f, 1.00f);
    st.Colors[ImGuiCol_HeaderHovered]  = ImVec4(0.00f, 0.80f, 0.53f, 0.50f);
    st.Colors[ImGuiCol_FrameBg]        = ImVec4(0.15f, 0.15f, 0.25f, 1.00f);
    st.Colors[ImGuiCol_Tab]            = ImVec4(0.15f, 0.05f, 0.35f, 1.00f);
    st.Colors[ImGuiCol_TabActive]      = ImVec4(0.30f, 0.10f, 0.60f, 1.00f);
    st.Colors[ImGuiCol_TabHovered]     = ImVec4(0.00f, 0.80f, 0.53f, 0.70f);
}

void EncryptionApp::render_unlock() {
    ImGui::TextColored({0.f, 0.8f, 0.53f, 1.f}, "Encryption Manager");
    ImGui::Separator();
    ImGui::Text("Enter master passphrase to unlock keyring:");
    ImGui::SetNextItemWidth(320.f);
    ImGui::InputText("##master", master_buf_, sizeof(master_buf_),
                     ImGuiInputTextFlags_Password);

    if (ImGui::Button("Unlock", {120.f, 0.f})) {
        auto r = keyring_.load(std::string_view(master_buf_));
        if (r.has_value()) {
            status_ = "Keyring unlocked.";
        } else {
            status_ = "Error: " + r.error().message();
        }
        sodium_memzero(master_buf_, sizeof(master_buf_));
    }

    if (!status_.empty()) {
        ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", status_.c_str());
    }
}

void EncryptionApp::render_keys() {
    const auto& entries = keyring_.entries();

    // Table of keys
    if (ImGui::BeginTable("keys_tbl", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY, ImVec2(0, 200.f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Description");
        ImGui::TableSetupColumn("Created");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const auto& e = entries[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool sel = (selected_key_ == i);
            if (ImGui::Selectable(e.name.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selected_key_ = i;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.description.c_str());
            ImGui::TableSetColumnIndex(2);
            // Format creation timestamp
            auto t = std::chrono::system_clock::to_time_t(e.created);
            char ts[32]; std::strftime(ts, sizeof(ts), "%Y-%m-%d", std::localtime(&t));
            ImGui::TextUnformatted(ts);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Add New Key:");
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputText("Name##kn",   name_buf_, sizeof(name_buf_));
    ImGui::SetNextItemWidth(300.f);
    ImGui::InputText("Desc##kd",   desc_buf_, sizeof(desc_buf_));
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputText("Pass##kp",   pass_buf_, sizeof(pass_buf_),
                     ImGuiInputTextFlags_Password);
    if (ImGui::Button("Add Key")) {
        auto r = keyring_.add(name_buf_, desc_buf_, pass_buf_);
        if (r.has_value()) {
            auto s = keyring_.save();
            status_ = s.has_value() ? "Key added." : "Save error: " + s.error().message();
        } else {
            status_ = "Error: " + r.error().message();
        }
        std::memset(name_buf_, 0, sizeof(name_buf_));
        std::memset(desc_buf_, 0, sizeof(desc_buf_));
        sodium_memzero(pass_buf_, sizeof(pass_buf_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove Selected") && selected_key_ >= 0 &&
        selected_key_ < static_cast<int>(entries.size())) {
        auto r = keyring_.remove(entries[static_cast<size_t>(selected_key_)].name);
        if (r.has_value()) {
            keyring_.save();
            selected_key_ = -1;
            status_ = "Key removed.";
        } else {
            status_ = "Error: " + r.error().message();
        }
    }
}

void EncryptionApp::render_encrypt() {
    ImGui::Text("Encrypt a file:");
    ImGui::SetNextItemWidth(400.f);
    ImGui::InputText("Input file##ei",  in_path_,  sizeof(in_path_));
    ImGui::SetNextItemWidth(400.f);
    ImGui::InputText("Output file##eo", out_path_, sizeof(out_path_));

    // Key selector
    const auto& entries = keyring_.entries();
    std::string cur_name = (selected_key_ >= 0 &&
                            selected_key_ < static_cast<int>(entries.size()))
                           ? entries[static_cast<size_t>(selected_key_)].name
                           : "(select key)";

    if (ImGui::BeginCombo("Key##ek", cur_name.c_str())) {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            bool s = (selected_key_ == i);
            if (ImGui::Selectable(entries[static_cast<size_t>(i)].name.c_str(), s))
                selected_key_ = i;
        }
        ImGui::EndCombo();
    }

    bool busy = active_op_.valid() &&
        active_op_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;

    if (!busy && ImGui::Button("Encrypt File")) {
        if (selected_key_ < 0) {
            status_ = "Select a key first.";
        } else {
            auto dk_res = keyring_.unlock(entries[static_cast<size_t>(selected_key_)].name);
            if (!dk_res.has_value()) {
                status_ = "Error: " + dk_res.error().message();
            } else {
                DerivedKey dk = dk_res.value();
                std::string in  = in_path_;
                std::string out = out_path_;
                progress_ = 0.f;
                active_op_ = std::async(std::launch::async, [this, dk, in, out]()
                    -> Result<void, SLError> {
                    return Crypto::encrypt_file(in, out, dk,
                        [this](uint64_t done, uint64_t total) {
                            if (total > 0) progress_ = static_cast<float>(done) / static_cast<float>(total);
                        });
                });
                status_ = "Encrypting...";
            }
        }
    }

    if (active_op_.valid()) {
        if (active_op_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto r = active_op_.get();
            status_ = r.has_value() ? "Encryption complete." : "Error: " + r.error().message();
            progress_ = 0.f;
        } else {
            ImGui::ProgressBar(progress_, ImVec2(-1.f, 0.f));
        }
    }
}

void EncryptionApp::render_decrypt() {
    ImGui::Text("Decrypt a file:");
    ImGui::SetNextItemWidth(400.f);
    ImGui::InputText("Encrypted file##di",  in_path_,  sizeof(in_path_));
    ImGui::SetNextItemWidth(400.f);
    ImGui::InputText("Output file##do",     out_path_, sizeof(out_path_));
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputText("Passphrase##dp",      pass_buf_, sizeof(pass_buf_),
                     ImGuiInputTextFlags_Password);

    bool busy = active_op_.valid() &&
        active_op_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;

    if (!busy && ImGui::Button("Decrypt File")) {
        std::string in   = in_path_;
        std::string out  = out_path_;
        std::string pass = pass_buf_;
        progress_ = 0.f;
        active_op_ = std::async(std::launch::async, [this, in, out, pass]()
            -> Result<void, SLError> {
            return Crypto::decrypt_file(in, out, pass,
                [this](uint64_t done, uint64_t total) {
                    if (total > 0) progress_ = static_cast<float>(done) / static_cast<float>(total);
                });
        });
        sodium_memzero(pass_buf_, sizeof(pass_buf_));
        status_ = "Decrypting...";
    }

    if (active_op_.valid()) {
        if (active_op_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto r = active_op_.get();
            status_ = r.has_value() ? "Decryption complete." : "Error: " + r.error().message();
            progress_ = 0.f;
        } else {
            ImGui::ProgressBar(progress_, ImVec2(-1.f, 0.f));
        }
    }
}

// ---------------------------------------------------------------------------
// Main render frame
// ---------------------------------------------------------------------------
static void render_frame(EncryptionApp& app) {
    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoCollapse;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0.f, 0.f});
    ImGui::SetNextWindowSize(io.DisplaySize);

    if (ImGui::Begin("EncryptionManager", nullptr, wflags)) {
        ImGui::TextColored({0.6f, 0.4f, 1.f, 1.f}, "Encryption Manager");
        ImGui::SameLine(ImGui::GetWindowWidth() - 120.f);
        if (!app.keyring_.is_unlocked()) {
            ImGui::TextDisabled("[locked]");
        } else {
            ImGui::TextColored({0.f, 0.8f, 0.4f, 1.f}, "[unlocked]");
        }
        ImGui::Separator();

        if (!app.keyring_.is_unlocked()) {
            app.render_unlock();
        } else {
            if (ImGui::BeginTabBar("EncTabs")) {
                if (ImGui::BeginTabItem("Keys"))    { app.render_keys();    ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Encrypt")) { app.render_encrypt(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Decrypt")) { app.render_decrypt(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }

        if (!app.status_.empty()) {
            ImGui::Separator();
            ImGui::TextColored({1.f, 0.8f, 0.f, 1.f}, "%s", app.status_.c_str());
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// run() — full application lifecycle
// ---------------------------------------------------------------------------
int EncryptionApp::run(int /*argc*/, char* /*argv*/[]) {
    if (auto r = Crypto::init(); !r.has_value()) {
        return EXIT_FAILURE;
    }

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    WaylandState ws;
    ws.display = wl_display_connect(nullptr);
    if (!ws.display) return EXIT_FAILURE;

    ws.registry = wl_display_get_registry(ws.display);
    wl_registry_add_listener(ws.registry, &reg_listener, &ws);
    wl_display_roundtrip(ws.display);

    if (!ws.compositor || !ws.xdg_wm) {
        wl_display_disconnect(ws.display);
        return EXIT_FAILURE;
    }

    ws.surface   = wl_compositor_create_surface(ws.compositor);
    ws.xdg_surf  = xdg_wm_base_get_xdg_surface(ws.xdg_wm, ws.surface);
    xdg_surface_add_listener(ws.xdg_surf, &surf_listener, &ws);
    ws.toplevel  = xdg_surface_get_toplevel(ws.xdg_surf);
    xdg_toplevel_add_listener(ws.toplevel, &tl_listener, &ws);
    xdg_toplevel_set_title(ws.toplevel, "Encryption Manager");
    xdg_toplevel_set_app_id(ws.toplevel, "straylight-encryption");
    wl_surface_commit(ws.surface);
    wl_display_roundtrip(ws.display);

    // EGL
    auto egl_dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(ws.display));
    EGLint major = 0, minor = 0;
    eglInitialize(egl_dpy, &major, &minor);

    constexpr EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig egl_cfg{}; EGLint ncfg = 0;
    eglChooseConfig(egl_dpy, cfg_attrs, &egl_cfg, 1, &ncfg);

    ws.egl_win = wl_egl_window_create(ws.surface, ws.width, ws.height);
    auto egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg,
        reinterpret_cast<EGLNativeWindowType>(ws.egl_win), nullptr);

    constexpr EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE
    };
    auto egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attrs);
    eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);

    // ImGui
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(ws.width), static_cast<float>(ws.height));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");
    apply_style();

    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(ws.display);
        wl_display_flush(ws.display);

        if (ws.needs_resize) {
            ws.needs_resize = false;
            wl_egl_window_resize(ws.egl_win, ws.width, ws.height, 0, 0);
            io.DisplaySize = ImVec2(static_cast<float>(ws.width),
                                    static_cast<float>(ws.height));
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        render_frame(*this);
        ImGui::Render();

        glViewport(0, 0, ws.width, ws.height);
        glClearColor(0.08f, 0.08f, 0.14f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_dpy, egl_surf);
    }

    // Cleanup
    if (active_op_.valid()) active_op_.wait();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(egl_dpy, egl_surf);
    eglDestroyContext(egl_dpy, egl_ctx);
    eglTerminate(egl_dpy);
    wl_egl_window_destroy(ws.egl_win);
    xdg_toplevel_destroy(ws.toplevel);
    xdg_surface_destroy(ws.xdg_surf);
    wl_surface_destroy(ws.surface);
    if (ws.seat) wl_seat_destroy(ws.seat);
    xdg_wm_base_destroy(ws.xdg_wm);
    wl_compositor_destroy(ws.compositor);
    wl_registry_destroy(ws.registry);
    wl_display_disconnect(ws.display);
    return EXIT_SUCCESS;
}

} // namespace straylight::encryption
