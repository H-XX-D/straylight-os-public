// tools/garden/garden_manager.h
// High-level manager for all garden environments.
#pragma once

#include "environment.h"
#include <straylight/result.h>
#include <straylight/error.h>

#include <string>
#include <vector>

namespace straylight {

/// Summary info for a garden environment (lightweight, no full load).
struct GardenSummary {
    std::string name;
    std::string description;
    std::string created_at;
    std::string modified_at;
    size_t package_count = 0;
    bool has_python_venv = false;
    std::string root_dir;
};

/// Manages all garden environments under ~/.config/straylight/gardens/.
class GardenManager {
public:
    GardenManager();
    explicit GardenManager(const std::string& base_dir);

    /// Initialize the manager — ensures the base directory exists.
    Result<void, SLError> init();

    /// Create a new environment.
    Result<Environment, SLError> create(const std::string& name,
                                          const std::string& description = "");

    /// Load an existing environment.
    Result<Environment, SLError> load(const std::string& name);

    /// List all environments with summary info.
    std::vector<GardenSummary> list() const;

    /// Check if an environment exists.
    bool exists(const std::string& name) const;

    /// Delete an environment.
    Result<void, SLError> destroy(const std::string& name);

    /// Clone an environment.
    Result<Environment, SLError> clone(const std::string& source, const std::string& dest);

    /// Import an environment from a garden.json spec file.
    Result<Environment, SLError> import_spec(const std::string& spec_path,
                                               const std::string& name = "");

    /// Install a package into an environment.
    /// Supports: apt/pkg (system), pip (python), cargo (rust), manual.
    Result<void, SLError> install_package(const std::string& env_name,
                                            const std::string& package_name,
                                            const std::string& method = "manual");

    /// Get the base directory for all gardens.
    const std::string& base_dir() const { return base_dir_; }

private:
    std::string base_dir_;

    /// Run a shell command and capture exit code.
    int run_command(const std::string& cmd) const;
};

} // namespace straylight
