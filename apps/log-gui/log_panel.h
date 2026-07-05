// apps/log-gui/log_panel.h
// StrayLight Log Viewer panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
// STRAYLIGHT_LOG_GUI_WIRED: real data-source includes (journalctl JSON over os CLI)
#include <nlohmann/json.hpp>
#include <array>
#include <cstdlib>
#include <ctime>

namespace straylight::logview {

struct LogEntry {
    char timestamp[32];
    char service[32];
    int  level;  // 0=debug, 1=info, 2=warn, 3=error, 4=critical
    char message[256];
};

struct ServiceErrorCount {
    char name[32];
    int  errors;
};

struct LogState {
    // STRAYLIGHT_LOG_GUI_WIRED (os source_kind: journalctl -o json; no fabricated data)
    std::vector<LogEntry> entries;
    std::vector<LogEntry> filtered;
    std::vector<ServiceErrorCount> error_stats;

    int  filter_service = 0;
    int  filter_level = 0;
    char search_text[256] = {};
    bool follow_mode = true;
    bool auto_scroll = true;
    float sim_timer = 0;

    // Live os link (journalctl). No daemon socket for this panel.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;
    int         load_count_ = 200;  // number of journal entries to load per refresh

    static constexpr const char* services[] = {
        "All", "compositor", "networking", "audio", "session",
        "vault", "scheduler", "kernel", "display"
    };
    static constexpr int num_services = 9;

    static constexpr const char* levels[] = {
        "All", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
    };
    static constexpr int num_levels = 6;

    static constexpr const char* level_names[] = { "DEBUG", "INFO", "WARN", "ERROR", "CRIT" };

    // Map syslog PRIORITY (0-7) to LogEntry.level (0=debug..4=critical).
    // 7 -> DEBUG(0), 6 -> INFO(1), 4/5 -> WARN(2), 3 -> ERROR(3), 0-2 -> CRITICAL(4).
    static int priority_to_level(int prio) {
        if (prio >= 7) return 0;   // debug
        if (prio == 6) return 1;   // info
        if (prio >= 4) return 2;   // warn (4,5)
        if (prio == 3) return 3;   // error
        return 4;                  // critical (0,1,2)
    }

    // journalctl may emit a field as a string, an int, or an array of values.
    static std::string json_str(const nlohmann::json& j, const char* key) {
        auto it = j.find(key);
        if (it == j.end() || it->is_null()) return std::string();
        if (it->is_string()) return it->get<std::string>();
        if (it->is_number_integer()) return std::to_string(it->get<long long>());
        if (it->is_number()) return std::to_string(it->get<double>());
        if (it->is_array() && !it->empty()) {
            const auto& f = it->front();
            if (f.is_string()) return f.get<std::string>();
        }
        return std::string();
    }

    void ensure_open() {
        // os CLI: nothing persistent to open; readiness is established by refresh().
    }

    void refresh() {
        ensure_open();
        // One-shot snapshot of the most recent journal entries as JSON lines.
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "journalctl -n %d -o json --no-pager 2>/dev/null", load_count_);
        FILE* fp = popen(cmd, "r");
        if (!fp) {
            ok_ = false;
            err_ = "failed to launch journalctl (popen)";
            return;
        }

        std::vector<LogEntry> parsed;
        std::string line;
        line.reserve(4096);
        int c;
        int parse_errors = 0;
        while ((c = fgetc(fp)) != EOF) {
            if (c != '\n') { line.push_back((char)c); continue; }
            if (!line.empty()) {
                nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
                if (!j.is_discarded() && j.is_object()) {
                    LogEntry e{};

                    // __REALTIME_TIMESTAMP is microseconds since epoch (as string).
                    std::string ts_us = json_str(j, "__REALTIME_TIMESTAMP");
                    if (!ts_us.empty()) {
                        long long us = 0;
                        for (char ch : ts_us) {
                            if (ch < '0' || ch > '9') break;
                            us = us * 10 + (ch - '0');
                        }
                        time_t secs = (time_t)(us / 1000000LL);
                        struct tm tmv{};
                        localtime_r(&secs, &tmv);
                        strftime(e.timestamp, sizeof(e.timestamp), "%Y-%m-%d %H:%M:%S", &tmv);
                    } else {
                        e.timestamp[0] = '\0';
                    }

                    // SYSLOG_IDENTIFIER, fallback _COMM.
                    std::string ident = json_str(j, "SYSLOG_IDENTIFIER");
                    if (ident.empty()) ident = json_str(j, "_COMM");
                    snprintf(e.service, sizeof(e.service), "%s", ident.c_str());

                    // PRIORITY (syslog 0-7) -> level. Default to INFO if absent.
                    std::string pr = json_str(j, "PRIORITY");
                    int prio = 6;
                    if (!pr.empty()) {
                        bool neg = false; size_t i = 0;
                        if (pr[0] == '-') { neg = true; i = 1; }
                        int v = 0; bool any = false;
                        for (; i < pr.size(); ++i) {
                            if (pr[i] < '0' || pr[i] > '9') break;
                            v = v * 10 + (pr[i] - '0'); any = true;
                        }
                        if (any) prio = neg ? -v : v;
                    }
                    e.level = priority_to_level(prio);

                    // MESSAGE.
                    std::string msg = json_str(j, "MESSAGE");
                    snprintf(e.message, sizeof(e.message), "%s", msg.c_str());

                    parsed.push_back(e);
                } else {
                    ++parse_errors;
                }
            }
            line.clear();
        }
        // Trailing line without newline.
        if (!line.empty()) {
            nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_discarded() && j.is_object()) {
                LogEntry e{};
                std::string ts_us = json_str(j, "__REALTIME_TIMESTAMP");
                if (!ts_us.empty()) {
                    long long us = 0;
                    for (char ch : ts_us) {
                        if (ch < '0' || ch > '9') break;
                        us = us * 10 + (ch - '0');
                    }
                    time_t secs = (time_t)(us / 1000000LL);
                    struct tm tmv{};
                    localtime_r(&secs, &tmv);
                    strftime(e.timestamp, sizeof(e.timestamp), "%Y-%m-%d %H:%M:%S", &tmv);
                }
                std::string ident = json_str(j, "SYSLOG_IDENTIFIER");
                if (ident.empty()) ident = json_str(j, "_COMM");
                snprintf(e.service, sizeof(e.service), "%s", ident.c_str());
                std::string pr = json_str(j, "PRIORITY");
                int prio = 6;
                if (!pr.empty()) {
                    int v = 0; bool any = false;
                    for (char ch : pr) { if (ch < '0' || ch > '9') break; v = v * 10 + (ch - '0'); any = true; }
                    if (any) prio = v;
                }
                e.level = priority_to_level(prio);
                std::string msg = json_str(j, "MESSAGE");
                snprintf(e.message, sizeof(e.message), "%s", msg.c_str());
                parsed.push_back(e);
            }
        }

        int rc = pclose(fp);
        if (rc != 0 && parsed.empty()) {
            ok_ = false;
            err_ = "journalctl returned no entries (exit code nonzero)";
            return;
        }

        entries = std::move(parsed);
        ok_ = true;
        err_.clear();
        apply_filter();
        compute_stats();
        auto_scroll = true;
        (void)parse_errors;
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

    void apply_filter() {
        filtered.clear();
        std::string search(search_text);
        for (auto& e : entries) {
            if (filter_service > 0 && strcmp(e.service, services[filter_service]) != 0) continue;
            if (filter_level > 0 && e.level < (filter_level - 1)) continue;
            if (!search.empty()) {
                std::string msg(e.message);
                if (msg.find(search) == std::string::npos &&
                    std::string(e.service).find(search) == std::string::npos) continue;
            }
            filtered.push_back(e);
        }
    }

    void compute_stats() {
        // Count entries with level>=3 (== syslog PRIORITY<=3) grouped by identifier.
        error_stats.clear();
        for (auto& e : entries) {
            if (e.level < 3) continue;
            bool found = false;
            for (auto& sc : error_stats) {
                if (strcmp(sc.name, e.service) == 0) { sc.errors++; found = true; break; }
            }
            if (!found) {
                ServiceErrorCount sc{};
                snprintf(sc.name, 32, "%s", e.service);
                sc.errors = 1;
                error_stats.push_back(sc);
            }
        }
    }
};

inline void render_log_panel(LogState& st) {
    if (st.entries.empty()) st.init();
    // STRAYLIGHT_LOG_GUI_WIRED: refresh real data + unavailable banner
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("journal unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    // STRAYLIGHT_LOG_GUI_WIRED: per-frame rand() mock removed; entries come from
    // LogState::refresh() (journalctl -o json). Live follow is driven by
    // maybe_refresh()'s ~2s throttle, not fabricated entries.

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("LOG VIEWER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Filter bar
    ImGui::Text("Service:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("##svc_filter", &st.filter_service, LogState::services, LogState::num_services)) {
        st.apply_filter();
    }
    ImGui::SameLine(230);
    ImGui::Text("Level:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("##lvl_filter", &st.filter_level, LogState::levels, LogState::num_levels)) {
        st.apply_filter();
    }
    ImGui::SameLine(440);
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputTextWithHint("##search", "Search...", st.search_text, sizeof(st.search_text))) {
        st.apply_filter();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &st.follow_mode);
    ImGui::SameLine();
    ImGui::Text("(%zu entries)", st.filtered.size());
    ImGui::Spacing();

    float stats_w = 250.0f;

    // Log view
    ImGui::BeginChild("##log_view", ImVec2(ImGui::GetContentRegionAvail().x - stats_w - 8, 0), true);

    ImVec4 level_colors[] = {
        ImVec4(0.5f, 0.5f, 0.5f, 1.0f),  // DEBUG - gray
        ImVec4(0.7f, 0.9f, 1.0f, 1.0f),  // INFO - light blue
        ImVec4(1.0f, 0.85f, 0.0f, 1.0f), // WARN - yellow
        ImVec4(1.0f, 0.3f, 0.3f, 1.0f),  // ERROR - red
        ImVec4(1.0f, 0.0f, 0.5f, 1.0f),  // CRITICAL - magenta
    };

    ImGuiListClipper clipper;
    clipper.Begin((int)st.filtered.size());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            auto& e = st.filtered[i];
            ImVec4 col = level_colors[std::clamp(e.level, 0, 4)];

            ImGui::TextDisabled("%s", e.timestamp);
            ImGui::SameLine(170);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("[%-5s]", LogState::level_names[std::clamp(e.level, 0, 4)]);
            ImGui::PopStyleColor();
            ImGui::SameLine(240);
            ImGui::TextColored(accent, "%-12s", e.service);
            ImGui::SameLine(350);
            ImGui::TextWrapped("%s", e.message);
        }
    }

    if (st.auto_scroll && st.follow_mode) {
        ImGui::SetScrollHereY(1.0f);
        st.auto_scroll = false;
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Statistics panel
    ImGui::BeginChild("##stats", ImVec2(stats_w, 0), true);
    ImGui::TextColored(accent, "Error Statistics");
    ImGui::Separator();
    ImGui::Spacing();

    int max_errors = 1;
    for (auto& s : st.error_stats) max_errors = std::max(max_errors, s.errors);

    for (auto& s : st.error_stats) {
        ImGui::Text("%-12s", s.name);
        ImGui::SameLine(110);

        float frac = (float)s.errors / (float)max_errors;
        ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        float bar_w = ImGui::GetContentRegionAvail().x - 30;
        float bar_h = 14;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                            IM_COL32(30, 30, 50, 255), 2.0f);
        ImU32 bar_col = s.errors > 2 ? IM_COL32(255, 60, 60, 255) :
                        s.errors > 0 ? IM_COL32(255, 200, 0, 255) : IM_COL32(0, 255, 136, 255);
        draw->AddRectFilled(bar_pos, ImVec2(bar_pos.x + frac * bar_w, bar_pos.y + bar_h),
                            bar_col, 2.0f);
        ImGui::Dummy(ImVec2(bar_w, bar_h));
        ImGui::SameLine();
        ImGui::Text("%d", s.errors);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Total entries: %zu", st.entries.size());
    ImGui::Text("Filtered: %zu", st.filtered.size());
    int total_errors = 0;
    for (auto& e : st.entries) if (e.level >= 3) total_errors++;
    ImGui::Text("Total errors: %d", total_errors);

    ImGui::EndChild();
}

} // namespace straylight::logview
