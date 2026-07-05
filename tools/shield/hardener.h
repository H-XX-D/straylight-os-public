// tools/shield/hardener.h
// Automated security hardening for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight {

/// A single hardening action with before/after state.
struct HardenAction {
    std::string category;
    std::string description;
    std::string before;
    std::string after;
    bool applied = false;
    std::string error;
};

/// Report of all hardening actions taken.
struct HardenReport {
    std::string level; // "basic", "moderate", "strict"
    int applied = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<HardenAction> actions;
};

/// Applies security hardening to the system.
class Hardener {
public:
    enum class Level { Basic, Moderate, Strict };

    Hardener();
    ~Hardener();

    /// Apply hardening at the specified level.
    Result<HardenReport, std::string> harden(Level level);

    /// Preview what would be changed without applying.
    Result<HardenReport, std::string> preview(Level level);

    /// Format a hardening report for display.
    static std::string format_report(const HardenReport& report);

    /// Parse a level string.
    static Result<Level, std::string> parse_level(const std::string& s);

private:
    bool dry_run_ = false;

    // Hardening action groups.
    std::vector<HardenAction> harden_sysctl(Level level);
    std::vector<HardenAction> harden_services(Level level);
    std::vector<HardenAction> harden_filesystem(Level level);
    std::vector<HardenAction> harden_firewall(Level level);
    std::vector<HardenAction> harden_ssh(Level level);
    std::vector<HardenAction> harden_audit(Level level);

    // Helpers.
    std::string run_cmd(const std::string& cmd);
    bool set_sysctl(const std::string& key, const std::string& value,
                     HardenAction& action);
    bool set_ssh_config(const std::string& key, const std::string& value,
                         HardenAction& action);
};

} // namespace straylight
