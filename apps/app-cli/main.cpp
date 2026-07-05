// apps/app-cli/main.cpp
// Native multicall CLI for StrayLight desktop applications.

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct AppSpec {
    std::string_view slug;
    std::string_view binary;
    std::string_view title;
    std::string_view category;
    std::string_view summary;
    std::string_view related_cli;
};

const std::vector<AppSpec>& app_catalog() {
    static const std::vector<AppSpec> apps = {
        {"terminal", "straylight-terminal", "Terminal", "desktop", "Terminal emulator with StrayLight theme integration.", ""},
        {"files", "straylight-files", "Files", "desktop", "Graphical file manager and local filesystem browser.", ""},
        {"sysmon", "straylight-sysmon", "System Monitor", "desktop", "Graphical system monitor for CPU, memory, process, and device status.", ""},
        {"settings", "straylight-settings", "Settings", "desktop", "Main graphical settings application.", ""},
        {"oobe", "straylight-oobe", "OOBE", "first-run", "First-run onboarding experience.", ""},
        {"wizard", "straylight-wizard", "Wizard", "first-run", "Setup wizard for layout, theme, and developer defaults.", ""},
        {"widget-launcher", "straylight-widget-launcher", "Widget Launcher", "desktop", "Launcher for registered StrayLight widgets.", ""},
        {"audio-gui", "straylight-audio-gui", "Audio GUI", "hardware", "Graphical audio routing and device panel.", "straylight-audio-cli"},
        {"audio-mixer", "straylight-audio-mixer", "Audio Mixer", "hardware", "PipeWire-backed mixer for streams, sinks, and sources.", "straylight-audio-cli"},
        {"bench-gui", "straylight-bench-gui", "Bench GUI", "development", "Graphical benchmark runner and result viewer.", "straylight-bench"},
        {"boot-gui", "straylight-boot-gui", "Boot GUI", "system", "Graphical boot environment and startup configuration panel.", "straylight-boot"},
        {"color-gui", "straylight-color-gui", "Color GUI", "desktop", "Graphical color profile and display color management panel.", "straylight-color"},
        {"cron-gui", "straylight-cron-gui", "Cron GUI", "workflow", "Graphical scheduler and job dependency panel.", "straylight-cron-cli"},
        {"disk-gui", "straylight-disk-gui", "Disk GUI", "hardware", "Graphical storage, SMART, and partitioning panel.", "straylight-disk"},
        {"display-gui", "straylight-display-gui", "Display GUI", "hardware", "Graphical display layout, refresh, and scaling panel.", "straylight-display"},
        {"fabric-gui", "straylight-fabric-gui", "Fabric GUI", "hardware", "Graphical hardware fabric and topology viewer.", "straylight-fabric-cli"},
        {"flux-gui", "straylight-flux-gui", "Flux GUI", "workflow", "Graphical stream processing and event flow panel.", "straylight-flux-cli"},
        {"fonts-gui", "straylight-fonts-gui", "Fonts GUI", "desktop", "Graphical font discovery and rendering panel.", "straylight-fonts"},
        {"health-gui", "straylight-health-gui", "Health GUI", "system", "Graphical health dashboard and check viewer.", "straylight-health-cli"},
        {"hub", "straylight-hub", "Hub", "desktop", "Central StrayLight dashboard for services, devices, and alerts.", ""},
        {"input-gui", "straylight-input-gui", "Input GUI", "hardware", "Graphical keyboard, pointer, tablet, and gamepad panel.", "straylight-input"},
        {"intent-gui", "straylight-intent-gui", "Intent GUI", "ai", "Graphical natural-language command and preview panel.", "straylight-intent-cli"},
        {"logview-gui", "straylight-logview-gui", "Log Viewer GUI", "system", "Graphical structured log viewer.", "straylight-log"},
        {"mesh-gui", "straylight-mesh-gui", "Mesh GUI", "network", "Graphical distributed compute mesh panel.", "straylight-mesh-cli"},
        {"migrate-gui", "straylight-migrate-gui", "Migrate GUI", "recovery", "Graphical system migration panel.", "straylight-migrate"},
        {"morph-gui", "straylight-morph-gui", "Morph GUI", "ml", "Graphical model and workload morphing panel.", ""},
        {"network-gui", "straylight-network-gui", "Network GUI", "network", "Graphical network interface, DNS, route, and VPN panel.", "straylight-network"},
        {"notify-gui", "straylight-notify-gui", "Notify GUI", "desktop", "Graphical notification routing and do-not-disturb panel.", "straylight-notify-cli"},
        {"photonics-gui", "straylight-photonics-gui", "Photonics GUI", "exotic", "Graphical photonics subsystem panel.", ""},
        {"pmem-gui", "straylight-pmem-gui", "Persistent Memory GUI", "exotic", "Graphical persistent-memory and tiering panel.", ""},
        {"policy-gui", "straylight-policy-gui", "Policy GUI", "security", "Graphical policy and role management panel.", "straylight-policy-cli"},
        {"power-gui", "straylight-power-gui", "Power GUI", "system", "Graphical power policy and battery panel.", "straylight-power-cli"},
        {"predict-gui", "straylight-predict-gui", "Predict GUI", "ai", "Graphical predictive preload and forecast panel.", "straylight-predict-cli"},
        {"probe-gui", "straylight-probe-gui", "Probe GUI", "network", "Graphical scan and diagnostics panel.", "straylight-probe"},
        {"quantum-gui", "straylight-quantum-gui", "Quantum GUI", "exotic", "Graphical quantum simulation and circuit panel.", ""},
        {"replay-gui", "straylight-replay-gui", "Replay GUI", "development", "Graphical flight-recorder replay panel.", "straylight-replay-cli"},
        {"rewind-gui", "straylight-rewind-gui", "Rewind GUI", "development", "Graphical checkpoint and time-travel debug panel.", "straylight-rewind-cli"},
        {"rhem-gui", "straylight-rhem-gui", "RHEM GUI", "ml", "Graphical heterogeneous memory policy panel.", ""},
        {"sandbox-gui", "straylight-sandbox-gui", "Sandbox GUI", "security", "Graphical sandbox and isolation policy panel.", "straylight-sandbox"},
        {"shield-gui", "straylight-shield-gui", "Shield GUI", "security", "Graphical security audit and hardening panel.", "straylight-shield"},
        {"snapshot-gui", "straylight-snapshot-gui", "Snapshot GUI", "recovery", "Graphical snapshot and restore panel.", "straylight-snapshot"},
        {"snn-gui", "straylight-snn-gui", "SNN GUI", "ml", "Graphical spiking neural network panel.", ""},
        {"update-gui", "straylight-update-gui", "Update GUI", "system", "Graphical system update and rollback panel.", "straylight-update"},
        {"users-gui", "straylight-users-gui", "Users GUI", "security", "Graphical user and group management panel.", "straylight-users"},
        {"vault-gui", "straylight-vault-gui", "Vault GUI", "security", "Graphical secrets and key management panel.", "straylight-vault"},
    };
    return apps;
}

std::string base_name(const std::string& path) {
    const auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::string cli_name(const AppSpec& app) {
    return std::string(app.binary) + "-cli";
}

std::string normalize_token(std::string token) {
    if (ends_with(token, "-cli")) {
        token.resize(token.size() - 4);
    }
    if (starts_with(token, "straylight-")) {
        return token;
    }
    return "straylight-" + token;
}

const AppSpec* find_app(std::string_view token) {
    const std::string raw(token);
    const std::string normalized = normalize_token(raw);
    for (const auto& app : app_catalog()) {
        if (raw == app.slug || raw == app.binary || raw == cli_name(app) ||
            normalized == app.binary || normalized == cli_name(app)) {
            return &app;
        }
    }
    return nullptr;
}

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u";
                    const char* hex = "0123456789abcdef";
                    out << "00" << hex[(ch >> 4) & 0xf] << hex[ch & 0xf];
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

std::optional<std::string> find_executable(std::string_view binary) {
    const std::string name(binary);
    if (name.find('/') != std::string::npos) {
        if (access(name.c_str(), X_OK) == 0) {
            return name;
        }
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    std::vector<std::string> dirs;
    if (path_env != nullptr) {
        std::stringstream ss(path_env);
        std::string item;
        while (std::getline(ss, item, ':')) {
            if (!item.empty()) {
                dirs.push_back(item);
            }
        }
    }
    dirs.push_back("/usr/bin");
    dirs.push_back("/usr/local/bin");

    for (const auto& dir : dirs) {
        const std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool has_graphical_session() {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* display = std::getenv("DISPLAY");
    return (wayland != nullptr && wayland[0] != '\0') ||
           (display != nullptr && display[0] != '\0');
}

std::string session_label() {
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* display = std::getenv("DISPLAY");
    if (wayland != nullptr && wayland[0] != '\0') {
        return std::string("Wayland: ") + wayland;
    }
    if (display != nullptr && display[0] != '\0') {
        return std::string("X11: ") + display;
    }
    return "none";
}

void print_global_help() {
    std::cout
        << "straylight-app-cli - StrayLight app launcher and help CLI\n\n"
        << "Usage:\n"
        << "  straylight-app-cli list [--json]\n"
        << "  straylight-app-cli help [APP]\n"
        << "  straylight-app-cli <APP> <command> [args]\n"
        << "  straylight-<app>-cli <command> [args]\n\n"
        << "Commands for each app:\n"
        << "  help                 Show app-specific help\n"
        << "  info [--json]        Show app metadata\n"
        << "  doctor [--json]      Check executable, display session, docs, and related CLI\n"
        << "  path                 Print the executable path if found\n"
        << "  open [-- args...]    Start the graphical app in the background\n"
        << "  exec [-- args...]    Replace the CLI process with the app\n"
        << "  doc                  Print the installed app CLI documentation path\n\n"
        << "Examples:\n"
        << "  straylight-app-cli list\n"
        << "  straylight-app-cli health-gui doctor\n"
        << "  straylight-health-gui-cli open\n"
        << "  straylight-vault-gui-cli --help\n";
}

void print_app_help(const AppSpec& app) {
    std::cout
        << cli_name(app) << " - " << app.title << "\n\n"
        << app.summary << "\n\n"
        << "Usage:\n"
        << "  " << cli_name(app) << " help\n"
        << "  " << cli_name(app) << " info [--json]\n"
        << "  " << cli_name(app) << " doctor [--json]\n"
        << "  " << cli_name(app) << " path\n"
        << "  " << cli_name(app) << " open [-- app-args...]\n"
        << "  " << cli_name(app) << " exec [-- app-args...]\n"
        << "  " << cli_name(app) << " doc\n\n"
        << "App executable: " << app.binary << "\n"
        << "Category:       " << app.category << "\n";
    if (!app.related_cli.empty()) {
        std::cout << "Related CLI:    " << app.related_cli << "\n";
    }
    std::cout << "\nExit codes:\n"
              << "  0 success\n"
              << "  1 runtime error\n"
              << "  2 usage error\n";
}

void print_app_json(const AppSpec& app) {
    std::cout
        << "{"
        << "\"slug\":\"" << json_escape(app.slug) << "\","
        << "\"binary\":\"" << json_escape(app.binary) << "\","
        << "\"cli\":\"" << json_escape(cli_name(app)) << "\","
        << "\"title\":\"" << json_escape(app.title) << "\","
        << "\"category\":\"" << json_escape(app.category) << "\","
        << "\"summary\":\"" << json_escape(app.summary) << "\","
        << "\"related_cli\":\"" << json_escape(app.related_cli) << "\""
        << "}\n";
}

void print_app_info(const AppSpec& app) {
    std::cout
        << "App:         " << app.title << "\n"
        << "Slug:        " << app.slug << "\n"
        << "Binary:      " << app.binary << "\n"
        << "CLI:         " << cli_name(app) << "\n"
        << "Category:    " << app.category << "\n"
        << "Summary:     " << app.summary << "\n";
    if (!app.related_cli.empty()) {
        std::cout << "Related CLI: " << app.related_cli << "\n";
    }
}

void print_list(bool json) {
    if (json) {
        std::cout << "[\n";
        const auto& apps = app_catalog();
        for (size_t i = 0; i < apps.size(); ++i) {
            const auto& app = apps[i];
            std::cout
                << "  {"
                << "\"slug\":\"" << json_escape(app.slug) << "\","
                << "\"binary\":\"" << json_escape(app.binary) << "\","
                << "\"cli\":\"" << json_escape(cli_name(app)) << "\","
                << "\"title\":\"" << json_escape(app.title) << "\","
                << "\"category\":\"" << json_escape(app.category) << "\""
                << "}";
            if (i + 1 < apps.size()) {
                std::cout << ",";
            }
            std::cout << "\n";
        }
        std::cout << "]\n";
        return;
    }

    std::cout << "StrayLight app CLIs\n\n";
    for (const auto& app : app_catalog()) {
        std::cout << "  " << cli_name(app) << "\n"
                  << "      " << app.title << " - " << app.summary << "\n";
    }
}

std::vector<std::string> collect_app_args(int argc, char* argv[], int start) {
    std::vector<std::string> args;
    bool pass_through = false;
    for (int i = start; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!pass_through && arg == "--") {
            pass_through = true;
            continue;
        }
        args.push_back(arg);
    }
    return args;
}

int open_app(const AppSpec& app, const std::vector<std::string>& args) {
    auto executable = find_executable(app.binary);
    if (!executable.has_value()) {
        std::cerr << "Error: " << app.binary << " was not found in PATH.\n";
        return 1;
    }

    std::vector<std::string> storage;
    storage.push_back(*executable);
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> exec_args;
    exec_args.reserve(storage.size() + 1);
    for (auto& value : storage) {
        exec_args.push_back(value.data());
    }
    exec_args.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Error: fork failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0) {
        setsid();
        execvp(exec_args[0], exec_args.data());
        std::cerr << "Error: exec failed: " << std::strerror(errno) << "\n";
        _exit(127);
    }

    std::cout << "Started " << app.binary << " (pid " << pid << ")\n";
    return 0;
}

int exec_app(const AppSpec& app, const std::vector<std::string>& args) {
    auto executable = find_executable(app.binary);
    if (!executable.has_value()) {
        std::cerr << "Error: " << app.binary << " was not found in PATH.\n";
        return 1;
    }

    std::vector<std::string> storage;
    storage.push_back(*executable);
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> exec_args;
    exec_args.reserve(storage.size() + 1);
    for (auto& value : storage) {
        exec_args.push_back(value.data());
    }
    exec_args.push_back(nullptr);

    execvp(exec_args[0], exec_args.data());
    std::cerr << "Error: exec failed: " << std::strerror(errno) << "\n";
    return 1;
}

int print_doctor(const AppSpec& app, bool json) {
    const auto executable = find_executable(app.binary);
    const bool display_ok = has_graphical_session();
    if (json) {
        std::cout
            << "{"
            << "\"app\":\"" << json_escape(app.slug) << "\","
            << "\"binary\":\"" << json_escape(app.binary) << "\","
            << "\"executable_found\":" << (executable.has_value() ? "true" : "false") << ","
            << "\"executable_path\":\"" << json_escape(executable.value_or("")) << "\","
            << "\"graphical_session\":" << (display_ok ? "true" : "false") << ","
            << "\"session\":\"" << json_escape(session_label()) << "\","
            << "\"docs\":\"/usr/share/doc/straylight-desktop/straylight-app-clis.md\","
            << "\"related_cli\":\"" << json_escape(app.related_cli) << "\""
            << "}\n";
        return executable.has_value() ? 0 : 1;
    }

    std::cout << app.title << " doctor\n\n";
    std::cout << "Executable: ";
    if (executable.has_value()) {
        std::cout << *executable << " [ok]\n";
    } else {
        std::cout << app.binary << " [missing]\n";
    }
    std::cout << "Display:    " << session_label() << (display_ok ? " [ok]" : " [not set]") << "\n";
    std::cout << "Docs:       /usr/share/doc/straylight-desktop/straylight-app-clis.md\n";
    if (!app.related_cli.empty()) {
        std::cout << "Related:    " << app.related_cli << "\n";
    }
    return executable.has_value() ? 0 : 1;
}

void print_doc_path(const AppSpec& app) {
    std::cout
        << "Documentation: /usr/share/doc/straylight-desktop/straylight-app-clis.md\n"
        << "Section:       " << cli_name(app) << "\n"
        << "Fallback:      /usr/share/doc/straylight/app-clis.md\n";
}

bool has_json_flag(int argc, char* argv[], int start) {
    for (int i = start; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--json") {
            return true;
        }
    }
    return false;
}

int dispatch_app(const AppSpec& app, int argc, char* argv[], int index) {
    if (index >= argc) {
        print_app_help(app);
        return 0;
    }

    const std::string command = argv[index];
    if (command == "--help" || command == "-h" || command == "help") {
        print_app_help(app);
        return 0;
    }
    if (command == "info") {
        if (has_json_flag(argc, argv, index + 1)) {
            print_app_json(app);
        } else {
            print_app_info(app);
        }
        return 0;
    }
    if (command == "doctor") {
        return print_doctor(app, has_json_flag(argc, argv, index + 1));
    }
    if (command == "path") {
        auto executable = find_executable(app.binary);
        if (!executable.has_value()) {
            std::cerr << "Error: " << app.binary << " was not found in PATH.\n";
            return 1;
        }
        std::cout << *executable << "\n";
        return 0;
    }
    if (command == "open" || command == "launch" || command == "start") {
        return open_app(app, collect_app_args(argc, argv, index + 1));
    }
    if (command == "exec" || command == "run") {
        return exec_app(app, collect_app_args(argc, argv, index + 1));
    }
    if (command == "doc" || command == "docs") {
        print_doc_path(app);
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "' for " << cli_name(app) << "\n\n";
    print_app_help(app);
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string invoked = base_name(argc > 0 ? argv[0] : "straylight-app-cli");
    const bool global_invocation = invoked == "straylight-app-cli";

    if (!global_invocation) {
        const AppSpec* app = find_app(invoked);
        if (app == nullptr) {
            std::cerr << "Error: unknown app CLI name '" << invoked << "'\n";
            return 2;
        }
        return dispatch_app(*app, argc, argv, 1);
    }

    if (argc < 2) {
        print_global_help();
        return 0;
    }

    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        if (argc >= 3) {
            const AppSpec* app = find_app(argv[2]);
            if (app == nullptr) {
                std::cerr << "Error: unknown app '" << argv[2] << "'\n";
                return 2;
            }
            print_app_help(*app);
            return 0;
        }
        print_global_help();
        return 0;
    }
    if (command == "--version" || command == "version") {
        std::cout << "straylight-app-cli 1.0.0\n";
        return 0;
    }
    if (command == "list") {
        print_list(has_json_flag(argc, argv, 2));
        return 0;
    }

    const AppSpec* app = find_app(command);
    if (app != nullptr) {
        return dispatch_app(*app, argc, argv, 2);
    }

    std::cerr << "Error: unknown command or app '" << command << "'\n\n";
    print_global_help();
    return 2;
}
