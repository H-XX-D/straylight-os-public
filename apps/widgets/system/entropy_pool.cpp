// apps/widgets/system/entropy_pool.cpp
#include "entropy_pool.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::EntropyPoolWidget, "entropy_pool", "Entropy Pool", straylight::widgets::WidgetCategory::System);
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <sstream>

namespace straylight::widgets {

void EntropyPoolWidget::read_kernel_entropy() {
    {
        std::ifstream f("/proc/sys/kernel/random/entropy_avail");
        if (f) f >> entropy_avail_;
    }
    {
        std::ifstream f("/proc/sys/kernel/random/poolsize");
        if (f) f >> poolsize_;
    }
    // Default poolsize if not readable (modern kernels report 256)
    if (poolsize_ <= 0) poolsize_ = 4096;
}

void EntropyPoolWidget::read_straylight_entropy() {
    kernel_source_online_ = false;
    rdrand_available_ = false;
    rdseed_available_ = false;
    jitter_available_ = false;
    kernel_bytes_generated_ = 0;
    kernel_health_failures_ = 0;

    std::ifstream f("/proc/straylight/entropy");
    if (!f) return;

    kernel_source_online_ = true;
    std::string line;
    while (std::getline(f, line)) {
        auto has_available = [&] {
            return line.find("available") != std::string::npos;
        };
        if (line.find("RDRAND:") != std::string::npos) {
            rdrand_available_ = has_available();
        } else if (line.find("RDSEED:") != std::string::npos) {
            rdseed_available_ = has_available();
        } else if (line.find("Jitter:") != std::string::npos) {
            jitter_available_ = has_available();
        } else if (line.find("Bytes generated:") != std::string::npos) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::istringstream iss(line.substr(colon + 1));
                iss >> kernel_bytes_generated_;
            }
        } else if (line.find("Health failures:") != std::string::npos) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::istringstream iss(line.substr(colon + 1));
                iss >> kernel_health_failures_;
            }
        }
    }
}

void EntropyPoolWidget::read_drbg_stats() {
    if (!ipc_connected_) {
        auto res = ipc_.connect("/run/straylight/entropy.sock");
        ipc_connected_ = res.has_value();
        if (!ipc_connected_) {
            error_msg_ = res.error();
            return;
        }
    }

    auto res = ipc_.command("entropy.drbg_stats");
    if (!res.has_value()) {
        error_msg_ = res.error();
        ipc_connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("instances") || !j["instances"].is_array()) return;

    drbgs_.clear();
    for (auto& dj : j["instances"]) {
        DrbgInstance d;
        d.name = dj.value("name", "");
        d.algorithm = dj.value("algorithm", "unknown");
        d.bytes_generated = dj.value("bytes_generated", uint64_t(0));
        d.reseed_count = dj.value("reseed_count", uint64_t(0));
        d.health_ok = dj.value("health_ok", true);
        drbgs_.push_back(std::move(d));
    }
}

void EntropyPoolWidget::update() {
    if (!should_update()) return;
    read_kernel_entropy();
    read_straylight_entropy();
    read_drbg_stats();

    int idx = hist_offset_ % kHistLen;
    entropy_hist_[idx] = static_cast<float>(entropy_avail_);
    hist_offset_++;
}

void EntropyPoolWidget::render(bool* p_open) {
    if (!ImGui::Begin("Entropy Pool", p_open)) {
        ImGui::End();
        return;
    }

    // Kernel entropy gauge
    ImGui::Text("Kernel Entropy Pool");
    float frac = (poolsize_ > 0) ? static_cast<float>(entropy_avail_) / static_cast<float>(poolsize_) : 0.0f;
    char ov[64]; std::snprintf(ov, sizeof(ov), "%d / %d bits", entropy_avail_, poolsize_);

    ImVec4 col;
    if (entropy_avail_ < 256) col = ImVec4(1, 0.2f, 0.2f, 1);
    else if (entropy_avail_ < 1024) col = ImVec4(1, 0.8f, 0, 1);
    else col = ImVec4(0.3f, 0.9f, 0.3f, 1);

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(std::min(frac, 1.0f), ImVec2(-1, 20), ov);
    ImGui::PopStyleColor();

    if (entropy_avail_ < 256) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "WARNING: Low entropy may cause blocking in /dev/random");
    }

    // Entropy history
    int count = std::min(hist_offset_, kHistLen);
    if (count > 0) {
        std::array<float, kHistLen> plot{};
        for (int j = 0; j < count; ++j) {
            int src = (hist_offset_ - count + j) % kHistLen;
            plot[j] = entropy_hist_[src];
        }
        ImGui::PlotLines("##ent_hist", plot.data(), count,
                         0, nullptr, 0.0f, static_cast<float>(poolsize_), ImVec2(-1, 60));
    }

    ImGui::Separator();

    ImGui::Text("StrayLight Kernel Source");
    if (kernel_source_online_) {
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "online");
        ImGui::SameLine();
        ImGui::TextColored(rdrand_available_ ? ImVec4(0.3f, 1, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                           "RDRAND");
        ImGui::SameLine();
        ImGui::TextColored(rdseed_available_ ? ImVec4(0.3f, 1, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                           "RDSEED");
        ImGui::SameLine();
        ImGui::TextColored(jitter_available_ ? ImVec4(0.3f, 1, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                           "Jitter");
        ImGui::Text("Kernel bytes: %llu", static_cast<unsigned long long>(kernel_bytes_generated_));
        ImGui::SameLine();
        ImGui::Text("Health failures: %llu", static_cast<unsigned long long>(kernel_health_failures_));
    } else {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "/proc/straylight/entropy missing");
    }

    ImGui::Separator();

    // DRBG instances from straylight-entropy
    ImGui::Text("DRBG Instances");
    if (!ipc_connected_) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "straylight-entropy not connected");
        if (!error_msg_.empty()) ImGui::TextWrapped("(%s)", error_msg_.c_str());
    }

    if (!drbgs_.empty()) {
        if (ImGui::BeginTable("##drbg_table", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Algorithm");
            ImGui::TableSetupColumn("Bytes Out");
            ImGui::TableSetupColumn("Reseeds");
            ImGui::TableSetupColumn("Health");
            ImGui::TableHeadersRow();

            for (auto& d : drbgs_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(d.algorithm.c_str());
                ImGui::TableNextColumn();
                if (d.bytes_generated >= (1ULL << 30)) {
                    ImGui::Text("%.2f GiB", static_cast<double>(d.bytes_generated) / (1ULL << 30));
                } else if (d.bytes_generated >= (1ULL << 20)) {
                    ImGui::Text("%.2f MiB", static_cast<double>(d.bytes_generated) / (1ULL << 20));
                } else {
                    ImGui::Text("%llu B", static_cast<unsigned long long>(d.bytes_generated));
                }
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(d.reseed_count));
                ImGui::TableNextColumn();
                if (d.health_ok) {
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "OK");
                } else {
                    ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "FAIL");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
