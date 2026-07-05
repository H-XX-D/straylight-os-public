// apps/oobe/pages/package_profile.cpp
// Package profile selection page
#include "package_profile.h"

#include <straylight/log.h>

#include <imgui.h>

namespace straylight::oobe {

std::vector<PackageProfile> PackageProfilePage::builtin_profiles() {
    return {
        {
            "ML Workstation",
            "Full ML development environment with GPU support, "
            "CUDA/ROCm toolkits, and pre-installed frameworks.",
            {"straylight-ml", "nvidia-driver", "cuda-toolkit",
             "python3-torch", "python3-jax", "jupyter-lab"},
            {}
        },
        {
            "Developer",
            "General-purpose development workstation with compilers, "
            "editors, containers, and debugging tools.",
            {"build-essential", "cmake", "gdb", "docker.io",
             "podman", "vim", "neovim", "git-lfs"},
            {"straylight-ml"}
        },
        {
            "Server",
            "Headless server profile — no desktop environment, "
            "minimal footprint with SSH and monitoring tools.",
            {"openssh-server", "htop", "tmux", "prometheus-node-exporter"},
            {"straylight-ml"}
        },
        {
            "Minimal",
            "Bare-minimum installation. Only core system packages.",
            {},
            {"straylight-ml", "vim", "nano"}
        },
    };
}

PackageProfilePage::PackageProfilePage()
    : profiles_(builtin_profiles()) {
}

bool PackageProfilePage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##OobePackageProfile", nullptr, flags);

    ImGui::SetCursorPosY(60.0f);
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Choose Your Profile");
    ImGui::Separator();
    ImGui::Spacing();

    // Profile cards
    for (int i = 0; i < static_cast<int>(profiles_.size()); ++i) {
        const auto& p = profiles_[static_cast<size_t>(i)];

        ImGui::SetCursorPosX(60.0f);

        bool is_selected = (i == selected_);
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
        }

        ImGui::BeginChild(p.name.c_str(), ImVec2(500, 80), true);
        ImGui::Text("%s", p.name.c_str());
        ImGui::TextWrapped("%s", p.description.c_str());
        ImGui::EndChild();

        if (is_selected) {
            ImGui::PopStyleColor();
        }

        if (ImGui::IsItemClicked()) {
            selected_ = i;
        }

        ImGui::Spacing();
    }

    // Installation progress
    if (installing_) {
        ImGui::SetCursorPosX(60.0f);
        ImGui::ProgressBar(progress_, ImVec2(500, 20));
        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("Installing packages...");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Buttons
    ImGui::SetCursorPosX(60.0f);
    bool advance = false;

    if (!installing_) {
        if (ImGui::Button("Apply & Continue", ImVec2(160, 40))) {
            if (selected_ >= 0) {
                const auto& profile =
                    profiles_[static_cast<size_t>(selected_)];
                SL_INFO("Applying profile: {}", profile.name);
                // TODO: Fork/exec apt-get with live progress
                // For now, simulate instant completion
                advance = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip", ImVec2(80, 40))) {
            selected_ = -1;
            advance = true;
        }
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::oobe
