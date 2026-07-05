// apps/wizard/pages/ml_setup.cpp
// ML setup page implementation
#include "ml_setup.h"

#include <straylight/log.h>

#include <imgui.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace straylight::wizard {

namespace fs = std::filesystem;

static const char* kGpuProfiles[] = {"performance", "balanced", "power-save"};
static constexpr int kNumProfiles = 3;

/// Check if a Python import succeeds within a timeout.
static bool check_python_import(const char* module, int timeout_s = 2) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: exec python3 -c "import <module>"
        std::string cmd = std::string("import ") + module;
        execlp("python3", "python3", "-c", cmd.c_str(), nullptr);
        _exit(127);
    } else if (pid < 0) {
        return false;
    }

    // Parent: wait with timeout
    for (int i = 0; i < timeout_s * 10; ++i) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        usleep(100000);  // 100ms
    }

    // Timeout — kill child
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

MlSetupPage::MlSetupPage() {
    frameworks_ = {
        {"PyTorch", false},
        {"JAX", false},
        {"TensorFlow", false},
    };
}

// ── Storage drive enumeration ─────────────────────────────────────────────

void MlSetupPage::enumerate_storage() {
    storage_drives_.clear();

    // Always offer the home directory as the default option
    const char* home_env = std::getenv("HOME");
    std::string home_dir = home_env ? home_env : "/home";
    StorageDrive home_opt;
    home_opt.path    = home_dir + "/ml-data";
    home_opt.label   = "Home directory  (" + home_dir + "/ml-data)";
    home_opt.size    = "";
    home_opt.mounted = true;
    home_opt.mountpt = home_dir;
    storage_drives_.push_back(std::move(home_opt));

    // Enumerate additional block devices via lsblk
    FILE* pipe = popen(
        "lsblk -J -b -o NAME,SIZE,TYPE,MOUNTPOINT,LABEL,MODEL 2>/dev/null",
        "r");
    if (!pipe) { storage_enumerated_ = true; return; }

    std::string raw;
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        raw += buf.data();
    pclose(pipe);

    // Simple JSON walk — only include large (>= 20 GiB) non-boot disks
    // that aren't the root / home partition already.
    try {
#ifndef NLOHMANN_JSON_INCLUDED
        // Fallback: skip JSON parse if nlohmann not linked (header not found)
        (void)raw;
#else
        auto root = nlohmann::json::parse(raw);
        for (const auto& dev : root.at("blockdevices")) {
            std::string type = dev.value("type", std::string{});
            if (type != "disk" && type != "part") continue;

            uint64_t bytes = 0;
            try {
                auto sv = dev["size"];
                if (sv.is_number()) bytes = sv.get<uint64_t>();
                else if (sv.is_string()) bytes = std::stoull(sv.get<std::string>());
            } catch (...) {}
            if (bytes < 20ULL * 1024 * 1024 * 1024) continue;

            std::string mpt   = dev.value("mountpoint", std::string{});
            if (mpt == "/" || mpt == "/boot" || mpt == "/boot/efi") continue;

            std::string name  = dev.value("name",  std::string{});
            std::string label = dev.value("label", std::string{});
            std::string model = dev.value("model", std::string{});

            auto human = [](uint64_t b) -> std::string {
                char buf[32];
                if (b >= (1ULL << 40))
                    snprintf(buf, sizeof(buf), "%.1f TiB", b / (double)(1ULL << 40));
                else
                    snprintf(buf, sizeof(buf), "%.1f GiB", b / (double)(1ULL << 30));
                return buf;
            };

            StorageDrive d;
            d.path    = mpt.empty() ? ("/dev/" + name) : mpt;
            d.label   = label.empty() ? (name + (model.empty() ? "" : "  " + model)) : label;
            d.size    = human(bytes);
            d.mounted = !mpt.empty();
            d.mountpt = mpt;
            storage_drives_.push_back(std::move(d));
        }
#endif
    } catch (...) {}

    storage_enumerated_ = true;
    SL_INFO("ml_setup: found {} storage options", storage_drives_.size());
}

void MlSetupPage::write_data_store_config() {
    const char* home = std::getenv("HOME");
    const char* xdg  = std::getenv("XDG_CONFIG_HOME");
    std::string cfg_dir;
    if (xdg)  cfg_dir = std::string(xdg)  + "/straylight";
    else if (home) cfg_dir = std::string(home) + "/.config/straylight";
    if (cfg_dir.empty()) return;

    std::error_code ec;
    fs::create_directories(cfg_dir, ec);

    std::ofstream f(cfg_dir + "/ml_config.json", std::ios::trunc);
    f << "{\n"
      << "  \"gpu_profile\": \"" << gpu_profile_ << "\",\n"
      << "  \"data_store\": \"" << data_store_path_ << "\"\n"
      << "}\n";
}

void MlSetupPage::detect_frameworks() {
    SL_INFO("Detecting ML frameworks...");

    const char* modules[] = {"torch", "jax", "tensorflow"};
    for (size_t i = 0; i < frameworks_.size(); ++i) {
        frameworks_[i].present = check_python_import(modules[i]);
        SL_INFO("  {}: {}", frameworks_[i].name,
                frameworks_[i].present ? "found" : "not found");
    }

    // Detect GPU vendor
    if (fs::exists("/proc/driver/nvidia/version")) {
        gpu_vendor_ = "NVIDIA";
    } else {
        // Check lspci for AMD
        // (simplified — real impl would parse lspci output)
        gpu_vendor_ = "Unknown";
    }

    detected_ = true;
}

bool MlSetupPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##WizardML", nullptr, flags);

    ImGui::SetCursorPosY(40.0f);
    ImGui::SetCursorPosX(40.0f);
    ImGui::Text("ML & GPU Setup");
    ImGui::Separator();
    ImGui::Spacing();

    // Detect button
    ImGui::SetCursorPosX(60.0f);
    if (!detected_) {
        if (ImGui::Button("Detect Frameworks & GPU", ImVec2(250, 36))) {
            detect_frameworks();
        }
    } else {
        // Show results
        ImGui::Text("GPU Vendor: %s", gpu_vendor_.c_str());
        ImGui::Spacing();

        ImGui::Text("Installed Frameworks:");
        for (const auto& fw : frameworks_) {
            ImGui::SetCursorPosX(80.0f);
            if (fw.present) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.3f, 0.9f, 0.3f, 1.0f));
                ImGui::Text("[OK] %s", fw.name.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("[--] %s", fw.name.c_str());
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // GPU scheduling profile
        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("GPU Scheduling Profile");
        ImGui::SetCursorPosX(60.0f);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("##gpu_profile", &profile_index_,
                         kGpuProfiles, kNumProfiles)) {
            gpu_profile_ = kGpuProfiles[profile_index_];
        }

        // ── Dataset / model storage drive ────────────────────────────────
        if (!storage_enumerated_) enumerate_storage();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("Dataset & Model Storage");
        ImGui::SetCursorPosX(60.0f);
        ImGui::TextDisabled(
            "Where would you like to store datasets and models?");
        ImGui::Spacing();

        if (!storage_drives_.empty()) {
            // Build combo label list
            std::vector<const char*> labels;
            for (const auto& d : storage_drives_) labels.push_back(d.label.c_str());

            ImGui::SetCursorPosX(60.0f);
            ImGui::SetNextItemWidth(420.0f);
            ImGui::Combo("##storage_drive", &storage_index_,
                         labels.data(), static_cast<int>(labels.size()));

            if (storage_index_ >= 0 &&
                storage_index_ < static_cast<int>(storage_drives_.size())) {
                const auto& sel = storage_drives_[static_cast<size_t>(storage_index_)];
                ImGui::SetCursorPosX(80.0f);
                ImGui::TextDisabled("Path: %s  %s",
                                    sel.path.c_str(),
                                    sel.size.empty() ? "" : sel.size.c_str());
                if (!sel.mounted) {
                    ImGui::SetCursorPosX(80.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                        "This drive is not mounted — StrayLight will mount it "
                        "at /mnt/ml-data on first boot.");
                }
            }
        } else {
            ImGui::SetCursorPosX(60.0f);
            ImGui::TextDisabled("No additional drives detected — "
                                "using home directory.");
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Buttons
    ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 80.0f));
    bool advance = false;
    if (ImGui::Button("Next", ImVec2(120, 40))) {
        // Write GPU profile + data store config
        if (detected_) {
            // Determine data_store_path_
            if (!storage_drives_.empty() &&
                storage_index_ < static_cast<int>(storage_drives_.size())) {
                data_store_path_ =
                    storage_drives_[static_cast<size_t>(storage_index_)].path;
            }
            write_data_store_config();
        }
        advance = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(80, 40))) {
        advance = true;
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::wizard
