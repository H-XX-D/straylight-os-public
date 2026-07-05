/**
 * StrayLight Intent CLI — Natural-language system command interface.
 *
 * Usage:
 *   straylight-intent "record my screen at 4k"
 *   straylight-intent --dry-run "encrypt file secrets.txt"
 *   straylight-intent --interactive
 *   straylight-intent --no-confirm "turn off wifi"
 *   straylight-intent --list-actions
 */

#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static constexpr const char* SOCKET_PATH = "/run/straylight/intent.sock";
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
    constexpr const char* dim     = "\033[2m";
}

// ─── JSON helpers ───────────────────────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
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
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
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

static bool extract_json_bool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return false;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return false;
    return json.substr(pos, 4) == "true";
}

static std::vector<std::string> extract_json_string_array(const std::string& json,
                                                          const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;
    auto arr_start = json.find('[', pos);
    if (arr_start == std::string::npos) return result;
    auto arr_end = json.find(']', arr_start);
    if (arr_end == std::string::npos) return result;
    std::string arr = json.substr(arr_start + 1, arr_end - arr_start - 1);
    size_t p = 0;
    while (p < arr.size()) {
        auto q1 = arr.find('"', p);
        if (q1 == std::string::npos) break;
        auto q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        p = q2 + 1;
    }
    return result;
}

// ─── Socket communication ───────────────────────────────────────────────────

static std::string send_rpc(const std::string& method, const std::string& params_json) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return "{\"error\": \"cannot create socket\"}";
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "{\"error\": \"cannot connect to intent daemon at " +
               std::string(SOCKET_PATH) + " - is straylight-intent running?\"}";
    }

    std::ostringstream rpc;
    rpc << "{\"jsonrpc\": \"2.0\", \"method\": \"" << method
        << "\", \"params\": " << params_json << ", \"id\": 1}\n";

    std::string msg = rpc.str();
    ssize_t sent = write(fd, msg.c_str(), msg.size());
    if (sent < 0 || static_cast<size_t>(sent) != msg.size()) {
        close(fd);
        return "{\"error\": \"write failed\"}";
    }

    // Read response with timeout
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 10000); // 10 second timeout
    if (pr <= 0) {
        close(fd);
        return "{\"error\": \"response timeout\"}";
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

// ─── Display helpers ────────────────────────────────────────────────────────

static void print_intent_plan(const std::string& response) {
    std::string action_type = extract_json_string(response, "action_type");
    std::string action_name = extract_json_string(response, "action_name");
    std::string description = extract_json_string(response, "description");
    double confidence = extract_json_number(response, "confidence");
    bool destructive = extract_json_bool(response, "destructive");
    auto commands = extract_json_string_array(response, "commands");

    std::cout << "\n";
    std::cout << color::bold << color::cyan << "  Intent Resolution" << color::reset << "\n";
    std::cout << color::dim << "  ─────────────────────────────────────────" << color::reset << "\n";

    std::cout << "  Action:      " << color::bold << action_name << color::reset << "\n";
    std::cout << "  Type:        " << action_type << "\n";
    std::cout << "  Description: " << description << "\n";

    // Confidence bar
    std::cout << "  Confidence:  ";
    int bar_len = static_cast<int>(confidence * 20);
    if (confidence >= 0.8) std::cout << color::green;
    else if (confidence >= 0.5) std::cout << color::yellow;
    else std::cout << color::red;
    for (int i = 0; i < 20; ++i) {
        std::cout << (i < bar_len ? "█" : "░");
    }
    std::cout << " " << static_cast<int>(confidence * 100) << "%" << color::reset << "\n";

    if (destructive) {
        std::cout << "  " << color::red << color::bold
                  << "WARNING: This action is destructive!" << color::reset << "\n";
    }

    std::cout << "\n  " << color::bold << "Commands:" << color::reset << "\n";
    for (size_t i = 0; i < commands.size(); ++i) {
        std::cout << "    " << color::dim << (i + 1) << "." << color::reset << " "
                  << color::yellow << commands[i] << color::reset << "\n";
    }
    std::cout << "\n";
}

static void print_execution_results(const std::string& response) {
    // Find the execution array
    auto exec_pos = response.find("\"execution\"");
    if (exec_pos == std::string::npos) return;

    auto arr_start = response.find('[', exec_pos);
    if (arr_start == std::string::npos) return;

    // Parse each execution result object
    size_t pos = arr_start + 1;
    int idx = 0;
    while (pos < response.size()) {
        auto obj_start = response.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < response.size(); ++i) {
            if (response[i] == '{') ++depth;
            if (response[i] == '}') {
                --depth;
                if (depth == 0) { obj_end = i; break; }
            }
        }

        std::string obj = response.substr(obj_start, obj_end - obj_start + 1);
        bool success = extract_json_bool(obj, "success");
        double duration = extract_json_number(obj, "duration_ms");
        std::string output = extract_json_string(obj, "output");
        std::string error = extract_json_string(obj, "error");

        ++idx;
        if (success) {
            std::cout << color::green << "  [OK]" << color::reset
                      << " Command " << idx << " completed in "
                      << static_cast<int>(duration) << "ms\n";
        } else {
            std::cout << color::red << "  [FAIL]" << color::reset
                      << " Command " << idx << " failed\n";
        }

        if (!output.empty()) {
            // Indent and print output
            std::istringstream ss(output);
            std::string line;
            while (std::getline(ss, line)) {
                std::cout << color::dim << "    " << line << color::reset << "\n";
            }
        }
        if (!error.empty() && !success) {
            std::cout << color::red << "    " << error << color::reset << "\n";
        }

        pos = obj_end + 1;
    }
}

static void print_actions_list(const std::string& response) {
    auto result_pos = response.find("\"result\"");
    if (result_pos == std::string::npos) {
        std::cerr << color::red << "Error: " << color::reset
                  << extract_json_string(response, "message") << "\n";
        return;
    }

    // Extract the result block
    auto brace = response.find('{', result_pos + 8);
    if (brace == std::string::npos) return;

    int depth = 0;
    size_t end = brace;
    for (size_t i = brace; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        if (response[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
    }
    std::string result = response.substr(brace, end - brace + 1);

    // Find actions array
    auto arr_start = result.find('[');
    if (arr_start == std::string::npos) return;

    std::cout << "\n" << color::bold << color::cyan
              << "  Registered Actions" << color::reset << "\n";
    std::cout << color::dim << "  ─────────────────────────────────────────"
              << color::reset << "\n\n";

    // Parse each action object
    size_t pos = arr_start + 1;
    while (pos < result.size()) {
        auto obj_start = result.find('{', pos);
        if (obj_start == std::string::npos) break;

        int d = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < result.size(); ++i) {
            if (result[i] == '{') ++d;
            if (result[i] == '}') { --d; if (d == 0) { obj_end = i; break; } }
        }

        std::string obj = result.substr(obj_start, obj_end - obj_start + 1);
        std::string name = extract_json_string(obj, "name");
        std::string desc = extract_json_string(obj, "description");
        std::string type = extract_json_string(obj, "type");
        bool destructive = extract_json_bool(obj, "destructive");

        std::cout << "  " << color::bold << name << color::reset;
        if (destructive) {
            std::cout << " " << color::red << "[destructive]" << color::reset;
        }
        std::cout << "\n";
        std::cout << "    " << color::dim << type << color::reset
                  << " — " << desc << "\n\n";

        pos = obj_end + 1;
    }
}

static bool ask_confirmation() {
    std::cout << color::bold << "  Execute? [y/N] " << color::reset;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

// ─── Modes ──────────────────────────────────────────────────────────────────

static int run_resolve(const std::string& text, bool dry_run, bool no_confirm) {
    std::string method = dry_run ? "intent.resolve" : "intent.execute";

    std::ostringstream params;
    params << "{\"text\": \"" << json_escape(text) << "\"";
    if (no_confirm) {
        params << ", \"force\": true";
    }
    params << "}";

    std::string response = send_rpc(method, params.str());

    // Check for connection error
    if (response.find("\"error\"") != std::string::npos &&
        response.find("\"result\"") == std::string::npos) {
        std::string err = extract_json_string(response, "message");
        if (err.empty()) err = extract_json_string(response, "error");
        std::cerr << color::red << "Error: " << color::reset << err << "\n";
        return 1;
    }

    // Extract the result
    auto result_pos = response.find("\"result\"");
    if (result_pos == std::string::npos) {
        std::cerr << color::red << "Error: " << color::reset << "no result in response\n";
        return 1;
    }

    // Find the result object
    auto brace = response.find('{', result_pos + 8);
    if (brace == std::string::npos) {
        std::cerr << color::red << "Error: " << color::reset << "malformed response\n";
        return 1;
    }

    int depth = 0;
    size_t end = brace;
    for (size_t i = brace; i < response.size(); ++i) {
        if (response[i] == '{') ++depth;
        if (response[i] == '}') { --depth; if (depth == 0) { end = i; break; } }
    }
    std::string result = response.substr(brace, end - brace + 1);

    // Check if needs_confirmation
    if (extract_json_bool(result, "needs_confirmation")) {
        // Show the plan from the intent sub-object
        auto intent_pos = result.find("\"intent\"");
        if (intent_pos != std::string::npos) {
            auto ib = result.find('{', intent_pos + 8);
            if (ib != std::string::npos) {
                int d2 = 0;
                size_t ie = ib;
                for (size_t i = ib; i < result.size(); ++i) {
                    if (result[i] == '{') ++d2;
                    if (result[i] == '}') { --d2; if (d2 == 0) { ie = i; break; } }
                }
                print_intent_plan(result.substr(ib, ie - ib + 1));
            }
        }

        if (no_confirm || ask_confirmation()) {
            // Re-send with force
            std::ostringstream p2;
            p2 << "{\"text\": \"" << json_escape(text) << "\", \"force\": true}";
            std::string r2 = send_rpc("intent.execute", p2.str());

            auto rp2 = r2.find("\"result\"");
            if (rp2 != std::string::npos) {
                auto b2 = r2.find('{', rp2 + 8);
                if (b2 != std::string::npos) {
                    int d2 = 0;
                    size_t e2 = b2;
                    for (size_t i = b2; i < r2.size(); ++i) {
                        if (r2[i] == '{') ++d2;
                        if (r2[i] == '}') { --d2; if (d2 == 0) { e2 = i; break; } }
                    }
                    std::string res2 = r2.substr(b2, e2 - b2 + 1);
                    print_intent_plan(res2);
                    print_execution_results(res2);
                }
            }
        } else {
            std::cout << color::dim << "  Cancelled." << color::reset << "\n";
            return 0;
        }
        return 0;
    }

    if (dry_run) {
        // Resolve-only mode
        print_intent_plan(result);
        std::cout << color::dim << "  (dry-run: not executed)" << color::reset << "\n\n";
        return 0;
    }

    // Show the intent plan from the result
    auto intent_pos = result.find("\"intent\"");
    if (intent_pos != std::string::npos) {
        auto ib = result.find('{', intent_pos + 8);
        if (ib != std::string::npos) {
            int d2 = 0;
            size_t ie = ib;
            for (size_t i = ib; i < result.size(); ++i) {
                if (result[i] == '{') ++d2;
                if (result[i] == '}') { --d2; if (d2 == 0) { ie = i; break; } }
            }
            print_intent_plan(result.substr(ib, ie - ib + 1));
        }
    } else {
        // Direct resolve result
        print_intent_plan(result);
    }

    // Show execution results if present
    if (result.find("\"execution\"") != std::string::npos) {
        print_execution_results(result);
    }

    return 0;
}

static int run_interactive() {
    std::cout << color::bold << color::cyan
              << "\n  StrayLight Intent " << color::reset
              << color::dim << "v" << VERSION << color::reset << "\n";
    std::cout << color::dim
              << "  Natural-language system commands. Type 'quit' to exit.\n"
              << color::reset << "\n";

    while (true) {
        std::cout << color::bold << color::blue << "  intent> " << color::reset;
        std::string line;
        if (!std::getline(std::cin, line)) break;

        // Trim
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t");
        line = line.substr(start, end - start + 1);

        if (line.empty()) continue;
        if (line == "quit" || line == "exit" || line == "q") break;

        if (line == "actions" || line == "list") {
            std::string response = send_rpc("intent.actions", "{}");
            print_actions_list(response);
            continue;
        }

        if (line == "reload") {
            std::string response = send_rpc("intent.reload", "{}");
            std::cout << color::green << "  Registry reloaded." << color::reset << "\n\n";
            continue;
        }

        if (line == "help") {
            std::cout << "\n";
            std::cout << "  " << color::bold << "Commands:" << color::reset << "\n";
            std::cout << "    " << color::cyan << "<natural language>" << color::reset
                      << "  — Resolve and execute an intent\n";
            std::cout << "    " << color::cyan << "actions" << color::reset
                      << "             — List all registered actions\n";
            std::cout << "    " << color::cyan << "reload" << color::reset
                      << "              — Reload action registry\n";
            std::cout << "    " << color::cyan << "help" << color::reset
                      << "                — Show this help\n";
            std::cout << "    " << color::cyan << "quit" << color::reset
                      << "                — Exit\n\n";
            continue;
        }

        // Default: resolve and possibly execute
        run_resolve(line, false, false);
    }

    return 0;
}

static void print_usage() {
    std::cout << color::bold << "straylight-intent" << color::reset
              << " — Natural-language system commands\n\n";
    std::cout << color::bold << "USAGE:" << color::reset << "\n";
    std::cout << "    straylight-intent [OPTIONS] \"<natural language command>\"\n\n";
    std::cout << color::bold << "OPTIONS:" << color::reset << "\n";
    std::cout << "    --dry-run       Show what would happen without executing\n";
    std::cout << "    --no-confirm    Skip confirmation for destructive actions\n";
    std::cout << "    --interactive   Enter interactive mode\n";
    std::cout << "    --list-actions  List all registered actions\n";
    std::cout << "    --version       Show version\n";
    std::cout << "    --help          Show this help\n\n";
    std::cout << color::bold << "EXAMPLES:" << color::reset << "\n";
    std::cout << "    straylight-intent \"record my screen at 4k\"\n";
    std::cout << "    straylight-intent --dry-run \"encrypt file secrets.txt\"\n";
    std::cout << "    straylight-intent --no-confirm \"turn off wifi\"\n";
    std::cout << "    straylight-intent --interactive\n\n";
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    bool dry_run = false;
    bool no_confirm = false;
    bool interactive = false;
    bool list_actions = false;
    std::string text;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--no-confirm") {
            no_confirm = true;
        } else if (arg == "--interactive" || arg == "-i") {
            interactive = true;
        } else if (arg == "--list-actions" || arg == "--actions") {
            list_actions = true;
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "straylight-intent " << VERSION << "\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg[0] != '-') {
            if (!text.empty()) text += " ";
            text += arg;
        }
    }

    if (interactive) {
        return run_interactive();
    }

    if (list_actions) {
        std::string response = send_rpc("intent.actions", "{}");
        print_actions_list(response);
        return 0;
    }

    if (text.empty()) {
        std::cerr << color::red << "Error: " << color::reset
                  << "no intent text provided\n";
        print_usage();
        return 1;
    }

    return run_resolve(text, dry_run, no_confirm);
}
