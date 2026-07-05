// apps/audio-gui/audio_panel.h
// StrayLight Audio Control panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace straylight::audio {

struct AudioApp {
    char name[64];
    float volume;
    bool  muted;
    float level_l;  // simulated VU meter
    float level_r;
};

struct AudioRoute {
    int source;
    int sink;
    bool active;
};

struct AudioState {
    // audio-gui-wired-wpctl
    float master_volume = 0.0f;
    bool  master_mute = false;

    int output_device = 0;
    int input_device = 0;

    std::vector<AudioApp> apps;
    std::vector<AudioRoute> routes;

    // Real device/source/sink lists from `wpctl status` (no fabricated data).
    // Stored as std::string plus a parallel const char* cache so the existing
    // ImGui::Combo(const char* const items[], int count) call keeps working.
    std::vector<std::string> output_devices;   // Audio Sinks   (descriptions)
    std::vector<std::string> input_devices;    // Audio Sources (descriptions)
    std::vector<std::string> sources;          // Stream node names (sink-inputs)
    std::vector<std::string> sinks;            // Audio Sinks   (short labels)
    std::vector<const char*> output_devices_c;
    std::vector<const char*> input_devices_c;

    float frame_counter = 0;

    // OS-read status (no daemon). No fabricated data.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // ---- OS read helpers --------------------------------------------------
    static std::string run_cmd(const char* cmd) {
        std::string out;
        FILE* p = popen(cmd, "r");
        if (!p) return out;
        char buf[1024];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        return out;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Strip the leading tree-drawing glyphs ( │ ├ └ ─ ) and spaces that wpctl
    // prints, returning the trimmed remainder of the line.
    static std::string strip_tree(const std::string& line) {
        std::string r;
        // wpctl uses UTF-8 box-drawing chars; drop any byte >= 0x80 and the
        // ASCII tree/space punctuation, keep everything from the first id digit
        // or '*' marker onward.
        size_t i = 0;
        while (i < line.size()) {
            unsigned char c = (unsigned char)line[i];
            if (c == ' ' || c == '\t' || c == '|') { ++i; continue; }
            if (c >= 0x80) { ++i; continue; }       // box-drawing byte
            break;
        }
        r = line.substr(i);
        return r;
    }

    // Parse one node line of the form (after tree strip):
    //   "*   56. Built-in Audio Analog Stereo        [vol: 0.40]"
    //   "35. GB206 ... (HDMI) [vol: 0.40]"
    // Returns true on success; fills id/desc/is_default. vol left untouched.
    static bool parse_node(const std::string& body, int& id, std::string& desc,
                           bool& is_default) {
        std::string s = trim(body);
        if (s.empty()) return false;
        is_default = false;
        if (s[0] == '*') { is_default = true; s = trim(s.substr(1)); }
        // leading integer id followed by '.'
        size_t dot = s.find('.');
        if (dot == std::string::npos) return false;
        std::string idtok = trim(s.substr(0, dot));
        if (idtok.empty()) return false;
        for (char ch : idtok) if (ch < '0' || ch > '9') return false;
        id = atoi(idtok.c_str());
        std::string rest = trim(s.substr(dot + 1));
        // strip trailing "[vol: ...]" tag if present
        size_t br = rest.rfind('[');
        if (br != std::string::npos) rest = trim(rest.substr(0, br));
        desc = rest;
        return true;
    }

    void rebuild_cstr() {
        output_devices_c.clear();
        input_devices_c.clear();
        for (auto& s : output_devices) output_devices_c.push_back(s.c_str());
        for (auto& s : input_devices)  input_devices_c.push_back(s.c_str());
    }

    void refresh() {
        ok_ = false;
        err_.clear();

        std::string status = run_cmd("wpctl status 2>/dev/null");
        if (trim(status).empty()) {
            err_ = "wpctl status returned nothing (PipeWire/WirePlumber unavailable)";
            output_devices.clear(); input_devices.clear();
            sources.clear(); sinks.clear(); apps.clear(); routes.clear();
            rebuild_cstr();
            return;
        }

        output_devices.clear();
        input_devices.clear();
        sources.clear();
        sinks.clear();
        apps.clear();
        routes.clear();

        int default_sink_idx = -1;
        int default_source_idx = -1;

        // Walk lines. Track which Audio sub-section we are in.
        // Sections: 0=none 1=Sinks 2=Sources 3=Streams (within the Audio block).
        enum Sec { NONE, SINKS, SOURCES, STREAMS };
        Sec sec = NONE;
        bool in_audio = false;

        std::istringstream ss(status);
        std::string line;
        while (std::getline(ss, line)) {
            std::string body = trim(strip_tree(line));
            if (body == "Audio") { in_audio = true; sec = NONE; continue; }
            if (body == "Video" || body == "Settings") { in_audio = false; sec = NONE; continue; }
            if (!in_audio) continue;

            // Section headers end with ':' (e.g. "Sinks:", "Sources:", "Streams:")
            if (body == "Sinks:")      { sec = SINKS;   continue; }
            if (body == "Sources:")    { sec = SOURCES; continue; }
            if (body == "Streams:")    { sec = STREAMS; continue; }
            if (body == "Devices:" || body == "Filters:") { sec = NONE; continue; }
            if (body.empty()) continue;

            int id = 0; std::string desc; bool is_def = false;
            if (!parse_node(body, id, desc, is_def)) continue;

            if (sec == SINKS) {
                output_devices.push_back(desc);
                sinks.push_back(desc);
                if (is_def) default_sink_idx = (int)output_devices.size() - 1;
            } else if (sec == SOURCES) {
                input_devices.push_back(desc);
                if (is_def) default_source_idx = (int)input_devices.size() - 1;
            } else if (sec == STREAMS) {
                // Each playback stream (sink-input) -> an AudioApp + a route.
                AudioApp a{};
                snprintf(a.name, sizeof(a.name), "%s", desc.c_str());
                a.volume = 0.0f;     // per-stream volume queried below
                a.muted = false;
                a.level_l = 0.0f;    // no real VU source -> stay zero
                a.level_r = 0.0f;
                // per-stream volume via node id
                {
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd), "wpctl get-volume %d 2>/dev/null", id);
                    std::string vout = run_cmd(cmd);
                    float v = 0.0f;
                    size_t vp = vout.find("Volume:");
                    if (vp != std::string::npos) v = (float)atof(vout.c_str() + vp + 7);
                    a.volume = v;
                    a.muted = (vout.find("MUTED") != std::string::npos);
                }
                apps.push_back(a);
                sources.push_back(desc);
                // Route this stream to the default sink (its link target).
                routes.push_back({ (int)apps.size() - 1,
                                   default_sink_idx >= 0 ? default_sink_idx : 0,
                                   true });
            }
        }

        if (default_sink_idx >= 0)   output_device = default_sink_idx;
        if (default_source_idx >= 0) input_device  = default_source_idx;

        // Master volume / mute from the default sink.
        {
            std::string vout = run_cmd("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
            size_t vp = vout.find("Volume:");
            if (vp != std::string::npos) master_volume = (float)atof(vout.c_str() + vp + 7);
            else master_volume = 0.0f;
            master_mute = (vout.find("MUTED") != std::string::npos);
        }

        rebuild_cstr();

        if (output_devices.empty() && input_devices.empty()) {
            err_ = "wpctl status listed no audio sinks or sources";
            ok_ = false;
            return;
        }
        ok_ = true;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() {
        refresh();
    }
};

inline void render_audio_panel(AudioState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("audio unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);
    st.frame_counter += ImGui::GetIO().DeltaTime;

    // per-frame VU mock removed: levels come only from real data (refresh());
    // no real VU source via wpctl, so level_l/level_r stay zero.

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("AUDIO CONTROL");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Master Volume
    ImGui::BeginChild("##master", ImVec2(-1, 70), true);
    ImGui::TextColored(accent, "Master Volume");
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::Checkbox("Mute##master_mute", &st.master_mute)) {}

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80);
    ImGui::SliderFloat("##master_vol", &st.master_volume, 0.0f, 1.0f, "%.0f%%");
    ImGui::SameLine();
    ImGui::Text("%.0f%%", st.master_volume * 100);
    ImGui::EndChild();
    ImGui::Spacing();

    // Device selection
    ImGui::BeginChild("##devices", ImVec2(-1, 60), true);
    ImGui::Text("Output:");
    ImGui::SameLine(80);
    ImGui::SetNextItemWidth(250);
    ImGui::Combo("##out_dev", &st.output_device, st.output_devices_c.data(), (int)st.output_devices_c.size());
    ImGui::SameLine(400);
    ImGui::Text("Input:");
    ImGui::SameLine(450);
    ImGui::SetNextItemWidth(250);
    ImGui::Combo("##in_dev", &st.input_device, st.input_devices_c.data(), (int)st.input_devices_c.size());
    ImGui::EndChild();
    ImGui::Spacing();

    float half_w = ImGui::GetContentRegionAvail().x * 0.55f;

    // Per-app volume sliders
    ImGui::BeginChild("##per_app", ImVec2(half_w, 0), true);
    ImGui::TextColored(accent, "Per-Application Volume");
    ImGui::Separator();
    ImGui::Spacing();

    for (int i = 0; i < (int)st.apps.size(); ++i) {
        auto& a = st.apps[i];
        ImGui::PushID(i);

        ImGui::Text("%s", a.name);
        ImGui::SameLine(130);
        ImGui::SetNextItemWidth(180);
        ImGui::SliderFloat("##vol", &a.volume, 0.0f, 1.0f, "%.0f%%");
        ImGui::SameLine();
        ImGui::Checkbox("Mute", &a.muted);

        // VU meter bars
        ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        float bar_w = ImGui::GetContentRegionAvail().x - 20;
        float bar_h = 6.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();

        // Left channel
        draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                            IM_COL32(30, 30, 50, 255));
        float fill_l = a.level_l * st.master_volume * bar_w;
        ImU32 color_l = (a.level_l > 0.8f) ? IM_COL32(255, 60, 60, 255) :
                        (a.level_l > 0.5f) ? IM_COL32(255, 200, 0, 255) : IM_COL32(0, 255, 136, 255);
        draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + fill_l, bar_pos.y + bar_h), color_l);
        ImGui::Dummy(ImVec2(0, bar_h + 1));

        // Right channel
        ImVec2 bar_pos2 = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(bar_pos2, ImVec2(bar_pos2.x + bar_w, bar_pos2.y + bar_h),
                            IM_COL32(30, 30, 50, 255));
        float fill_r = a.level_r * st.master_volume * bar_w;
        ImU32 color_r = (a.level_r > 0.8f) ? IM_COL32(255, 60, 60, 255) :
                        (a.level_r > 0.5f) ? IM_COL32(255, 200, 0, 255) : IM_COL32(0, 255, 136, 255);
        draw->AddRectFilled(bar_pos2, ImVec2(bar_pos2.x + fill_r, bar_pos2.y + bar_h), color_r);
        ImGui::Dummy(ImVec2(0, bar_h + 6));

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Audio routing matrix
    ImGui::BeginChild("##routing", ImVec2(0, 0), true);
    ImGui::TextColored(accent, "Audio Routing Matrix");
    ImGui::Separator();
    ImGui::Spacing();

    // Header row
    ImGui::Text("          ");
    for (int s = 0; s < (int)st.sinks.size(); ++s) {
        ImGui::SameLine(100 + s * 80);
        ImGui::Text("%s", st.sinks[s].c_str());
    }

    for (int src = 0; src < (int)st.sources.size(); ++src) {
        ImGui::Text("%-10s", st.sources[src].c_str());
        for (int snk = 0; snk < (int)st.sinks.size(); ++snk) {
            ImGui::SameLine(100 + snk * 80);
            ImGui::PushID(src * 100 + snk);
            bool connected = false;
            for (auto& r : st.routes) {
                if (r.source == src && r.sink == snk) { connected = r.active; break; }
            }
            if (ImGui::Checkbox("##route", &connected)) {
                bool found = false;
                for (auto& r : st.routes) {
                    if (r.source == src && r.sink == snk) { r.active = connected; found = true; break; }
                }
                if (!found && connected) {
                    st.routes.push_back({src, snk, true});
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

} // namespace straylight::audio
