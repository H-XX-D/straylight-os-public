/**
 * bridge-cli — Command-line interface for straylight-bridge daemon.
 *
 * Commands:
 *   create <host> <name> <size> [--mode <mode>] [--encrypted]
 *   list                       List all active bridges
 *   stats <bridge-id>          Show statistics for a bridge
 *   sync <bridge-id>           Manually trigger sync
 *   destroy <bridge-id>        Destroy a bridge
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Communication with the daemon via control files.
// The bridge daemon watches /run/straylight/bridge/command and writes
// responses to /run/straylight/bridge/response.
// ---------------------------------------------------------------------------

static const std::string CMD_PATH = "/run/straylight/bridge/command";
static const std::string RESP_PATH = "/run/straylight/bridge/response";

static void print_usage() {
    fprintf(stderr,
        "Usage: bridge-cli <command> [args]\n"
        "\n"
        "Commands:\n"
        "  create <host> <name> <size> [--mode <immediate|batched|manual>] [--encrypted]\n"
        "                               Create a shared memory bridge\n"
        "  list                         List all active bridges\n"
        "  stats <bridge-id>            Show bridge statistics\n"
        "  sync <bridge-id>             Manually trigger sync\n"
        "  destroy <bridge-id>          Destroy a bridge\n"
        "\n"
        "Size format: plain bytes or with suffix (K, M, G)\n"
        "  Examples: 4096, 64K, 1M, 2G\n"
        "\n");
}

static size_t parse_size(const std::string& s) {
    if (s.empty()) return 0;

    char suffix = s.back();
    std::string num_str = s;

    size_t multiplier = 1;
    if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024;
        num_str = s.substr(0, s.size() - 1);
    } else if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024 * 1024;
        num_str = s.substr(0, s.size() - 1);
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024ULL * 1024 * 1024;
        num_str = s.substr(0, s.size() - 1);
    }

    return static_cast<size_t>(std::stoull(num_str)) * multiplier;
}

static std::string format_size(size_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        return std::to_string(bytes / (1024ULL * 1024 * 1024)) + "G";
    } else if (bytes >= 1024 * 1024) {
        return std::to_string(bytes / (1024 * 1024)) + "M";
    } else if (bytes >= 1024) {
        return std::to_string(bytes / 1024) + "K";
    }
    return std::to_string(bytes) + "B";
}

static bool send_command(const std::string& cmd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("/run/straylight/bridge", ec);

    // Remove old response.
    fs::remove(RESP_PATH, ec);

    // Write command.
    std::ofstream cf(CMD_PATH);
    if (!cf) {
        fprintf(stderr, "Error: cannot write to %s\n", CMD_PATH.c_str());
        fprintf(stderr, "Is the straylight-bridge daemon running?\n");
        return false;
    }
    cf << cmd << "\n";
    cf.close();
    return true;
}

static std::string read_response(int timeout_ms = 3000) {
    namespace fs = std::filesystem;

    // Poll for response file.
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (fs::exists(RESP_PATH)) {
            // Small delay to ensure write is complete.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            std::ifstream rf(RESP_PATH);
            std::ostringstream oss;
            oss << rf.rdbuf();
            return oss.str();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        elapsed += 100;
    }

    return "";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static int cmd_create(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: bridge-cli create <host> <name> <size> [--mode <m>] [--encrypted]\n");
        return 1;
    }

    std::string host = argv[2];
    std::string name = argv[3];
    size_t size = parse_size(argv[4]);
    std::string mode = "batched";
    bool encrypted = false;

    for (int i = 5; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--encrypted") {
            encrypted = true;
        }
    }

    if (size == 0) {
        fprintf(stderr, "Invalid size: %s\n", argv[4]);
        return 1;
    }

    printf("Creating bridge:\n");
    printf("  Remote host : %s\n", host.c_str());
    printf("  Region name : %s\n", name.c_str());
    printf("  Size        : %s (%zu bytes)\n", format_size(size).c_str(), size);
    printf("  Sync mode   : %s\n", mode.c_str());
    printf("  Encrypted   : %s\n", encrypted ? "yes" : "no");

    std::ostringstream cmd;
    cmd << "create " << host << " " << name << " " << size << " " << mode;

    if (!send_command(cmd.str())) return 1;

    std::string response = read_response();
    if (response.empty()) {
        fprintf(stderr, "No response from daemon (timeout). Is straylight-bridge running?\n");
        return 1;
    }

    printf("\n%s", response.c_str());
    return 0;
}

static int cmd_list() {
    printf("Active Bridges\n");
    printf("==============\n");

    if (!send_command("list")) return 1;

    std::string response = read_response();
    if (response.empty()) {
        printf("  (no bridges or daemon not running)\n");
        return 0;
    }

    printf("%-4s  %-20s  %-25s  %10s  %-10s\n",
           "ID", "Name", "Remote", "Size", "Mode");
    printf("%-4s  %-20s  %-25s  %10s  %-10s\n",
           "--", "----", "------", "----", "----");

    std::istringstream iss(response);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        std::istringstream ls(line);
        uint32_t id;
        std::string name, remote;
        size_t size;
        std::string mode;
        ls >> id >> name >> remote >> size >> mode;

        printf("%-4u  %-20s  %-25s  %10s  %-10s\n",
               id, name.c_str(), remote.c_str(),
               format_size(size).c_str(), mode.c_str());
    }

    return 0;
}

static int cmd_stats(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: bridge-cli stats <bridge-id>\n");
        return 1;
    }

    uint32_t id = static_cast<uint32_t>(std::stoul(argv[2]));

    if (!send_command("stats " + std::to_string(id))) return 1;

    std::string response = read_response();
    if (response.empty()) {
        fprintf(stderr, "No response from daemon\n");
        return 1;
    }

    if (response.find("error:") != std::string::npos) {
        fprintf(stderr, "%s", response.c_str());
        return 1;
    }

    // Parse key=value pairs.
    printf("Bridge Statistics\n");
    printf("=================\n");

    std::istringstream iss(response);
    std::string token;
    while (iss >> token) {
        auto eq = token.find('=');
        if (eq == std::string::npos) continue;

        std::string key = token.substr(0, eq);
        std::string val = token.substr(eq + 1);

        // Format nicely.
        if (key == "id")        printf("  Bridge ID       : %s\n", val.c_str());
        else if (key == "name") printf("  Region Name     : %s\n", val.c_str());
        else if (key == "size") printf("  Region Size     : %s\n", format_size(std::stoull(val)).c_str());
        else if (key == "syncs") printf("  Total Syncs     : %s\n", val.c_str());
        else if (key == "pages") printf("  Pages Synced    : %s\n", val.c_str());
        else if (key == "bytes") printf("  Bytes Synced    : %s\n", format_size(std::stoull(val)).c_str());
        else if (key == "dirty") printf("  Dirty Pages     : %s\n", val.c_str());
        else if (key == "latency") printf("  Avg Latency     : %s\n", val.c_str());
        else if (key == "connected") printf("  Connected       : %s\n", val.c_str());
        else if (key == "uptime") printf("  Uptime          : %s\n", val.c_str());
    }

    return 0;
}

static int cmd_sync(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: bridge-cli sync <bridge-id>\n");
        return 1;
    }

    uint32_t id = static_cast<uint32_t>(std::stoul(argv[2]));

    printf("Triggering manual sync for bridge %u...\n", id);

    if (!send_command("sync " + std::to_string(id))) return 1;

    std::string response = read_response();
    if (response.empty()) {
        fprintf(stderr, "No response from daemon\n");
        return 1;
    }

    printf("%s", response.c_str());
    return 0;
}

static int cmd_destroy(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: bridge-cli destroy <bridge-id>\n");
        return 1;
    }

    uint32_t id = static_cast<uint32_t>(std::stoul(argv[2]));

    printf("Destroying bridge %u...\n", id);

    if (!send_command("destroy " + std::to_string(id))) return 1;

    std::string response = read_response();
    if (response.empty()) {
        fprintf(stderr, "No response from daemon\n");
        return 1;
    }

    printf("%s", response.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "create") {
        return cmd_create(argc, argv);
    } else if (cmd == "list") {
        return cmd_list();
    } else if (cmd == "stats") {
        return cmd_stats(argc, argv);
    } else if (cmd == "sync") {
        return cmd_sync(argc, argv);
    } else if (cmd == "destroy") {
        return cmd_destroy(argc, argv);
    } else if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
        print_usage();
        return 1;
    }
}
