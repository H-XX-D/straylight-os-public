// apps/widgets/system/io_latency.cpp
#include "io_latency.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::IoLatencyWidget, "io_latency", "I/O Latency", straylight::widgets::WidgetCategory::System);
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>

namespace straylight::widgets {

void IoLatencyWidget::discover_devices() {
    namespace fs = std::filesystem;
    const std::string block_dir = "/sys/block";
    if (!fs::exists(block_dir)) return;

    for (auto& entry : fs::directory_iterator(block_dir)) {
        std::string devname = entry.path().filename().string();
        // Skip loop, ram, dm devices
        if (devname.find("loop") == 0 || devname.find("ram") == 0) continue;

        bool found = false;
        for (auto& d : devices_) {
            if (d.name == devname) { found = true; break; }
        }
        if (found) continue;

        BlockDevice dev;
        dev.name = devname;

        // Model
        {
            std::ifstream f(entry.path().string() + "/device/model");
            if (f) {
                std::getline(f, dev.model);
                while (!dev.model.empty() && dev.model.back() == ' ') dev.model.pop_back();
            }
        }

        // Scheduler
        {
            std::ifstream f(entry.path().string() + "/queue/scheduler");
            if (f) {
                std::string sched;
                std::getline(f, sched);
                // Extract active scheduler in brackets [mq-deadline]
                auto ob = sched.find('[');
                auto cb = sched.find(']');
                if (ob != std::string::npos && cb != std::string::npos) {
                    dev.scheduler = sched.substr(ob + 1, cb - ob - 1);
                } else {
                    dev.scheduler = sched;
                }
            }
        }

        devices_.push_back(std::move(dev));
    }
}

void IoLatencyWidget::read_stats() {
    for (auto& dev : devices_) {
        std::string stat_path = "/sys/block/" + dev.name + "/stat";
        std::ifstream f(stat_path);
        if (!f) continue;

        // /sys/block/*/stat fields:
        // read_ios read_merges read_sectors read_ticks write_ios write_merges
        // write_sectors write_ticks in_flight io_ticks time_in_queue
        // discard_ios discard_merges discard_sectors discard_ticks
        // flush_ios flush_ticks
        uint64_t r_ios, r_merges, r_sectors, r_ticks;
        uint64_t w_ios, w_merges, w_sectors, w_ticks;
        uint64_t inflight, io_ticks, time_in_queue;

        if (f >> r_ios >> r_merges >> r_sectors >> r_ticks
              >> w_ios >> w_merges >> w_sectors >> w_ticks
              >> inflight >> io_ticks >> time_in_queue) {

            // Compute deltas
            uint64_t dr_ios = r_ios - dev.prev_read_ios;
            uint64_t dw_ios = w_ios - dev.prev_write_ios;
            uint64_t dr_ticks = r_ticks - dev.prev_read_ticks;
            uint64_t dw_ticks = w_ticks - dev.prev_write_ticks;
            uint64_t dio_ticks = io_ticks - dev.prev_io_ticks;

            if (dev.prev_read_ios > 0) { // Not first sample
                dev.read_avg_ms = (dr_ios > 0) ? static_cast<float>(dr_ticks) / static_cast<float>(dr_ios) : 0.0f;
                dev.write_avg_ms = (dw_ios > 0) ? static_cast<float>(dw_ticks) / static_cast<float>(dw_ios) : 0.0f;
                // Utilization: io_ticks is in ms, sample interval in ms
                float interval_ms = sample_interval_ * 1000.0f;
                dev.util_pct = (interval_ms > 0) ? std::min(100.0f, static_cast<float>(dio_ticks) / interval_ms * 100.0f) : 0.0f;
            }

            dev.read_ios = r_ios;
            dev.write_ios = w_ios;
            dev.read_sectors = r_sectors;
            dev.write_sectors = w_sectors;
            dev.read_ticks_ms = r_ticks;
            dev.write_ticks_ms = w_ticks;
            dev.io_ticks_ms = io_ticks;

            dev.prev_read_ios = r_ios;
            dev.prev_write_ios = w_ios;
            dev.prev_read_ticks = r_ticks;
            dev.prev_write_ticks = w_ticks;
            dev.prev_io_ticks = io_ticks;
        }
    }
}

void IoLatencyWidget::push_history(BlockDevice& dev) {
    int idx = dev.hist_offset % BlockDevice::kHistLen;
    dev.read_lat_hist[idx] = dev.read_avg_ms;
    dev.write_lat_hist[idx] = dev.write_avg_ms;
    dev.util_hist[idx] = dev.util_pct;
    dev.hist_offset++;
}

void IoLatencyWidget::update() {
    if (!should_update()) return;
    if (devices_.empty()) discover_devices();
    read_stats();
    for (auto& d : devices_) push_history(d);
}

void IoLatencyWidget::render(bool* p_open) {
    if (!ImGui::Begin("I/O Latency", p_open)) {
        ImGui::End();
        return;
    }

    if (devices_.empty()) {
        ImGui::TextWrapped("No block devices found in /sys/block.");
        ImGui::End();
        return;
    }

    // Device summary table
    if (ImGui::BeginTable("##io_table", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
            ImVec2(0, 160))) {

        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("Model");
        ImGui::TableSetupColumn("Scheduler");
        ImGui::TableSetupColumn("Read Lat (ms)");
        ImGui::TableSetupColumn("Write Lat (ms)");
        ImGui::TableSetupColumn("Util");
        ImGui::TableSetupColumn("IOPS (R/W)");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
            auto& d = devices_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(d.name.c_str(), selected_dev_ == i,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                selected_dev_ = i;
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.model.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.scheduler.c_str());
            ImGui::TableNextColumn();
            {
                ImVec4 col = (d.read_avg_ms > 10.0f) ? ImVec4(1, 0.3f, 0.3f, 1) :
                             (d.read_avg_ms > 2.0f)  ? ImVec4(1, 0.8f, 0, 1) : ImVec4(1, 1, 1, 1);
                ImGui::TextColored(col, "%.2f", d.read_avg_ms);
            }
            ImGui::TableNextColumn();
            {
                ImVec4 col = (d.write_avg_ms > 10.0f) ? ImVec4(1, 0.3f, 0.3f, 1) :
                             (d.write_avg_ms > 2.0f)  ? ImVec4(1, 0.8f, 0, 1) : ImVec4(1, 1, 1, 1);
                ImGui::TextColored(col, "%.2f", d.write_avg_ms);
            }
            ImGui::TableNextColumn();
            {
                char ov[16]; std::snprintf(ov, sizeof(ov), "%.0f%%", d.util_pct);
                ImGui::ProgressBar(d.util_pct / 100.0f, ImVec2(60, 0), ov);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%llu/%llu",
                        static_cast<unsigned long long>(d.read_ios),
                        static_cast<unsigned long long>(d.write_ios));
        }
        ImGui::EndTable();
    }

    // Detail charts for selected device
    if (selected_dev_ >= 0 && selected_dev_ < static_cast<int>(devices_.size())) {
        auto& dev = devices_[selected_dev_];
        ImGui::Separator();
        ImGui::Text("Device: %s (%s)", dev.name.c_str(), dev.model.c_str());

        int count = std::min(dev.hist_offset, BlockDevice::kHistLen);
        if (count > 1) {
            std::array<float, BlockDevice::kHistLen> rp{}, wp{}, up{};
            float max_lat = 0.1f;
            for (int j = 0; j < count; ++j) {
                int src = (dev.hist_offset - count + j) % BlockDevice::kHistLen;
                rp[j] = dev.read_lat_hist[src];
                wp[j] = dev.write_lat_hist[src];
                up[j] = dev.util_hist[src];
                max_lat = std::max({max_lat, rp[j], wp[j]});
            }

            ImGui::Text("Read Latency (ms)");
            ImGui::PlotLines("##read_lat", rp.data(), count, 0, nullptr, 0.0f, max_lat * 1.3f, ImVec2(-1, 50));

            ImGui::Text("Write Latency (ms)");
            ImGui::PlotLines("##write_lat", wp.data(), count, 0, nullptr, 0.0f, max_lat * 1.3f, ImVec2(-1, 50));

            ImGui::Text("Utilization");
            ImGui::PlotLines("##io_util", up.data(), count, 0, nullptr, 0.0f, 100.0f, ImVec2(-1, 50));
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
