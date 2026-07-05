/**
 * mirror-cli — Command-line interface for straylight-mirror daemon.
 *
 * Commands:
 *   start <target>   Start mirroring to target host
 *   stop             Stop active mirror session
 *   status           Show mirror session status
 *   resume           Resume interrupted mirror session
 *   verify           Verify mirror integrity
 */

#include "../../services/mirror/mirror_engine.h"
#include "../../services/mirror/state_capture.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace straylight::mirror;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void print_usage() {
    fprintf(stderr,
        "Usage: mirror-cli <command> [args]\n"
        "\n"
        "Commands:\n"
        "  start <target> [--port <p>] [--bandwidth <mbps>]  Start mirror to target\n"
        "  stop                     Stop active mirror session\n"
        "  status                   Show mirror session status\n"
        "  resume                   Resume interrupted session\n"
        "  verify                   Verify mirror integrity\n"
        "\n");
}

static std::string read_file_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f) std::getline(f, line);
    return line;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static int cmd_start(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: mirror-cli start <target-host> [--port <p>] [--bandwidth <mbps>]\n");
        return 1;
    }

    std::string target = argv[2];
    uint16_t port = 9900;
    double bandwidth = 0.0;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bandwidth" && i + 1 < argc) {
            bandwidth = std::stod(argv[++i]);
        }
    }

    printf("Starting mirror to %s:%d", target.c_str(), port);
    if (bandwidth > 0.0) printf(" (bandwidth limit: %.1f Mbps)", bandwidth);
    printf("\n");

    MirrorEngine engine;
    if (bandwidth > 0.0) {
        engine.set_bandwidth_limit_mbps(bandwidth);
    }

    auto result = engine.start_mirror(target, port);
    if (!result) {
        fprintf(stderr, "Error: %s\n", result.err().c_str());
        return 1;
    }

    // Poll progress until complete.
    printf("Mirror session started. Streaming...\n\n");

    while (engine.is_active()) {
        auto progress = engine.get_progress();
        printf("\r  Phase: %-20s  Progress: %6.1f%%  Synced: %6lluMB  "
               "Files: %llu/%llu  Elapsed: %.0fs",
               phase_to_string(progress.phase),
               progress.percent_complete(),
               static_cast<unsigned long long>(progress.synced_bytes / (1024 * 1024)),
               static_cast<unsigned long long>(progress.files_synced),
               static_cast<unsigned long long>(progress.files_total),
               progress.elapsed_seconds);
        fflush(stdout);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    auto final_progress = engine.get_progress();
    printf("\n\n");

    if (final_progress.phase == MirrorPhase::Complete) {
        printf("Mirror completed successfully!\n");
        printf("  Total synced: %llu MB\n",
               static_cast<unsigned long long>(final_progress.synced_bytes / (1024 * 1024)));
        printf("  Files: %llu\n",
               static_cast<unsigned long long>(final_progress.files_synced));
        printf("  Duration: %.1f seconds\n", final_progress.elapsed_seconds);
        return 0;
    } else {
        fprintf(stderr, "Mirror failed: %s\n", final_progress.error.c_str());
        return 1;
    }
}

static int cmd_stop() {
    // Signal the running daemon to stop.
    std::string pid_path = "/var/run/straylight/straylight-mirror.pid";
    std::ifstream pidf(pid_path);
    if (!pidf) {
        fprintf(stderr, "No active mirror session found (no PID file)\n");
        return 1;
    }

    int pid = 0;
    pidf >> pid;
    if (pid <= 0) {
        fprintf(stderr, "Invalid PID in %s\n", pid_path.c_str());
        return 1;
    }

    if (kill(pid, SIGTERM) == 0) {
        printf("Sent stop signal to straylight-mirror (pid %d)\n", pid);
        return 0;
    } else {
        fprintf(stderr, "Failed to signal daemon (pid %d): %s\n", pid, strerror(errno));
        return 1;
    }
}

static int cmd_status() {
    printf("Mirror Session Status\n");
    printf("=====================\n");

    // Check if daemon is running.
    std::string pid_path = "/var/run/straylight/straylight-mirror.pid";
    std::ifstream pidf(pid_path);
    if (!pidf) {
        printf("  Status: not running\n");
        return 0;
    }

    int pid = 0;
    pidf >> pid;
    bool running = (pid > 0 && kill(pid, 0) == 0);

    printf("  Daemon PID : %d (%s)\n", pid, running ? "running" : "dead");

    // Read state file if available.
    std::string state_path = "/var/run/straylight/straylight-mirror.state";
    if (std::filesystem::exists(state_path)) {
        std::string state = read_file_line(state_path);
        printf("  State      : %s\n", state.c_str());
    }

    // Show transfer progress from runtime file.
    std::string progress_path = "/var/run/straylight/mirror-progress";
    if (std::filesystem::exists(progress_path)) {
        std::ifstream pf(progress_path);
        std::string line;
        while (std::getline(pf, line)) {
            printf("  %s\n", line.c_str());
        }
    }

    return 0;
}

static int cmd_resume() {
    printf("Resuming mirror session...\n");

    // Check for saved session state.
    std::string session_path = "/var/lib/straylight/mirror/session.state";
    if (!std::filesystem::exists(session_path)) {
        fprintf(stderr, "No saved session to resume. Start a new mirror with 'mirror-cli start <target>'\n");
        return 1;
    }

    std::ifstream sf(session_path);
    std::string target_host;
    uint16_t target_port = 9900;
    if (sf) {
        sf >> target_host >> target_port;
    }

    if (target_host.empty()) {
        fprintf(stderr, "Invalid saved session state\n");
        return 1;
    }

    printf("Resuming mirror to %s:%d\n", target_host.c_str(), target_port);

    MirrorEngine engine;
    auto result = engine.start_mirror(target_host, target_port);
    if (!result) {
        fprintf(stderr, "Error: %s\n", result.err().c_str());
        return 1;
    }

    // Poll until done.
    while (engine.is_active()) {
        auto progress = engine.get_progress();
        printf("\r  Phase: %-20s  Progress: %6.1f%%",
               phase_to_string(progress.phase),
               progress.percent_complete());
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    printf("\nResume complete.\n");
    return 0;
}

static int cmd_verify() {
    printf("Verifying mirror integrity...\n");

    MirrorEngine engine;
    auto result = engine.verify();
    if (!result) {
        fprintf(stderr, "Verification error: %s\n", result.err().c_str());
        return 1;
    }

    if (result.value()) {
        printf("Verification PASSED: all files match\n");
        return 0;
    } else {
        printf("Verification FAILED: mismatches detected\n");
        return 1;
    }
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

    if (cmd == "start") {
        return cmd_start(argc, argv);
    } else if (cmd == "stop") {
        return cmd_stop();
    } else if (cmd == "status") {
        return cmd_status();
    } else if (cmd == "resume") {
        return cmd_resume();
    } else if (cmd == "verify") {
        return cmd_verify();
    } else if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
        print_usage();
        return 1;
    }
}
