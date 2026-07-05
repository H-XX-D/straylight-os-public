// tools/timeline/collectors.h
// Data collectors for the StrayLight timeline.
#pragma once

#include "event_store.h"

#include <string>
#include <vector>

namespace straylight {

/// Collects events from various system sources.
class Collectors {
public:
    /// Collect login/logout events from wtmp/utmp.
    static std::vector<TimelineEvent> collect_logins();

    /// Collect package install/remove events from dpkg.log.
    static std::vector<TimelineEvent> collect_packages();

    /// Collect recently modified/accessed files from ~/.local/share/recently-used.xbel
    /// and from find commands.
    static std::vector<TimelineEvent> collect_files(bool modified, bool accessed);

    /// Collect service start/stop/fail events from the systemd journal.
    static std::vector<TimelineEvent> collect_services();

    /// Collect commands from bash_history / zsh_history.
    static std::vector<TimelineEvent> collect_commands();

    /// Collect git log events from known repositories.
    static std::vector<TimelineEvent> collect_git();

    /// Collect application usage by parsing recent X11 / Wayland focus events
    /// or from /proc scanning.
    static std::vector<TimelineEvent> collect_apps();

    /// Run all collectors and return merged events sorted by timestamp.
    static std::vector<TimelineEvent> collect_all();

private:
    /// Execute a shell command and return stdout lines.
    static std::vector<std::string> run_lines(const std::string& cmd);

    /// Parse an ISO or epoch timestamp string.
    static std::chrono::system_clock::time_point parse_time(const std::string& s);

    /// Get $HOME.
    static std::string home_dir();
};

} // namespace straylight
