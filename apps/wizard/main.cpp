// apps/wizard/main.cpp
// StrayLight wizard — post-login personalization
#include "firstboot.h"
#include "pages/theme_picker.h"
#include "pages/layout_config.h"
#include "pages/ide_setup.h"
#include "pages/ml_setup.h"
#include "pages/tour.h"
#include "pages/summary.h"

#include <straylight/log.h>

#include <wayland-client.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using straylight::wizard::TourChoice;

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    using namespace straylight;
    using namespace straylight::wizard;

    Log::init("straylight-wizard");
    SL_INFO("StrayLight Wizard starting");

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // Check for --force flag
    bool force_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--force") == 0) {
            force_mode = true;
        }
    }

    // Check boot state
    const std::string state_path = "/var/lib/straylight/state";
    if (!force_mode && !is_firstboot(state_path)) {
        std::string state = read_boot_state(state_path);
        SL_INFO("Boot state is '{}', not 'wizard' and --force not set — "
                "exiting", state);
        return EXIT_SUCCESS;
    }

    // Connect to Wayland
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        SL_CRITICAL("Failed to connect to Wayland display");
        return EXIT_FAILURE;
    }

    wl_registry* registry = wl_display_get_registry(display);
    wl_display_roundtrip(display);

    // Create EGL context (simplified — real impl creates XDG toplevel 800x600)
    auto egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(display));
    EGLint emajor, eminor;
    eglInitialize(egl_display, &emajor, &eminor);

    constexpr int kWidth  = 800;
    constexpr int kHeight = 600;

    // Init ImGui
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWidth),
                            static_cast<float>(kHeight));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Instantiate pages
    ThemePickerPage  theme_page;
    LayoutConfigPage layout_page;
    IdeSetupPage     ide_page;
    MlSetupPage      ml_page;
    TourPage         tour_page;
    SummaryPage      summary_page;

    enum class WizardStep {
        kTheme, kLayout, kIdeSetup, kMlSetup, kTour, kSummary, kDone
    };
    WizardStep step = WizardStep::kTheme;

    SL_INFO("Wizard initialized, entering render loop");

    while (g_running.load(std::memory_order_relaxed)) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        bool advance = false;

        switch (step) {
            case WizardStep::kTheme:
                advance = theme_page.render();
                if (advance) step = WizardStep::kLayout;
                break;

            case WizardStep::kLayout:
                advance = layout_page.render();
                if (advance) step = WizardStep::kIdeSetup;
                break;

            case WizardStep::kIdeSetup:
                advance = ide_page.render();
                if (advance) step = WizardStep::kMlSetup;
                break;

            case WizardStep::kMlSetup:
                advance = ml_page.render();
                if (advance) {
                    // Populate summary
                    summary_page.set_theme(theme_page.selected_theme());

                    std::string layout_desc = "top_bar";
                    if (layout_page.left_dock_enabled())
                        layout_desc += " + left_dock";
                    if (layout_page.bottom_dock_enabled())
                        layout_desc += " + bottom_dock";
                    summary_page.set_layout(layout_desc);
                    summary_page.set_gpu_profile(ml_page.gpu_profile());

                    step = WizardStep::kTour;
                }
                break;

            case WizardStep::kTour:
                advance = tour_page.render();
                if (advance) {
                    if (tour_page.choice() == TourChoice::kYes) {
                        // Launch tour helper asynchronously; wizard finishes first
                        if (fork() == 0) {
                            setsid();
                            execlp("straylight-tour", "straylight-tour", nullptr);
                            _exit(0);
                        }
                    }
                    step = WizardStep::kSummary;
                }
                break;

            case WizardStep::kSummary:
                advance = summary_page.render();
                if (advance) {
                    // Mark wizard complete
                    mark_complete(state_path);
                    step = WizardStep::kDone;
                    g_running.store(false);
                }
                break;

            case WizardStep::kDone:
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

    SL_INFO("Wizard exited cleanly");
    return EXIT_SUCCESS;
}
