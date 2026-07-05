// tools/profile/profiler.h
// System profiler — comprehensive one-shot system profile report.
#pragma once

#include <straylight/result.h>

#include <map>
#include <string>
#include <vector>

namespace straylight {

/// A single profile metric.
struct ProfileMetric {
    std::string category;
    std::string name;
    std::string value;
    std::string unit;
};

/// Complete system profile.
struct SystemProfile {
    std::string hostname;
    std::string timestamp;
    std::vector<ProfileMetric> metrics;
    int health_score = -1;   // -1 = not available
    int security_score = -1;
};

/// Comparison result between two profiles.
struct ProfileComparison {
    struct Diff {
        std::string category;
        std::string name;
        std::string old_value;
        std::string new_value;
    };
    std::vector<Diff> changes;
    int old_health = -1;
    int new_health = -1;
};

class Profiler {
public:
    Profiler() = default;

    /// Collect a full system profile.
    SystemProfile collect();

    /// Format profile as terminal text table.
    static std::string format_text(const SystemProfile& profile);

    /// Format profile as JSON.
    static std::string format_json(const SystemProfile& profile);

    /// Format profile as HTML report.
    static std::string format_html(const SystemProfile& profile);

    /// Compare two profiles.
    static ProfileComparison compare(const SystemProfile& a, const SystemProfile& b);

    /// Load a profile from a JSON file.
    static Result<SystemProfile, std::string> load(const std::string& path);

    /// Save a profile to a JSON file.
    static Result<void, std::string> save(const SystemProfile& profile,
                                           const std::string& path);

private:
    /// Collect CPU info from /proc/cpuinfo.
    std::vector<ProfileMetric> collect_cpu();

    /// Collect memory info from /proc/meminfo.
    std::vector<ProfileMetric> collect_memory();

    /// Collect GPU info.
    std::vector<ProfileMetric> collect_gpu();

    /// Collect disk info.
    std::vector<ProfileMetric> collect_disk();

    /// Collect network info.
    std::vector<ProfileMetric> collect_network();

    /// Collect kernel info.
    std::vector<ProfileMetric> collect_kernel();

    /// Collect boot time.
    std::vector<ProfileMetric> collect_boot();

    /// Collect package count.
    std::vector<ProfileMetric> collect_packages();

    /// Collect running services.
    std::vector<ProfileMetric> collect_services();

    /// Collect thermal state.
    std::vector<ProfileMetric> collect_thermal();

    /// Get health score from straylight-health daemon.
    int get_health_score();

    /// Get security score from straylight-shield.
    int get_security_score();

    /// Run a command and return trimmed output.
    std::string run_cmd(const std::string& cmd) const;
};

} // namespace straylight
