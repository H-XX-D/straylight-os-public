// apps/greeter/main.cpp
// StrayLight greeter — session-lock login screen with PAM authentication
#include "auth.h"
#include "session.h"
#include "ui.h"

#include <straylight/log.h>
#include <straylight/config.h>

#include <wayland-client.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

/// Launch the user session after successful authentication.
void launch_session(const std::string& username,
                    const std::string& session_type) {
    struct passwd* pw = getpwnam(username.c_str());
    if (!pw) {
        SL_CRITICAL("User '{}' not found in passwd", username);
        return;
    }

    // Set environment for the session
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);
    setenv("XDG_RUNTIME_DIR",
           (std::string("/run/user/") + std::to_string(pw->pw_uid)).c_str(),
           1);

    // Set UID/GID
    setgid(pw->pw_gid);
    setuid(pw->pw_uid);

    if (session_type == "tty") {
        SL_INFO("Launching TTY session for '{}'", username);
        execl(pw->pw_shell, pw->pw_shell, nullptr);
    } else {
        SL_INFO("Launching StrayLight session for '{}'", username);
        execl("/usr/bin/straylight-session", "straylight-session", nullptr);
    }

    // exec failed
    SL_CRITICAL("exec failed for session");
    _exit(EXIT_FAILURE);
}

} // anonymous namespace

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::greeter;

    Log::init("straylight-greeter");
    SL_INFO("StrayLight Greeter starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // Connect to Wayland
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        SL_CRITICAL("Failed to connect to Wayland display");
        return EXIT_FAILURE;
    }

    // Acquire session lock
    auto lock_result = SessionLock::acquire(display);
    if (!lock_result.has_value()) {
        SL_CRITICAL("Failed to acquire session lock: {}",
                    lock_result.error().message());
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }
    auto session_lock = std::move(lock_result).value();

    // Set up EGL on the lock surface
    auto egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(display));
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

    auto* egl_window = wl_egl_window_create(
        session_lock.surface(),
        session_lock.width(), session_lock.height());

    auto egl_surface = eglCreateWindowSurface(
        egl_display, egl_config,
        reinterpret_cast<EGLNativeWindowType>(egl_window), nullptr);

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
    io.DisplaySize = ImVec2(static_cast<float>(session_lock.width()),
                            static_cast<float>(session_lock.height()));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Create auth and UI
    PamAuth pam_auth;
    GreeterUI greeter_ui;

    SL_INFO("Greeter initialized, entering render loop");

    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        bool submitted = greeter_ui.render();

        ImGui::Render();
        glViewport(0, 0, session_lock.width(), session_lock.height());
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, egl_surface);

        if (submitted) {
            greeter_ui.clear_error();

            auto result = pam_auth.authenticate(
                greeter_ui.username(), greeter_ui.password());

            if (result.has_value()) {
                SL_INFO("Authentication succeeded, launching session");
                session_lock.unlock_and_destroy();

                // Cleanup EGL
                ImGui_ImplOpenGL3_Shutdown();
                ImGui::DestroyContext();
                eglMakeCurrent(egl_display, EGL_NO_SURFACE,
                               EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(egl_display, egl_surface);
                eglDestroyContext(egl_display, egl_context);
                eglTerminate(egl_display);
                wl_egl_window_destroy(egl_window);
                wl_display_disconnect(display);

                // Launch user session (does not return)
                launch_session(greeter_ui.username(),
                               greeter_ui.selected_session());
                return EXIT_FAILURE;  // exec failed
            } else {
                greeter_ui.set_error(result.error().message());
            }
        }
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    session_lock.unlock_and_destroy();
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglDestroySurface(egl_display, egl_surface);
    eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);
    wl_egl_window_destroy(egl_window);
    wl_display_disconnect(display);

    SL_INFO("Greeter exited cleanly");
    return EXIT_SUCCESS;
}
