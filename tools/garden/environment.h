// tools/garden/environment.h
// Isolated development environment specification and activation.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

/// A single environment variable setting.
struct EnvVar {
    std::string key;
    std::string value;
    bool append = false;  // If true, append to existing value with ':' separator
};

/// Shell hook that runs on enter/exit.
struct ShellHook {
    std::string on_enter;  // Script to run when entering environment
    std::string on_exit;   // Script to run when exiting environment
};

/// Python virtual environment config.
struct PythonVenvConfig {
    bool enabled = false;
    std::string python_version;  // e.g. "3.11"
    std::vector<std::string> pip_packages;
};

/// A package installed in the environment.
struct InstalledPackage {
    std::string name;
    std::string version;
    std::string install_method;  // "apt", "pip", "cargo", "manual"
    std::string installed_at;    // ISO timestamp
};

/// Full specification of a garden environment.
struct EnvironmentSpec {
    std::string name;
    std::string description;
    std::string created_at;
    std::string modified_at;

    std::vector<EnvVar> env_vars;
    ShellHook hooks;
    PythonVenvConfig python;
    std::vector<InstalledPackage> packages;
    std::vector<std::string> path_prepend;  // Extra directories to prepend to PATH
};

/// Manages a single environment's spec file and directory structure.
class Environment {
public:
    Environment() = default;

    /// Load an environment spec from its garden.json file.
    static Result<Environment, SLError> load(const std::string& base_dir,
                                               const std::string& name);

    /// Create a new empty environment.
    static Result<Environment, SLError> create(const std::string& base_dir,
                                                 const std::string& name,
                                                 const std::string& description = "");

    /// Save the current spec to garden.json.
    Result<void, SLError> save() const;

    /// Delete the environment directory entirely.
    Result<void, SLError> destroy();

    /// Clone this environment to a new name.
    Result<Environment, SLError> clone(const std::string& new_name) const;

    /// Add an environment variable.
    void add_env_var(const std::string& key, const std::string& value, bool append = false);

    /// Remove an environment variable by key.
    void remove_env_var(const std::string& key);

    /// Add a package record.
    void add_package(const std::string& name, const std::string& version,
                     const std::string& method);

    /// Remove a package record by name.
    void remove_package(const std::string& name);

    /// Generate the shell activation script.
    /// This outputs a script fragment that can be eval'd to enter the environment.
    std::string generate_enter_script() const;

    /// Generate the shell deactivation script.
    std::string generate_exit_script() const;

    /// Export the spec as a portable garden.json.
    Result<void, SLError> export_spec(const std::string& output_path) const;

    /// Get the environment root directory.
    const std::string& root_dir() const { return root_dir_; }

    /// Get the spec.
    const EnvironmentSpec& spec() const { return spec_; }
    EnvironmentSpec& spec() { return spec_; }

    /// Get the path to the garden.json file.
    std::string spec_path() const;

    /// Get the path to the environment's bin directory.
    std::string bin_dir() const;

    /// Get the path to the environment's lib directory.
    std::string lib_dir() const;

private:
    std::string base_dir_;
    std::string root_dir_;
    EnvironmentSpec spec_;

    /// Create the directory structure for the environment.
    Result<void, SLError> create_directories();

    /// Serialize the spec to JSON.
    nlohmann::json to_json() const;

    /// Deserialize the spec from JSON.
    static EnvironmentSpec from_json(const nlohmann::json& j);
};

} // namespace straylight
