// apps/snn-gui/snn_panel.cpp
#include "snn_panel.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cfloat>
#include <algorithm>

namespace straylight::snn {

static constexpr ImVec4 kCyan     = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple   = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold     = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel  = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted    = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2   = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess  = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger   = {1.0f, 0.298f, 0.416f, 1.0f};

static ImU32 ToU32(ImVec4 v) {
    return IM_COL32(static_cast<int>(v.x*255), static_cast<int>(v.y*255),
                    static_cast<int>(v.z*255), static_cast<int>(v.w*255));
}

// Normalize a membrane potential in mV to [0,1] for visual alpha/fill mapping.
static float vnorm(float v_mv) {
    float n = (v_mv - kVRest) / (kVThreshold - kVRest); // V_rest->0, V_thr->1
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    return n;
}

void SNNState::init() {
    // Build a real layered LIF network from config (no rand(), no literals).
    build_network(config, neurons, synapses, input_ids);
    engine.reset(neurons.size(), synapses.size(), synapses, input_ids);

    // Metrics start at a true zero state; they fill in from the live engine.
    metrics.spike_rate_hz        = 0.0f;
    metrics.avg_membrane_v       = kVRest;
    metrics.energy_pj            = 0.0f;
    metrics.latency_ms           = 0.0f;
    metrics.total_spikes         = 0;
    metrics.accuracy             = 0.0f; // no classification task wired (see label)
    metrics.spike_history_offset = 0;
    for (int i = 0; i < 256; ++i) metrics.spike_history[i] = 0.0f;

    current_step    = 0;
    sim_progress    = 0.0f;
    simulating      = true;
    last_step_time_ = -1.0e9;
    raster_step_    = 0;
    spike_accum_    = 0;
    rate_window_ms_ = 0.0;

    // Raster: one row per neuron, rolling 100-step window (starts empty).
    spike_times.assign(neurons.size(), {});
}

void SNNState::record_metrics(int spikes_this_step, float compute_ms) {
    // Mean membrane potential across the network (real values, mV).
    double sum_v = 0.0;
    for (const auto& n : neurons) sum_v += n.membrane_v;
    metrics.avg_membrane_v = neurons.empty() ? kVRest
                           : (float)(sum_v / (double)neurons.size());

    metrics.total_spikes += spikes_this_step;

    // Rolling spike-rate over a ~200 ms window: spikes / elapsed seconds.
    spike_accum_    += spikes_this_step;
    rate_window_ms_ += config.dt_ms;
    if (rate_window_ms_ >= 200.0) {
        metrics.spike_rate_hz = (float)(spike_accum_ / (rate_window_ms_ / 1000.0));
        spike_accum_    = 0;
        rate_window_ms_ = 0.0;
    }

    // MODELED energy: spikes * per-spike pJ constant (NOT measured HW).
    metrics.energy_pj = metrics.total_spikes * kPerSpikePj;

    // Latency = measured wall-clock compute time of this step.
    metrics.latency_ms = compute_ms;

    // Spike-rate history ring (spikes emitted this step).
    metrics.spike_history[metrics.spike_history_offset] = (float)spikes_this_step;
    metrics.spike_history_offset = (metrics.spike_history_offset + 1) % 256;
}

void SNNState::step() {
    auto t0 = std::chrono::steady_clock::now();
    int spikes = engine.step(neurons, synapses, config, config.dt_ms);
    auto t1 = std::chrono::steady_clock::now();
    float compute_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    record_metrics(spikes, compute_ms);

    // Update rolling raster window (100 columns).
    int col = raster_step_ % 100;
    if (col == 0) for (auto& row : spike_times) row.clear();
    for (size_t k = 0; k < neurons.size(); ++k) {
        if (neurons[k].spiking) spike_times[k].push_back(col);
    }
    ++raster_step_;

    ++current_step;
    sim_progress = config.sim_steps > 0
                 ? (float)(current_step % config.sim_steps) / (float)config.sim_steps
                 : 0.0f;
}

void SNNState::maybe_step() {
    // Pace the simulation to wall-clock so it spikes live each frame.
    double now = ImGui::GetTime();
    if (last_step_time_ < 0.0) { last_step_time_ = now; step(); return; }
    double elapsed_ms = (now - last_step_time_) * 1000.0;
    // Advance up to a small batch of dt-steps per frame (cap to avoid spiral).
    int budget = 8;
    while (elapsed_ms >= config.dt_ms && budget-- > 0) {
        step();
        elapsed_ms     -= config.dt_ms;
        last_step_time_ += config.dt_ms / 1000.0;
    }
    if (budget <= 0) last_step_time_ = now; // resync if we fell behind
}

void SNNPanel::render_network_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("NetworkPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "NETWORK TOPOLOGY");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "%d neurons  %d synapses",
        static_cast<int>(state_.neurons.size()),
        static_cast<int>(state_.synapses.size()));
    ImGui::Separator();
    ImGui::Spacing();

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2 origin    = ImGui::GetCursorScreenPos();
    float avail_w    = ImGui::GetContentRegionAvail().x;
    float avail_h    = 340.0f;
    float neuron_r   = 10.0f;

    // Draw synapses first
    for (const auto& syn : state_.synapses) {
        if (!syn.active) continue;
        const auto& pre  = state_.neurons[syn.pre_id];
        const auto& post = state_.neurons[syn.post_id];
        float px = origin.x + pre.x  * avail_w;
        float py = origin.y + pre.y  * avail_h;
        float qx = origin.x + post.x * avail_w;
        float qy = origin.y + post.y * avail_h;

        float thickness = 0.5f + std::min(std::abs(syn.weight) * 0.18f, 2.5f);
        ImVec4 syn_col = syn.weight > 0.0f
            ? ImVec4{kCyan.x, kCyan.y, kCyan.z, 0.25f}
            : ImVec4{kDanger.x, kDanger.y, kDanger.z, 0.20f};
        dl->AddLine({px, py}, {qx, qy}, ToU32(syn_col), thickness);
    }

    // Draw neurons
    for (const auto& n : state_.neurons) {
        float nx = origin.x + n.x * avail_w;
        float ny = origin.y + n.y * avail_h;

        if (n.spiking) {
            // Glow effect
            dl->AddCircleFilled({nx, ny}, neuron_r + 5.0f, IM_COL32(25, 231, 255, 40));
            dl->AddCircleFilled({nx, ny}, neuron_r, ToU32(kCyan));
            dl->AddCircle({nx, ny}, neuron_r + 2.0f, IM_COL32(25, 231, 255, 180), 16, 2.0f);
        } else {
            float fill_a = 0.2f + vnorm(n.membrane_v) * 0.6f;
            ImVec4 fill_col{kCyan.x, kCyan.y, kCyan.z, fill_a};
            dl->AddCircleFilled({nx, ny}, neuron_r, ToU32(fill_col));
            dl->AddCircle({nx, ny}, neuron_r, IM_COL32(60, 120, 160, 160), 16, 1.0f);
        }

        // Tooltip
        if (ImGui::IsMouseHoveringRect(
                {nx - neuron_r, ny - neuron_r}, {nx + neuron_r, ny + neuron_r})) {
            ImGui::BeginTooltip();
            ImGui::TextColored(kCyan, "Neuron %d (%s)", n.id, n.label.c_str());
            ImGui::TextColored(kMuted, "V_m: %.2f mV  Thr: %.2f mV", n.membrane_v, n.threshold);
            ImGui::TextColored(n.spiking ? kSuccess : kMuted2,
                n.spiking ? "SPIKING" : "Resting");
            ImGui::EndTooltip();
        }
    }

    ImGui::Dummy(ImVec2(avail_w, avail_h));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Spike Rate: ");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%.1f Hz", state_.metrics.spike_rate_hz);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "Energy: ");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%.1f pJ", state_.metrics.energy_pj);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextColored(kMuted, "MODELED: spikes x %.2f pJ/spike constant (not measured hardware)", kPerSpikePj);
        ImGui::EndTooltip();
    }
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "Accuracy: ");
    ImGui::SameLine();
    ImGui::TextColored(kSuccess, "%.1f%%", state_.metrics.accuracy * 100.0f);
    ImGui::SameLine();
    ImGui::TextColored(kMuted2, "(no task wired)");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SNNPanel::render_activity_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("ActivityPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "NEURAL ACTIVITY");
    ImGui::Separator();
    ImGui::Spacing();

    // Raster plot
    ImGui::TextColored(kMuted, "Spike Raster (rolling 100 timesteps, live)");
    ImGui::Spacing();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 raster_origin = ImGui::GetCursorScreenPos();
    float raster_w = ImGui::GetContentRegionAvail().x;
    float raster_h = 120.0f;
    float dot_r    = 2.0f;
    int   n_neurons = static_cast<int>(state_.spike_times.size());
    int   n_steps   = 100;

    dl->AddRectFilled(raster_origin,
        {raster_origin.x + raster_w, raster_origin.y + raster_h},
        IM_COL32(6, 10, 25, 220), 6.0f);

    for (int n = 0; n < n_neurons; ++n) {
        float ny = raster_origin.y + (static_cast<float>(n) + 0.5f) / static_cast<float>(n_neurons) * raster_h;
        for (int t : state_.spike_times[n]) {
            float tx = raster_origin.x + (static_cast<float>(t) + 0.5f) / static_cast<float>(n_steps) * raster_w;
            dl->AddCircleFilled({tx, ny}, dot_r, ToU32(kCyan));
        }
    }

    ImGui::Dummy(ImVec2(raster_w, raster_h));

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Spikes / step (history)");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_PlotLines, kCyan);
    ImGui::PlotLines("##spike_rate", state_.metrics.spike_history, 256,
        state_.metrics.spike_history_offset, nullptr, 0.0f, FLT_MAX, ImVec2(-1.0f, 60.0f));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();

    // Energy gauge
    ImGui::TextColored(kMuted, "Energy (modeled): ");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%.2f pJ", state_.metrics.energy_pj);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextColored(kMuted, "MODELED: total_spikes x %.2f pJ/spike (not measured hardware)", kPerSpikePj);
        ImGui::EndTooltip();
    }
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "Latency: ");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%.3f ms", state_.metrics.latency_ms);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextColored(kMuted, "Measured wall-clock compute time of one step()");
        ImGui::EndTooltip();
    }
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "Total spikes: ");
    ImGui::SameLine();
    ImGui::TextColored(kPurple, "%d", state_.metrics.total_spikes);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SNNPanel::render_plasticity_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("PlasticityPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "SYNAPTIC PLASTICITY");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(kMuted, "Weight Matrix (first 8x8 synapses, live STDP)");
    ImGui::Spacing();

    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2 hm_origin   = ImGui::GetCursorScreenPos();
    float cell_size    = 24.0f;
    int   grid_n       = 8;

    // Normalize the heatmap against the live weight range.
    float wmax = 1.0f;
    for (const auto& s : state_.synapses) wmax = std::max(wmax, std::abs(s.weight));

    for (int row = 0; row < grid_n; ++row) {
        for (int col = 0; col < grid_n; ++col) {
            int idx = row * grid_n + col;
            float weight = 0.0f;
            float stdp   = 0.0f;
            if (idx < static_cast<int>(state_.synapses.size())) {
                weight = state_.synapses[idx].weight;
                stdp   = state_.synapses[idx].stdp_delta;
            }

            float norm = weight / wmax; // -1..1
            ImVec4 color;
            if (weight > 0.0f) {
                color = {kCyan.x * norm, kCyan.y * norm, kCyan.z * norm, 0.9f};
            } else {
                float abs_n = std::abs(norm);
                color = {kDanger.x * abs_n, kDanger.y * abs_n * 0.5f, kDanger.z * abs_n * 0.5f, 0.9f};
            }

            float cx = hm_origin.x + col * cell_size;
            float cy = hm_origin.y + row * cell_size;
            dl->AddRectFilled({cx + 1, cy + 1}, {cx + cell_size - 1, cy + cell_size - 1},
                ToU32(color), 2.0f);

            if (ImGui::IsMouseHoveringRect({cx, cy}, {cx + cell_size, cy + cell_size})) {
                ImGui::BeginTooltip();
                ImGui::TextColored(kCyan, "syn[%d]: w=%.3f", idx, weight);
                ImGui::TextColored(kMuted, "last STDP dw=%.5f", stdp);
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::Dummy(ImVec2(grid_n * cell_size, grid_n * cell_size));
    ImGui::Spacing();
    ImGui::TextColored(kMuted2, "[ ] Strong excitatory");
    ImGui::SameLine();
    ImGui::TextColored(kDanger, "[ ] Inhibitory");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "STDP Config:");
    ImGui::Spacing();
    ImGui::Text("LTP amplitude: %.4f", state_.config.stdp_ap);
    ImGui::Text("LTD amplitude: %.4f", state_.config.stdp_ad);
    ImGui::TextColored(state_.config.stdp_enabled ? kSuccess : kMuted2,
        state_.config.stdp_enabled ? "STDP: ENABLED" : "STDP: DISABLED");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SNNPanel::render_config_tab() {
    auto& cfg = state_.config;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("ConfigPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "SNN CONFIGURATION");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Simulation timestep (ms)");
    ImGui::SliderFloat("##dt", &cfg.dt_ms, 0.1f, 5.0f, "%.2f ms");

    ImGui::Spacing();
    ImGui::Text("Simulation steps");
    ImGui::SliderInt("##steps", &cfg.sim_steps, 100, 5000);

    ImGui::Spacing();
    ImGui::Text("Spike threshold");
    ImGui::SliderFloat("##threshold", &cfg.base_threshold, 0.1f, 3.0f, "%.2f");

    ImGui::Spacing();
    ImGui::Text("Membrane time constant (ms)");
    ImGui::SliderFloat("##tau_m", &cfg.tau_m_ms, 1.0f, 100.0f, "%.1f ms");

    ImGui::Spacing();
    ImGui::Text("Synaptic time constant (ms)");
    ImGui::SliderFloat("##tau_s", &cfg.tau_s_ms, 0.5f, 20.0f, "%.1f ms");

    ImGui::Spacing();
    ImGui::Text("Input firing rate (Hz)");
    ImGui::SliderFloat("##input_rate", &cfg.input_rate_hz, 1.0f, 200.0f, "%.0f Hz");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "STDP Parameters");
    ImGui::Spacing();
    ImGui::Checkbox("Enable STDP", &cfg.stdp_enabled);
    ImGui::BeginDisabled(!cfg.stdp_enabled);
    ImGui::SliderFloat("LTP (A+)", &cfg.stdp_ap, 0.0001f, 0.1f, "%.4f");
    ImGui::SliderFloat("LTD (A-)", &cfg.stdp_ad, 0.0001f, 0.1f, "%.4f");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.098f, 0.906f, 1.000f, 0.18f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.098f, 0.906f, 1.000f, 0.32f});
    if (ImGui::Button("Apply & Reset", ImVec2(140.0f, 34.0f))) {
        state_.init(); // rebuild network + engine from current config
    }
    ImGui::SameLine();
    if (ImGui::Button("Export Weights", ImVec2(140.0f, 34.0f))) {}
    ImGui::PopStyleColor(2);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void SNNPanel::render() {
    // Advance the live LIF simulation each frame (real spikes, no fabrication).
    state_.maybe_step();

    ImGui::TextColored(kCyan, "STRAYLIGHT");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "Spiking Neural Net Simulator (LIF, no neuromorphic HW)");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "%d neurons  |  step %d",
        static_cast<int>(state_.neurons.size()), state_.current_step);
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("snn_tabs", ImGuiTabBarFlags_None)) {
        const char* tab_names[] = {"Network", "Activity", "Plasticity", "Config"};
        for (int t = 0; t < 4; ++t) {
            if (ImGui::BeginTabItem(tab_names[t])) {
                state_.active_tab = t;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    switch (state_.active_tab) {
        case 0: render_network_tab();    break;
        case 1: render_activity_tab();   break;
        case 2: render_plasticity_tab(); break;
        case 3: render_config_tab();     break;
    }
}

} // namespace straylight::snn
