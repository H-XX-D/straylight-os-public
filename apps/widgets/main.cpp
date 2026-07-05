// apps/widgets/main.cpp
// Standalone widget launcher — opens any widget by name or lists all available.
//
// Usage:
//   straylight-widget-launcher                  # list all widgets
//   straylight-widget-launcher gpu_hud          # open GPU HUD
//   straylight-widget-launcher --all            # open all widgets
//
// On a running StrayLight desktop this is normally not invoked directly;
// widgets are loaded by the shell. This binary is for development/testing.

#include "widget_registry.h"

#ifndef STRAYLIGHT_WIDGET_LAUNCHER_HEADLESS
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef STRAYLIGHT_WIDGET_LAUNCHER_HEADLESS
static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
#endif

static void print_usage(const char* argv0) {
    std::printf("Usage: %s [widget_id | --all | --list]\n", argv0);
    std::printf("  --list   List all registered widgets and exit\n");
    std::printf("  --all    Open all widgets\n");
    std::printf("  <id>     Open a specific widget by ID\n");
}

int main(int argc, char** argv) {
    auto& registry = straylight::widgets::WidgetRegistry::instance();

    // Handle --list
    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        std::printf("Registered widgets (%zu):\n", registry.size());
        for (auto& entry : registry.entries()) {
            std::printf("  %-24s  %s  [%s]\n",
                        entry.id.c_str(),
                        entry.display_name.c_str(),
                        straylight::widgets::category_name(entry.category));
        }
        return 0;
    }

    if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    // Determine which widgets to open
    bool open_all = false;
    std::string target_id;
    if (argc >= 2) {
        if (std::strcmp(argv[1], "--all") == 0) {
            open_all = true;
        } else {
            target_id = argv[1];
            if (!registry.has(target_id)) {
                std::fprintf(stderr, "Error: Unknown widget '%s'\n", target_id.c_str());
                std::fprintf(stderr, "Use --list to see available widgets.\n");
                return 1;
            }
        }
    } else {
        // No args — list and exit
        print_usage(argv[0]);
        std::printf("\nAvailable widgets:\n");
        for (auto& entry : registry.entries()) {
            std::printf("  %-24s  %s\n", entry.id.c_str(), entry.display_name.c_str());
        }
        return 0;
    }

#ifdef STRAYLIGHT_WIDGET_LAUNCHER_HEADLESS
    std::fprintf(stderr,
                 "Widget registry is available, but GUI launching was built without GLFW/ImGui backend support.\n"
                 "Use --list to inspect registered widgets, or run widgets through the StrayLight shell/dashboard.\n");
    return 1;
#else

    // Create widget instances
    struct OpenWidget {
        std::unique_ptr<straylight::WidgetBase> widget;
        bool open = true;
    };
    std::vector<OpenWidget> open_widgets;

    if (open_all) {
        for (auto& entry : registry.entries()) {
            auto w = registry.create(entry.id);
            if (w) open_widgets.push_back({std::move(w), true});
        }
    } else {
        auto w = registry.create(target_id);
        if (w) open_widgets.push_back({std::move(w), true});
    }

    if (open_widgets.empty()) {
        std::fprintf(stderr, "No widgets to display.\n");
        return 1;
    }

    // Initialize GLFW + OpenGL + ImGui
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    std::string title = "StrayLight Widget Launcher";
    if (!target_id.empty()) {
        title = std::string("StrayLight: ") + open_widgets[0].widget->name();
    }

    GLFWwindow* window = glfwCreateWindow(1280, 800, title.c_str(), nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // V-Sync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.95f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Update and render each widget
        bool any_open = false;
        for (auto& ow : open_widgets) {
            if (!ow.open) continue;
            any_open = true;
            ow.widget->update();
            ow.widget->render(&ow.open);
        }

        // If all widgets closed, exit
        if (!any_open) break;

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.06f, 0.06f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    open_widgets.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
#endif
}
