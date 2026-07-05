// apps/bench-gui/bench_panel.h
// StrayLight Bench GUI — Benchmark panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace straylight::bench {

struct BenchResult {
    std::string name;
    float       score;
    float       max_score;
    std::string unit;
    std::string detail;
};

struct BenchCategory {
    std::string name;
    std::vector<BenchResult> results;
    bool        running = false;
    float       progress = 0.0f;
    bool        completed = false;
};

struct BenchState {
    std::vector<BenchCategory> categories;
    int active_tab = 0;

    // Comparison
    bool show_comparison = false;
    std::vector<BenchCategory> prev_results;

    // System info
    std::string cpu_model = "AMD Ryzen 9 7950X (16C/32T @ 5.7 GHz)";
    std::string gpu_model = "NVIDIA RTX 4090 (24 GB VRAM)";
    std::string ram_info = "128 GB DDR5-5600 (4x32 GB)";
    std::string storage_info = "Samsung 990 PRO 2TB NVMe Gen4";
    std::string os_info = "StrayLight OS 1.0 (kernel 6.8.0-straylight)";
    std::string net_info = "Intel X710 10GbE";

    // Export
    bool show_export_dialog = false;
    int export_format = 0;

    void init() {
        // CPU
        BenchCategory cpu;
        cpu.name = "CPU";
        cpu.completed = true;
        cpu.results.push_back({"Single-Core Integer", 1850.0f, 2200.0f, "pts", "Integer operations per second"});
        cpu.results.push_back({"Single-Core Float", 2100.0f, 2500.0f, "pts", "Floating point operations per second"});
        cpu.results.push_back({"Multi-Core Integer", 28500.0f, 35000.0f, "pts", "All-core integer throughput"});
        cpu.results.push_back({"Multi-Core Float", 32100.0f, 38000.0f, "pts", "All-core floating point throughput"});
        cpu.results.push_back({"Compression (zstd)", 1450.0f, 1800.0f, "MB/s", "zstd compression throughput"});
        cpu.results.push_back({"AES-256", 12500.0f, 15000.0f, "MB/s", "AES-256 encryption throughput"});
        categories.push_back(cpu);

        // Memory
        BenchCategory mem;
        mem.name = "Memory";
        mem.completed = true;
        mem.results.push_back({"Sequential Read", 68000.0f, 80000.0f, "MB/s", "DDR5 sequential read bandwidth"});
        mem.results.push_back({"Sequential Write", 62000.0f, 80000.0f, "MB/s", "DDR5 sequential write bandwidth"});
        mem.results.push_back({"Random Read", 42000.0f, 50000.0f, "MB/s", "Random access read bandwidth"});
        mem.results.push_back({"Random Write", 38000.0f, 50000.0f, "MB/s", "Random access write bandwidth"});
        mem.results.push_back({"Latency", 62.0f, 100.0f, "ns", "Memory access latency (lower is better)"});
        categories.push_back(mem);

        // GPU
        BenchCategory gpu;
        gpu.name = "GPU";
        gpu.completed = true;
        gpu.results.push_back({"FP32 Compute", 82600.0f, 100000.0f, "GFLOPS", "Single precision floating point"});
        gpu.results.push_back({"FP16 Tensor", 330400.0f, 400000.0f, "GFLOPS", "Half precision tensor operations"});
        gpu.results.push_back({"VRAM Bandwidth", 1008.0f, 1200.0f, "GB/s", "Video memory bandwidth"});
        gpu.results.push_back({"OpenCL", 245000.0f, 300000.0f, "pts", "OpenCL compute score"});
        gpu.results.push_back({"Vulkan Compute", 198000.0f, 250000.0f, "pts", "Vulkan compute shader score"});
        categories.push_back(gpu);

        // Storage
        BenchCategory stor;
        stor.name = "Storage";
        stor.completed = true;
        stor.results.push_back({"Sequential Read", 7000.0f, 7500.0f, "MB/s", "NVMe sequential read"});
        stor.results.push_back({"Sequential Write", 6500.0f, 7000.0f, "MB/s", "NVMe sequential write"});
        stor.results.push_back({"Random Read 4K", 1200.0f, 1500.0f, "K IOPS", "4K random read IOPS"});
        stor.results.push_back({"Random Write 4K", 1050.0f, 1200.0f, "K IOPS", "4K random write IOPS"});
        stor.results.push_back({"Mixed I/O", 980.0f, 1200.0f, "K IOPS", "Mixed random read/write"});
        categories.push_back(stor);

        // Network
        BenchCategory net;
        net.name = "Network";
        net.completed = true;
        net.results.push_back({"TCP Throughput", 9400.0f, 10000.0f, "Mbps", "TCP single-stream throughput"});
        net.results.push_back({"TCP Multi-stream", 9800.0f, 10000.0f, "Mbps", "TCP 8-stream throughput"});
        net.results.push_back({"UDP Throughput", 9200.0f, 10000.0f, "Mbps", "UDP throughput"});
        net.results.push_back({"Latency", 0.12f, 1.0f, "ms", "Local network RTT (lower is better)"});
        net.results.push_back({"Connections/sec", 45000.0f, 60000.0f, "conn/s", "New TCP connections per second"});
        categories.push_back(net);

        // ML
        BenchCategory ml;
        ml.name = "ML";
        ml.completed = true;
        ml.results.push_back({"ResNet-50 Inference", 4200.0f, 5000.0f, "img/s", "ResNet-50 FP16 inference throughput"});
        ml.results.push_back({"BERT-Large Inference", 380.0f, 500.0f, "seq/s", "BERT-Large FP16 inference throughput"});
        ml.results.push_back({"LLaMA-7B Generation", 145.0f, 200.0f, "tok/s", "LLaMA-7B text generation speed"});
        ml.results.push_back({"Training (ResNet-50)", 1200.0f, 1500.0f, "img/s", "ResNet-50 FP16 training throughput"});
        ml.results.push_back({"ONNX Runtime", 8500.0f, 10000.0f, "pts", "ONNX Runtime benchmark score"});
        categories.push_back(ml);

        // Previous results for comparison
        prev_results = categories;
        for (auto& cat : prev_results) {
            for (auto& r : cat.results) {
                r.score *= 0.92f; // 8% slower baseline
            }
        }
    }
};

inline void render_bench_panel(BenchState& st) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT BENCH");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Layout: sidebar + content
    float sidebar_w = 180.0f;

    // System info sidebar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.10f, 1.0f));
    if (ImGui::BeginChild("##sys_info", ImVec2(sidebar_w, -1), true)) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "System Info");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("CPU: %s", st.cpu_model.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("GPU: %s", st.gpu_model.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("RAM: %s", st.ram_info.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Storage: %s", st.storage_info.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Network: %s", st.net_info.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("OS: %s", st.os_info.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Comparison toggle
        ImGui::Checkbox("Show Comparison", &st.show_comparison);
        ImGui::Spacing();

        // Export
        if (ImGui::Button("Export JSON", ImVec2(-1, 28))) { st.show_export_dialog = true; st.export_format = 0; }
        if (ImGui::Button("Export HTML", ImVec2(-1, 28))) { st.show_export_dialog = true; st.export_format = 1; }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine();

    // Main content with tabs
    if (ImGui::BeginChild("##bench_content", ImVec2(0, -1), false)) {
        if (ImGui::BeginTabBar("##bench_tabs")) {
            for (int ci = 0; ci < (int)st.categories.size(); ++ci) {
                auto& cat = st.categories[ci];
                if (ImGui::BeginTabItem(cat.name.c_str())) {
                    st.active_tab = ci;
                    ImGui::Spacing();

                    // Run button + progress
                    if (cat.running) {
                        ImGui::BeginDisabled();
                        ImGui::Button("Running...", ImVec2(120, 30));
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::ProgressBar(cat.progress, ImVec2(200, 30));
                        cat.progress += ImGui::GetIO().DeltaTime * 0.15f;
                        if (cat.progress >= 1.0f) { cat.running = false; cat.completed = true; cat.progress = 1.0f; }
                    } else {
                        if (ImGui::Button("Run Benchmark", ImVec2(140, 30))) {
                            cat.running = true;
                            cat.progress = 0.0f;
                        }
                        if (cat.completed) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "Completed");
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Results
                    for (int ri = 0; ri < (int)cat.results.size(); ++ri) {
                        auto& r = cat.results[ri];
                        float frac = r.score / r.max_score;
                        frac = std::min(frac, 1.0f);

                        ImGui::Text("%s", r.name.c_str());
                        ImGui::SameLine(250);

                        // Score bar
                        ImVec4 bar_col = frac > 0.8f ? ImVec4(0.0f, 0.8f, 0.5f, 1.0f)
                                       : frac > 0.6f ? ImVec4(0.5f, 0.8f, 0.2f, 1.0f)
                                       : frac > 0.4f ? ImVec4(0.8f, 0.7f, 0.1f, 1.0f)
                                       : ImVec4(0.8f, 0.3f, 0.1f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_col);

                        char score_label[64];
                        snprintf(score_label, sizeof(score_label), "%.0f %s", r.score, r.unit.c_str());
                        ImGui::ProgressBar(frac, ImVec2(300, 22), score_label);
                        ImGui::PopStyleColor();

                        // Comparison
                        if (st.show_comparison && ci < (int)st.prev_results.size() && ri < (int)st.prev_results[ci].results.size()) {
                            float prev = st.prev_results[ci].results[ri].score;
                            float diff = ((r.score - prev) / prev) * 100.0f;
                            ImGui::SameLine();
                            ImVec4 diff_col = diff >= 0 ? ImVec4(0.2f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                            ImGui::TextColored(diff_col, "%+.1f%%", diff);
                        }

                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", r.detail.c_str());
                        ImGui::Spacing();
                    }

                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    // Export dialog
    if (st.show_export_dialog) {
        ImGui::OpenPopup("Export Results");
        st.show_export_dialog = false;
    }
    if (ImGui::BeginPopupModal("Export Results", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* formats[] = {"JSON", "HTML Report"};
        ImGui::Text("Export benchmark results as %s", formats[st.export_format]);
        ImGui::Spacing();
        static char export_path[512] = "/tmp/straylight-bench-results";
        ImGui::SetNextItemWidth(400);
        ImGui::InputText("##export_path", export_path, sizeof(export_path));
        ImGui::Spacing();
        if (ImGui::Button("Export", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

} // namespace straylight::bench
