/**
 * StrayLight Rewind CLI — Command-line interface for the rewind daemon.
 *
 * Commands:
 *   track <pid|name>              Begin checkpointing a process
 *   checkpoint <pid>              Create a manual checkpoint
 *   restore <pid> <checkpoint-id> Restore a process to a checkpoint
 *   list [pid]                    List checkpoints for a process (or all)
 *   untrack <pid>                 Stop tracking a process
 *   status                        Show daemon status and tracked processes
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* DAEMON_CMD_PATH  = "/var/run/straylight/rewind.sock.cmd";
const char* DAEMON_RESP_PATH = "/var/run/straylight/rewind.sock.resp";

// ── IPC with daemon ─────────────────────────────────────────────────

bool send_command(const std::string& cmd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(DAEMON_CMD_PATH).parent_path(), ec);

    std::ofstream out(DAEMON_CMD_PATH, std::ios::app);
    if (!out) {
        fprintf(stderr, "error: cannot write to %s — is straylight-rewind running?\n",
                DAEMON_CMD_PATH);
        return false;
    }
    out << cmd << "\n";
    out.close();
    return true;
}

std::string read_response(int timeout_ms = 3000) {
    // Wait for the daemon to produce a response
    namespace fs = std::filesystem;
    std::error_code ec;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) break;

        if (fs::exists(DAEMON_RESP_PATH, ec) && fs::file_size(DAEMON_RESP_PATH, ec) > 0) {
            // Small delay to let daemon finish writing
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            std::ifstream in(DAEMON_RESP_PATH);
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            in.close();

            // Clear the response file
            std::ofstream clear(DAEMON_RESP_PATH, std::ios::trunc);
            return content;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return "";
}

std::string send_and_receive(const std::string& cmd) {
    // Clear any stale response
    {
        std::ofstream clear(DAEMON_RESP_PATH, std::ios::trunc);
    }
    if (!send_command(cmd)) return "";
    return read_response();
}

// ── Resolve process name to PID ─────────────────────────────────────

pid_t resolve_target(const std::string& target) {
    // Try as numeric PID first
    try {
        int pid = std::stoi(target);
        if (pid > 0) return static_cast<pid_t>(pid);
    } catch (...) {}

    // Try as process name via /proc scan
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/proc", ec)) {
        if (!entry.is_directory()) continue;
        auto dirname = entry.path().filename().string();
        pid_t pid = 0;
        try { pid = std::stoi(dirname); }
        catch (...) { continue; }

        std::ifstream comm_file(entry.path() / "comm");
        if (!comm_file) continue;
        std::string comm;
        std::getline(comm_file, comm);
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
            comm.pop_back();

        if (comm == target) return pid;
    }

    fprintf(stderr, "error: cannot resolve '%s' to a PID\n", target.c_str());
    return -1;
}

// ── Formatting helpers ──────────────────────────────────────────────

std::string format_bytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024)
        return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
    if (bytes >= 1024ULL * 1024)
        return std::to_string(bytes / (1024ULL * 1024)) + " MB";
    if (bytes >= 1024)
        return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

// ── Commands ────────────────────────────────────────────────────────

int cmd_track(int argc, char* argv[]) {
    if (argc < 1) {
        fprintf(stderr, "usage: rewind-cli track <pid|name>\n");
        return 1;
    }
    pid_t pid = resolve_target(argv[0]);
    if (pid <= 0) return 1;

    auto resp = send_and_receive("track " + std::to_string(pid));
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }
    printf("%s", resp.c_str());
    return resp.find("OK") != std::string::npos ? 0 : 1;
}

int cmd_checkpoint(int argc, char* argv[]) {
    if (argc < 1) {
        fprintf(stderr, "usage: rewind-cli checkpoint <pid>\n");
        return 1;
    }
    pid_t pid = resolve_target(argv[0]);
    if (pid <= 0) return 1;

    printf("Creating checkpoint for pid %d...\n", pid);
    auto resp = send_and_receive("checkpoint " + std::to_string(pid));
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }
    printf("%s", resp.c_str());
    return resp.find("OK") != std::string::npos ? 0 : 1;
}

int cmd_restore(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: rewind-cli restore <pid> <checkpoint-id>\n");
        return 1;
    }
    pid_t pid = resolve_target(argv[0]);
    if (pid <= 0) return 1;
    std::string ckpt_id = argv[1];

    printf("Restoring pid %d to checkpoint %s...\n", pid, ckpt_id.c_str());
    auto resp = send_and_receive("restore " + std::to_string(pid) + " " + ckpt_id);
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }
    printf("%s", resp.c_str());
    return resp.find("OK") != std::string::npos ? 0 : 1;
}

int cmd_list(int argc, char* argv[]) {
    pid_t pid = 0;
    if (argc >= 1) {
        pid = resolve_target(argv[0]);
        if (pid <= 0) return 1;
    }

    auto resp = send_and_receive("list " + std::to_string(pid));
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }

    // Parse and format the response
    std::istringstream iss(resp);
    std::string line;
    std::getline(iss, line); // "CHECKPOINTS: N"

    printf("%-32s  %-24s  %10s  %s\n", "CHECKPOINT ID", "TIMESTAMP", "SIZE", "TYPE");
    printf("%-32s  %-24s  %10s  %s\n", "--------------------------------",
           "------------------------", "----------", "-----");

    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::istringstream lss(line);
        std::string id, timestamp, size_str, type;
        lss >> id >> timestamp >> size_str >> type;
        uint64_t size = 0;
        try { size = std::stoull(size_str); }
        catch (...) {}
        printf("%-32s  %-24s  %10s  %s\n",
               id.c_str(), timestamp.c_str(),
               format_bytes(size).c_str(), type.c_str());
    }

    return 0;
}

int cmd_untrack(int argc, char* argv[]) {
    if (argc < 1) {
        fprintf(stderr, "usage: rewind-cli untrack <pid>\n");
        return 1;
    }
    pid_t pid = resolve_target(argv[0]);
    if (pid <= 0) return 1;

    auto resp = send_and_receive("untrack " + std::to_string(pid));
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }
    printf("%s", resp.c_str());
    return resp.find("OK") != std::string::npos ? 0 : 1;
}

int cmd_status(int /*argc*/, char* /*argv*/[]) {
    auto resp = send_and_receive("status");
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon — is straylight-rewind running?\n");
        return 1;
    }

    // Parse and format
    std::istringstream iss(resp);
    std::string line;

    printf("StrayLight Rewind — Status\n");
    printf("==========================\n\n");

    while (std::getline(iss, line)) {
        if (line.substr(0, 9) == "TRACKING:") {
            auto count = line.substr(10);
            printf("Tracked processes: %s\n\n", count.c_str());
            printf("  %-8s  %-8s  %s\n", "PID", "STATE", "CHECKPOINTS");
            printf("  %-8s  %-8s  %s\n", "--------", "--------", "-----------");
        }
        else if (line.substr(0, 8) == "STORAGE:") {
            auto bytes_str = line.substr(9);
            uint64_t bytes = 0;
            try { bytes = std::stoull(bytes_str); } catch (...) {}
            printf("\nTotal storage: %s\n", format_bytes(bytes).c_str());
        }
        else if (!line.empty() && std::isdigit(line[0])) {
            std::istringstream lss(line);
            std::string pid_str, state, count;
            lss >> pid_str >> state;
            // Remainder is "N checkpoints"
            lss >> count;
            printf("  %-8s  %-8s  %s\n", pid_str.c_str(), state.c_str(), count.c_str());
        }
    }

    return 0;
}

void print_usage() {
    printf(
        "StrayLight Rewind CLI — Process checkpoint/restore\n"
        "\n"
        "Usage:\n"
        "  rewind-cli track <pid|name>              Begin checkpointing a process\n"
        "  rewind-cli checkpoint <pid>              Create a manual checkpoint now\n"
        "  rewind-cli restore <pid> <checkpoint-id> Restore process to a checkpoint\n"
        "  rewind-cli list [pid]                    List checkpoints\n"
        "  rewind-cli untrack <pid>                 Stop tracking a process\n"
        "  rewind-cli status                        Show daemon status\n"
        "\n"
        "Examples:\n"
        "  rewind-cli track 1234\n"
        "  rewind-cli track my-service\n"
        "  rewind-cli checkpoint 1234\n"
        "  rewind-cli restore 1234 ckpt-1710547200000-a3f2\n"
        "  rewind-cli list 1234\n"
    );
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];
    int sub_argc = argc - 2;
    char** sub_argv = argv + 2;

    if (cmd == "track")           return cmd_track(sub_argc, sub_argv);
    if (cmd == "checkpoint")      return cmd_checkpoint(sub_argc, sub_argv);
    if (cmd == "restore")         return cmd_restore(sub_argc, sub_argv);
    if (cmd == "list")            return cmd_list(sub_argc, sub_argv);
    if (cmd == "untrack")         return cmd_untrack(sub_argc, sub_argv);
    if (cmd == "status")          return cmd_status(sub_argc, sub_argv);
    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    fprintf(stderr, "error: unknown command '%s'\n\n", cmd.c_str());
    print_usage();
    return 1;
}
