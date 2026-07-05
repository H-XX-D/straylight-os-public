// apps/oobe/main.cpp
// StrayLight OOBE — first-boot out-of-box experience wizard
#include "oobe_state.h"
#include "pages/welcome.h"
#include "pages/account_setup.h"
#include "pages/package_profile.h"
#include "pages/network_config.h"
#include "pages/summary.h"

#include <straylight/log.h>

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
#include <fstream>
#include <string>

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

/// Read the boot state file.
std::string read_state_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string state;
    std::getline(f, state);
    return state;
}

/// Write the boot state file atomically.
void write_state_file(const std::string& path, const std::string& state) {
    std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    f << state;
    f.flush();
    f.close();
    std::rename(tmp.c_str(), path.c_str());
}

} // anonymous namespace

int main(int /*argc*/, char* /*argv*/[]) {
    using namespace straylight;
    using namespace straylight::oobe;

    Log::init("straylight-oobe");
    SL_INFO("StrayLight OOBE starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // Check boot state — only run if state is "oobe"
    const std::string state_path = "/var/lib/straylight/state";
    std::string boot_state = read_state_file(state_path);
    if (boot_state != "oobe") {
        SL_INFO("Boot state is '{}', not 'oobe' — exiting", boot_state);
        return EXIT_SUCCESS;
    }

    // Load OOBE progress (crash recovery)
    auto state_result = OobeState::load();
    if (!state_result.has_value()) {
        SL_ERROR("Failed to load OOBE state: {}",
                 state_result.error().message());
        return EXIT_FAILURE;
    }
    auto oobe_state = std::move(state_result).value();

    // Connect to Wayland
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        SL_CRITICAL("Failed to connect to Wayland display");
        return EXIT_FAILURE;
    }

    // Get compositor and create XDG shell surface
    // (simplified — full XDG shell binding omitted for brevity)
    wl_registry* registry = wl_display_get_registry(display);
    wl_display_roundtrip(display);

    // Create EGL context on a fullscreen surface
    auto egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(display));
    EGLint emajor, eminor;
    eglInitialize(egl_display, &emajor, &eminor);

    constexpr EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig egl_config;
    EGLint num_configs;
    eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_configs);

    // For now, use a placeholder surface (real impl creates XDG toplevel)
    constexpr int kWidth  = 1920;
    constexpr int kHeight = 1080;

    // Init ImGui
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth),
                            static_cast<float>(kHeight));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Instantiate pages
    WelcomePage        welcome_page;
    AccountSetupPage   account_page;
    PackageProfilePage package_page;
    NetworkConfigPage  network_page;
    SummaryPage        summary_page;

    SL_INFO("OOBE initialized, entering render loop at step: {}",
            step_to_string(oobe_state.current()));

    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        bool advance = false;
        OobeStep current = oobe_state.current();

        switch (current) {
            case OobeStep::kWelcome:
                advance = welcome_page.render();
                if (advance) {
                    oobe_state.advance(OobeStep::kAccount);
                    oobe_state.save();
                }
                break;

            case OobeStep::kAccount:
                advance = account_page.render();
                if (advance) {
                    oobe_state.advance(OobeStep::kPackageProfile);
                    oobe_state.save();
                }
                break;

            case OobeStep::kPackageProfile:
                advance = package_page.render();
                if (advance) {
                    oobe_state.advance(OobeStep::kNetwork);
                    oobe_state.save();
                }
                break;

            case OobeStep::kNetwork:
                advance = network_page.render();
                if (advance) {
                    oobe_state.advance(OobeStep::kSummary);
                    oobe_state.save();
                }
                break;

            case OobeStep::kSummary: {
                int result = summary_page.render();
                if (result == 1) {
                    // Apply and transition to wizard
                    oobe_state.advance(OobeStep::kDone);
                    oobe_state.save();

                    // Write boot state to "wizard"
                    write_state_file(state_path, "wizard");
                    SL_INFO("OOBE complete, state set to 'wizard'");
                    g_running.store(false);
                } else if (result == -1) {
                    oobe_state.advance(OobeStep::kNetwork);
                    oobe_state.save();
                }
                break;
            }

            case OobeStep::kDone:
                g_running.store(false);
                break;
        }

        ImGui::Render();
        glViewport(0, 0, kWidth, kHeight);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(egl_display, EGL_NO_SURFACE);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    eglTerminate(egl_display);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    SL_INFO("OOBE exited cleanly");
    return EXIT_SUCCESS;
}
