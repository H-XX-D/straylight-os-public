// tools/dash/main.cpp
// straylight-dash — full-screen terminal dashboard for StrayLight OS.
// Pure ANSI escape sequences, reads from /proc/ and /sys/.

#include "tui.h"
#include "widgets.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

static void print_usage() {
    std::cerr
        << "straylight-dash -- terminal dashboard for StrayLight OS\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-dash                    Launch dashboard\n"
        << "  straylight-dash --help             Show this help\n"
        << "\n"
        << "Keyboard:\n"
        << "  Tab       Switch active panel\n"
        << "  q         Quit\n"
        << "  s         Toggle process sort (CPU / MEM)\n"
        << "  k         Kill selected process (sends SIGTERM)\n"
        << "  Up/Down   Scroll process list\n";
}

/// Compute responsive panel layout based on terminal size.
struct Layout {
    straylight::Rect cpu;
    straylight::Rect mem;
    straylight::Rect gpu;
    straylight::Rect disk;
    straylight::Rect net;
    straylight::Rect procs;
    straylight::Rect services;
    straylight::Rect alice;
    straylight::Rect status_bar;

    void compute(int cols, int rows) {
        // Top row: CPU (left half) + Memory (right half).
        int half_w = cols / 2;
        int top_h = std::min(rows / 4, 12);
        cpu = {0, 0, half_w, top_h};
        mem = {half_w, 0, cols - half_w, top_h};

        // Middle row: GPU (left third) + Disk (middle third) + Network (right third).
        int third_w = cols / 3;
        int mid_h = std::min(rows / 4, 10);
        gpu = {0, top_h, third_w, mid_h};
        disk = {third_w, top_h, third_w, mid_h};
        net = {third_w * 2, top_h, cols - third_w * 2, mid_h};

        // Bottom: Processes (left 60%) + Services + Alice (right 40%).
        int bot_y = top_h + mid_h;
        int bot_h = rows - bot_y - 1; // -1 for status bar.
        int proc_w = cols * 3 / 5;
        int side_w = cols - proc_w;
        int svc_h = bot_h / 2;
        int alice_h = bot_h - svc_h;

        procs = {0, bot_y, proc_w, bot_h};
        services = {proc_w, bot_y, side_w, svc_h};
        alice = {proc_w, bot_y + svc_h, side_w, alice_h};

        // Status bar at the bottom.
        status_bar = {0, rows - 1, cols, 1};
    }
};

int main(int argc, char* argv[]) {
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 ||
                       std::strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }

    straylight::TUI tui;
    auto init_res = tui.init();
    if (!init_res.has_value()) {
        std::cerr << "Error: " << init_res.error() << "\n";
        return 1;
    }

    tui.install_resize_handler();

    straylight::Widgets widgets;
    Layout layout;
    int sort_col = 0; // 0=CPU, 1=MEM
    int active_panel = 0;
    bool running = true;

    while (running) {
        // Get terminal size and compute layout.
        auto ts = tui.size();
        layout.compute(ts.cols, ts.rows);

        // Collect data.
        widgets.refresh();

        // Clear and render.
        tui.clear();

        widgets.render_cpu(tui, layout.cpu);
        widgets.render_memory(tui, layout.mem);
        widgets.render_gpu(tui, layout.gpu);
        widgets.render_disk(tui, layout.disk);
        widgets.render_network(tui, layout.net);
        widgets.render_processes(tui, layout.procs, sort_col);
        widgets.render_services(tui, layout.services);
        widgets.render_alice(tui, layout.alice);

        // Status bar.
        tui.print_at(0, layout.status_bar.y,
                     std::string(straylight::Color::BgBlue) +
                     std::string(straylight::Color::White) +
                     " straylight-dash " +
                     straylight::Color::Reset +
                     " Tab:panel  s:sort  k:kill  q:quit" +
                     std::string(ts.cols - 50, ' '));

        tui.flush();

        // Wait ~1 second, checking for keypresses every 50ms.
        for (int t = 0; t < 20 && running; ++t) {
            int key = tui.read_key();
            if (key == straylight::TUI::KEY_Q) {
                running = false;
                break;
            } else if (key == straylight::TUI::KEY_TAB) {
                active_panel = (active_panel + 1) % 8;
            } else if (key == straylight::TUI::KEY_S || key == 'S') {
                sort_col = (sort_col + 1) % 2;
            } else if (key == straylight::TUI::KEY_K || key == 'K') {
                // Kill the top process in the list.
                const auto& procs = widgets.processes();
                if (!procs.empty()) {
                    kill(procs[0].pid, SIGTERM);
                }
            } else if (key == straylight::TUI::KEY_ESC) {
                running = false;
                break;
            }

            if (tui.was_resized()) {
                break; // Immediately re-render.
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    tui.shutdown();
    return 0;
}
