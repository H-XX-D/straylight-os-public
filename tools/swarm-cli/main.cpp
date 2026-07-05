// tools/swarm-cli/main.cpp
//
// straylight-swarm — CLI tool for the StrayLight Swarm subsystem.
//
// Usage:
//   straylight-swarm nodes               List discovered nodes
//   straylight-swarm status              Cluster overview
//   straylight-swarm run <command>       Run on best-fit node
//   straylight-swarm spread <command>    Run on all nodes
//   straylight-swarm submit <task.json>  Submit structured task
//   straylight-swarm cancel <task-id>    Cancel a running task
//   straylight-swarm ping <node-id>      Ping a specific node
//
// In production, this tool communicates with the straylight-swarm daemon
// via D-Bus (org.straylight.Swarm1). On macOS without D-Bus, it prints
// what it would do and exits.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void print_usage() {
    std::cerr
        << "Usage: straylight-swarm <command> [args...]\n"
        << "\n"
        << "Commands:\n"
        << "  nodes                  List discovered nodes\n"
        << "  status                 Cluster overview\n"
        << "  run <command>          Run command on best-fit node\n"
        << "  spread <command>       Run command on all nodes\n"
        << "  submit <task.json>     Submit structured task from JSON file\n"
        << "  cancel <task-id>       Cancel a running task\n"
        << "  ping <node-id>         Measure latency to a node\n"
        << "  help                   Show this help\n";
}

/// Execute a D-Bus method call via busctl and return stdout.
/// On macOS where busctl is unavailable, returns an empty string.
std::pair<std::string, bool> dbus_call(const std::string& method, const std::string& args = "") {
    std::string cmd = "busctl call org.straylight.Swarm1 "
                      "/org/straylight/Swarm1 org.straylight.Swarm1 "
                      + method;
    if (!args.empty()) {
        cmd += " " + args;
    }
    cmd += " 2>/dev/null";

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {"", false};

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    int status = pclose(pipe);
    return {result, status == 0};
}

/// Read a JSON file and return its contents.
std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open file: " << path << "\n";
        return "";
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

int cmd_nodes() {
    auto [output, ok] = dbus_call("ListNodes");

    if (!ok || output.empty()) {
        // Fallback: try to parse multicast announcements directly
        std::cout << "HOSTNAME          IP ADDRESS        CORES   RAM (GiB)  GPUs   VRAM (GiB)  LOAD\n";
        std::cout << "----------------  ----------------  ------  ---------  -----  ----------  -----\n";
        std::cout << "(no D-Bus connection — daemon may not be running)\n";
        std::cout << "\nTip: Start the swarm daemon with: systemctl start straylight-swarm\n";
        return 1;
    }

    // Parse D-Bus response and format as table
    // In production the response is a structured array; here we just display it
    std::cout << "HOSTNAME          IP ADDRESS        CORES   RAM (GiB)  GPUs   VRAM (GiB)  LOAD\n";
    std::cout << "----------------  ----------------  ------  ---------  -----  ----------  -----\n";
    std::cout << output;
    return 0;
}

int cmd_status() {
    auto [output, ok] = dbus_call("ClusterStatus");

    if (!ok || output.empty()) {
        std::cout << "Swarm Cluster Status\n";
        std::cout << "====================\n";
        std::cout << "Daemon:    not reachable (D-Bus unavailable)\n";
        std::cout << "Nodes:     unknown\n";
        std::cout << "Tasks:     unknown\n";
        std::cout << "\nTip: Start the swarm daemon with: systemctl start straylight-swarm\n";
        return 1;
    }

    std::cout << "Swarm Cluster Status\n";
    std::cout << "====================\n";
    std::cout << output;
    return 0;
}

int cmd_run(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: 'run' requires a command to execute\n";
        std::cerr << "Usage: straylight-swarm run <command>\n";
        return 1;
    }

    // Join all remaining args as the command
    std::string command;
    for (int i = 2; i < argc; i++) {
        if (i > 2) command += " ";
        command += argv[i];
    }

    std::cout << "Submitting task: " << command << "\n";
    std::cout << "Strategy: gpu_affinity (best-fit)\n";

    auto [output, ok] = dbus_call("SubmitTask", "ss \"" + command + "\" \"gpu_affinity\"");

    if (!ok || output.empty()) {
        std::cerr << "Error: failed to submit task (daemon not reachable)\n";
        return 1;
    }

    std::cout << "Task submitted: " << output;
    return 0;
}

int cmd_spread(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: 'spread' requires a command to execute\n";
        std::cerr << "Usage: straylight-swarm spread <command>\n";
        return 1;
    }

    std::string command;
    for (int i = 2; i < argc; i++) {
        if (i > 2) command += " ";
        command += argv[i];
    }

    std::cout << "Submitting task to ALL nodes: " << command << "\n";
    std::cout << "Strategy: spread\n";

    auto [output, ok] = dbus_call("SubmitTask", "ss \"" + command + "\" \"spread\"");

    if (!ok || output.empty()) {
        std::cerr << "Error: failed to submit task (daemon not reachable)\n";
        return 1;
    }

    std::cout << "Task submitted: " << output;
    return 0;
}

int cmd_submit(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: 'submit' requires a task JSON file\n";
        std::cerr << "Usage: straylight-swarm submit <task.json>\n";
        return 1;
    }

    std::string json = read_file(argv[2]);
    if (json.empty()) return 1;

    std::cout << "Submitting structured task from: " << argv[2] << "\n";

    auto [output, ok] = dbus_call("SubmitStructuredTask", "s \"" + json + "\"");

    if (!ok || output.empty()) {
        std::cerr << "Error: failed to submit task (daemon not reachable)\n";
        return 1;
    }

    std::cout << "Task submitted: " << output;
    return 0;
}

int cmd_cancel(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: 'cancel' requires a task ID\n";
        std::cerr << "Usage: straylight-swarm cancel <task-id>\n";
        return 1;
    }

    std::string task_id = argv[2];
    std::cout << "Cancelling task: " << task_id << "\n";

    auto [output, ok] = dbus_call("CancelTask", "s \"" + task_id + "\"");

    if (!ok) {
        std::cerr << "Error: failed to cancel task (daemon not reachable)\n";
        return 1;
    }

    std::cout << "Task cancelled.\n";
    return 0;
}

int cmd_ping(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: 'ping' requires a node ID\n";
        std::cerr << "Usage: straylight-swarm ping <node-id>\n";
        return 1;
    }

    std::string node_id = argv[2];
    std::cout << "Pinging node: " << node_id << "\n";

    auto [output, ok] = dbus_call("PingNode", "s \"" + node_id + "\"");

    if (!ok || output.empty()) {
        std::cerr << "Error: ping failed (daemon not reachable or node not found)\n";
        return 1;
    }

    std::cout << "Response: " << output;
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "nodes")        return cmd_nodes();
    if (cmd == "status")       return cmd_status();
    if (cmd == "run")          return cmd_run(argc, argv);
    if (cmd == "spread")       return cmd_spread(argc, argv);
    if (cmd == "submit")       return cmd_submit(argc, argv);
    if (cmd == "cancel")       return cmd_cancel(argc, argv);
    if (cmd == "ping")         return cmd_ping(argc, argv);
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Error: unknown command '" << cmd << "'\n\n";
    print_usage();
    return 1;
}
