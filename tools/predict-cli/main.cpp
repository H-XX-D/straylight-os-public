/**
 * StrayLight Predict CLI — Interface for the predictive preloading daemon.
 *
 * Commands:
 *   straylight-predict status       — Show daemon status and statistics
 *   straylight-predict predictions  — Show current predictions
 *   straylight-predict history      — Show usage patterns
 *   straylight-predict train        — Force model retrain
 *   straylight-predict preloads     — Show active preloads
 *   straylight-predict enable       — Enable predictive preloading
 *   straylight-predict disable      — Disable predictive preloading
 */

#include <cstring>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static constexpr const char* SOCKET_PATH = "/run/straylight/predict.sock";
static constexpr const char* VERSION = "1.0.0";

// ─── ANSI colors ────────────────────────────────────────────────────────────

namespace color {
    constexpr const char* reset   = "\033[0m";
    constexpr const char* bold    = "\033[1m";
    constexpr const char* red     = "\033[31m";
    constexpr const char* green   = "\033[32m";
    constexpr const char* yellow  = "\033[33m";
    constexpr const char* blue    = "\033[34m";
    constexpr const char* cyan    = "\033[36m";
    constexpr const char* magenta = "\033[35m";
    constexpr const char* dim     = "\033[2m";
}

// ─── JSON helpers ───────────────────────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return "";
    ++pos;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
        result += json[pos++];
    }
    return result;
}

static double extract_json_number(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return 0.0;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return 0.0;
    try { return std::stod(json.substr(pos)); } catch (...) { return 0.0; }
}

static int extract_json_int(const std::string& json, const std::string& key) {
    return static_cast<int>(extract_json_number(json, key));
}

static bool extract_json_bool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return false;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    return pos != std::string::npos && json.substr(pos, 4) == "true";
}

// ─── Socket communication ───────────────────────────────────────────────────

static std::string send_rpc(const std::string& method, const std::string& params = "{}") {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return "{\"error\":\"socket failed\"}";

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "{\"error\":\"cannot connect to predict daemon at " +
               std::string(SOCKET_PATH) + " - is straylight-predict running?\"}";
    }

    std::ostringstream rpc;
    rpc << "{\"jsonrpc\":\"2.0\",\"method\":\"" << method
        << "\",\"params\":" << params << ",\"id\":1}\n";

    std::string msg = rpc.str();
    write(fd, msg.c_str(), msg.size());

    struct pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, 10000) <= 0) {
        close(fd);
        return "{\"error\":\"response timeout\"}";
    }

    std::string response;
    char buf[8192];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        response += buf;
        if (response.find('\n') != std::string::npos) break;
    }
    close(fd);
    return response;
}

// ─── Extract result block from RPC response ─────────────────────────────────

static std::string extract_result(const std::string& response) {
    // Check for error
    if (response.find("\"error\"") != std::string::npos &&
        response.find("\"result\"") == std::string::npos) {
        std::string err = extract_json_string(response, "message");
        if (err.empty()) err = extract_json_string(response, "error");
        std::cerr << color::red << "Error: " << color::reset << err << "\n";
        return "";
    }

    auto result_pos = response.find("\"result\"");
    if (result_pos == std::string::npos) return "";

    auto brace = response.find('{', result_pos + 8);
    if (brace == std::string::npos) return "";

    int depth = 0;
    size_t end = brace;
    for (size_t i = brace; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        if (response[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
    }

    return response.substr(brace, end - brace + 1);
}

// ─── Command handlers ───────────────────────────────────────────────────────

static int cmd_status() {
    auto result = extract_result(send_rpc("predict.status"));
    if (result.empty()) return 1;

    bool enabled = extract_json_bool(result, "enabled");
    int events = extract_json_int(result, "model_events");
    int apps = extract_json_int(result, "known_apps");
    int cycles = extract_json_int(result, "prediction_cycles");
    int preloads = extract_json_int(result, "total_preloads");
    int evictions = extract_json_int(result, "total_evictions");
    int active = extract_json_int(result, "active_preloads");
    int predicted = extract_json_int(result, "predicted_apps");
    int ram_budget = extract_json_int(result, "ram_budget_mb");
    int ram_used = extract_json_int(result, "ram_used_mb");
    int vram_budget = extract_json_int(result, "vram_budget_mb");
    int vram_used = extract_json_int(result, "vram_used_mb");

    std::cout << "\n";
    std::cout << color::bold << color::cyan << "  StrayLight Predict" << color::reset
              << color::dim << " v" << VERSION << color::reset << "\n";
    std::cout << color::dim << "  ─────────────────────────────────────────"
              << color::reset << "\n\n";

    std::cout << "  Status:             "
              << (enabled ? (std::string(color::green) + "ENABLED") :
                            (std::string(color::red) + "DISABLED"))
              << color::reset << "\n";
    std::cout << "  Model events:       " << events << "\n";
    std::cout << "  Known apps:         " << apps << "\n";
    std::cout << "  Prediction cycles:  " << cycles << "\n";
    std::cout << "  Total preloads:     " << preloads << "\n";
    std::cout << "  Total evictions:    " << evictions << "\n";
    std::cout << "  Active preloads:    " << active << "\n";
    std::cout << "  Predicted apps:     " << predicted << "\n\n";

    // RAM budget bar
    std::cout << "  RAM budget:   ";
    if (ram_budget > 0) {
        int bar = (ram_used * 20) / ram_budget;
        for (int i = 0; i < 20; ++i) {
            if (i < bar) std::cout << color::green << "█";
            else std::cout << color::dim << "░";
        }
        std::cout << color::reset << " " << ram_used << "/" << ram_budget << " MB\n";
    } else {
        std::cout << color::dim << "N/A" << color::reset << "\n";
    }

    // VRAM budget bar
    std::cout << "  VRAM budget:  ";
    if (vram_budget > 0) {
        int bar = (vram_used * 20) / vram_budget;
        for (int i = 0; i < 20; ++i) {
            if (i < bar) std::cout << color::magenta << "█";
            else std::cout << color::dim << "░";
        }
        std::cout << color::reset << " " << vram_used << "/" << vram_budget << " MB\n";
    } else {
        std::cout << color::dim << "N/A" << color::reset << "\n";
    }

    std::cout << "\n";
    return 0;
}

static int cmd_predictions() {
    auto result = extract_result(send_rpc("predict.predictions", "{\"top_n\": 10}"));
    if (result.empty()) return 1;

    std::string focused = extract_json_string(result, "focused_app");

    std::cout << "\n";
    std::cout << color::bold << color::cyan << "  Predictions" << color::reset << "\n";
    std::cout << color::dim << "  ─────────────────────────────────────────"
              << color::reset << "\n";

    if (!focused.empty()) {
        std::cout << "  Focused app: " << color::bold << focused << color::reset << "\n";
    }
    std::cout << "\n";

    // Parse predictions array
    auto arr_start = result.find("\"predictions\"");
    if (arr_start == std::string::npos) {
        std::cout << "  No predictions available.\n\n";
        return 0;
    }

    arr_start = result.find('[', arr_start);
    if (arr_start == std::string::npos) return 0;

    size_t pos = arr_start + 1;
    int rank = 0;
    while (pos < result.size()) {
        auto obj_start = result.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < result.size(); ++i) {
            if (result[i] == '{') ++depth;
            if (result[i] == '}') { --depth; if (depth == 0) { obj_end = i; break; } }
        }

        std::string obj = result.substr(obj_start, obj_end - obj_start + 1);
        std::string app = extract_json_string(obj, "app");
        double prob = extract_json_number(obj, "probability");
        std::string reason = extract_json_string(obj, "reason");
        bool preloaded = extract_json_bool(obj, "preloaded");

        ++rank;
        int bar_len = static_cast<int>(prob * 30);

        std::cout << "  " << color::dim << rank << "." << color::reset << " "
                  << color::bold << app << color::reset;
        if (preloaded) {
            std::cout << " " << color::green << "[preloaded]" << color::reset;
        }
        std::cout << "\n";

        std::cout << "     ";
        if (prob >= 0.3) std::cout << color::green;
        else if (prob >= 0.15) std::cout << color::yellow;
        else std::cout << color::dim;
        for (int i = 0; i < 30; ++i) {
            std::cout << (i < bar_len ? "█" : "░");
        }
        std::cout << color::reset << " " << std::fixed << std::setprecision(1)
                  << (prob * 100) << "%\n";

        std::cout << "     " << color::dim << reason << color::reset << "\n\n";

        pos = obj_end + 1;
    }

    if (rank == 0) {
        std::cout << "  No predictions available. Run 'straylight-predict train' first.\n\n";
    }

    return 0;
}

static int cmd_history() {
    auto result = extract_result(send_rpc("predict.history"));
    if (result.empty()) return 1;

    int total_apps = extract_json_int(result, "total_apps");

    std::cout << "\n";
    std::cout << color::bold << color::cyan << "  Usage History" << color::reset
              << " (" << total_apps << " apps tracked)\n";
    std::cout << color::dim << "  ─────────────────────────────────────────"
              << color::reset << "\n\n";

    // Parse patterns array
    auto arr_start = result.find("\"patterns\"");
    if (arr_start == std::string::npos) return 0;
    arr_start = result.find('[', arr_start);
    if (arr_start == std::string::npos) return 0;

    // Table header
    std::cout << "  " << color::bold
              << std::left << std::setw(25) << "App"
              << std::right << std::setw(10) << "Launches"
              << std::setw(12) << "Avg(sec)"
              << "  Preceded By"
              << color::reset << "\n";
    std::cout << "  " << color::dim << std::string(70, '-') << color::reset << "\n";

    size_t pos = arr_start + 1;
    while (pos < result.size()) {
        auto obj_start = result.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < result.size(); ++i) {
            if (result[i] == '{') ++depth;
            if (result[i] == '}') { --depth; if (depth == 0) { obj_end = i; break; } }
        }

        std::string obj = result.substr(obj_start, obj_end - obj_start + 1);
        std::string app = extract_json_string(obj, "app");
        int launches = extract_json_int(obj, "total_launches");
        double avg_sec = extract_json_number(obj, "avg_session_seconds");
        std::string preceded = extract_json_string(obj, "preceded_by");

        std::cout << "  "
                  << std::left << std::setw(25) << app
                  << std::right << std::setw(10) << launches
                  << std::setw(12) << std::fixed << std::setprecision(1) << avg_sec
                  << "  " << color::dim << (preceded.empty() ? "-" : preceded)
                  << color::reset << "\n";

        pos = obj_end + 1;
    }

    std::cout << "\n";
    return 0;
}

static int cmd_train() {
    std::cout << color::dim << "  Training model..." << color::reset << "\n";

    auto result = extract_result(send_rpc("predict.train"));
    if (result.empty()) return 1;

    int events = extract_json_int(result, "events");
    int apps = extract_json_int(result, "apps");

    std::cout << color::green << "  Model trained" << color::reset
              << ": " << events << " events, " << apps << " apps\n\n";
    return 0;
}

static int cmd_preloads() {
    auto result = extract_result(send_rpc("predict.preloads"));
    if (result.empty()) return 1;

    std::cout << "\n";
    std::cout << color::bold << color::cyan << "  Active Preloads" << color::reset << "\n";
    std::cout << color::dim << "  ─────────────────────────────────────────"
              << color::reset << "\n\n";

    // Parse budget
    auto budget_pos = result.find("\"budget\"");
    if (budget_pos != std::string::npos) {
        auto b_start = result.find('{', budget_pos);
        if (b_start != std::string::npos) {
            int d = 0;
            size_t b_end = b_start;
            for (size_t i = b_start; i < result.size(); ++i) {
                if (result[i] == '{') ++d;
                if (result[i] == '}') { --d; if (d == 0) { b_end = i; break; } }
            }
            std::string budget = result.substr(b_start, b_end - b_start + 1);
            std::cout << "  Budget: RAM "
                      << extract_json_int(budget, "used_ram_mb") << "/"
                      << extract_json_int(budget, "max_ram_mb") << " MB"
                      << "  |  VRAM "
                      << extract_json_int(budget, "used_vram_mb") << "/"
                      << extract_json_int(budget, "max_vram_mb") << " MB\n\n";
        }
    }

    // Parse preloads array
    auto arr_start = result.find("\"preloads\"");
    if (arr_start == std::string::npos) return 0;
    arr_start = result.find('[', arr_start);
    if (arr_start == std::string::npos) return 0;
    auto arr_end = result.find(']', arr_start);
    if (arr_end == std::string::npos) return 0;

    size_t pos = arr_start + 1;
    int count = 0;
    while (pos < arr_end) {
        auto obj_start = result.find('{', pos);
        if (obj_start == std::string::npos || obj_start > arr_end) break;

        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < arr_end; ++i) {
            if (result[i] == '{') ++depth;
            if (result[i] == '}') { --depth; if (depth == 0) { obj_end = i; break; } }
        }
        if (depth != 0) break;

        std::string obj = result.substr(obj_start, obj_end - obj_start + 1);
        std::string app = extract_json_string(obj, "app");
        std::string resource = extract_json_string(obj, "resource");
        std::string type = extract_json_string(obj, "type");
        int size_kb = extract_json_int(obj, "size_kb");
        int loaded_ago = extract_json_int(obj, "loaded_seconds_ago");
        bool active = extract_json_bool(obj, "active");

        ++count;
        const char* type_color = color::dim;
        if (type == "library") type_color = color::blue;
        else if (type == "vpu_slab") type_color = color::magenta;
        else if (type == "file") type_color = color::yellow;
        else if (type == "mesh_node") type_color = color::cyan;

        std::cout << "  " << color::bold << app << color::reset
                  << " " << type_color << "[" << type << "]" << color::reset;
        if (!active) std::cout << color::red << " (evicted)" << color::reset;
        std::cout << "\n";
        std::cout << "    " << color::dim << resource << color::reset;
        if (size_kb > 0) {
            if (size_kb > 1024) {
                std::cout << "  " << (size_kb / 1024) << " MB";
            } else {
                std::cout << "  " << size_kb << " KB";
            }
        }
        std::cout << "  " << loaded_ago << "s ago\n\n";

        pos = obj_end + 1;
    }

    if (count == 0) {
        std::cout << "  No active preloads.\n\n";
    }

    return 0;
}

static int cmd_enable() {
    auto result = extract_result(send_rpc("predict.enable"));
    if (result.empty()) return 1;
    std::cout << color::green << "  Predictive preloading enabled." << color::reset << "\n";
    return 0;
}

static int cmd_disable() {
    auto result = extract_result(send_rpc("predict.disable"));
    if (result.empty()) return 1;
    std::cout << color::yellow << "  Predictive preloading disabled."
              << color::reset << " All preloads evicted.\n";
    return 0;
}

static void print_usage() {
    std::cout << color::bold << "straylight-predict" << color::reset
              << " — Predictive resource preloading\n\n";
    std::cout << color::bold << "USAGE:" << color::reset << "\n";
    std::cout << "    straylight-predict <command>\n\n";
    std::cout << color::bold << "COMMANDS:" << color::reset << "\n";
    std::cout << "    status        Show daemon status and statistics\n";
    std::cout << "    predictions   Show current app predictions\n";
    std::cout << "    history       Show learned usage patterns\n";
    std::cout << "    train         Force model retrain from timeline data\n";
    std::cout << "    preloads      Show active resource preloads\n";
    std::cout << "    enable        Enable predictive preloading\n";
    std::cout << "    disable       Disable and evict all preloads\n";
    std::cout << "    --version     Show version\n";
    std::cout << "    --help        Show this help\n\n";
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }
    if (cmd == "--version" || cmd == "-v") {
        std::cout << "straylight-predict " << VERSION << "\n";
        return 0;
    }

    if (cmd == "status")       return cmd_status();
    if (cmd == "predictions")  return cmd_predictions();
    if (cmd == "history")      return cmd_history();
    if (cmd == "train")        return cmd_train();
    if (cmd == "preloads")     return cmd_preloads();
    if (cmd == "enable")       return cmd_enable();
    if (cmd == "disable")      return cmd_disable();

    std::cerr << color::red << "Error: " << color::reset
              << "unknown command: " << cmd << "\n\n";
    print_usage();
    return 1;
}
