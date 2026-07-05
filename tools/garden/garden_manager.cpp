// tools/garden/garden_manager.cpp
#include "garden_manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace straylight {

GardenManager::GardenManager() {
    const char* home = ::getenv("HOME");
    if (home) {
        base_dir_ = std::string(home) + "/.config/straylight/gardens";
    } else {
        base_dir_ = "/tmp/straylight-gardens";
    }
}

GardenManager::GardenManager(const std::string& base_dir) : base_dir_(base_dir) {}

Result<void, SLError> GardenManager::init() {
    std::error_code ec;
    fs::create_directories(base_dir_, ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "Cannot create gardens directory " + base_dir_ + ": " + ec.message()});
    }
    return Result<void, SLError>::ok();
}

Result<Environment, SLError> GardenManager::create(const std::string& name,
                                                      const std::string& description) {
    auto init_r = init();
    if (!init_r.has_value()) return Result<Environment, SLError>::error(init_r.error());

    return Environment::create(base_dir_, name, description);
}

Result<Environment, SLError> GardenManager::load(const std::string& name) {
    return Environment::load(base_dir_, name);
}

std::vector<GardenSummary> GardenManager::list() const {
    std::vector<GardenSummary> results;

    if (!fs::exists(base_dir_)) return results;

    for (const auto& entry : fs::directory_iterator(base_dir_)) {
        if (!entry.is_directory()) continue;

        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;

        std::string spec_path = entry.path().string() + "/garden.json";
        if (!fs::exists(spec_path)) continue;

        GardenSummary summary;
        summary.name = name;
        summary.root_dir = entry.path().string();

        // Try to read spec for details
        std::ifstream f(spec_path);
        if (f.is_open()) {
            try {
                nlohmann::json j;
                f >> j;
                summary.description = j.value("description", "");
                summary.created_at = j.value("created_at", "");
                summary.modified_at = j.value("modified_at", "");
                if (j.contains("packages") && j["packages"].is_array()) {
                    summary.package_count = j["packages"].size();
                }
                if (j.contains("python") && j["python"].is_object()) {
                    summary.has_python_venv = j["python"].value("enabled", false);
                }
            } catch (...) {
                // Partial read is fine for listing
            }
        }

        results.push_back(summary);
    }

    std::sort(results.begin(), results.end(),
              [](const GardenSummary& a, const GardenSummary& b) {
                  return a.name < b.name;
              });

    return results;
}

bool GardenManager::exists(const std::string& name) const {
    return fs::exists(base_dir_ + "/" + name + "/garden.json");
}

Result<void, SLError> GardenManager::destroy(const std::string& name) {
    auto env = load(name);
    if (!env.has_value()) {
        return Result<void, SLError>::error(env.error());
    }
    auto environment = std::move(env).value();
    return environment.destroy();
}

Result<Environment, SLError> GardenManager::clone(const std::string& source,
                                                     const std::string& dest) {
    auto src_env = load(source);
    if (!src_env.has_value()) {
        return Result<Environment, SLError>::error(src_env.error());
    }
    return src_env.value().clone(dest);
}

Result<Environment, SLError> GardenManager::import_spec(const std::string& spec_path,
                                                           const std::string& name) {
    std::ifstream f(spec_path);
    if (!f.is_open()) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + spec_path});
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::ParseError,
                    "Failed to parse " + spec_path + ": " + e.what()});
    }

    std::string env_name = name.empty() ? j.value("name", "imported") : name;

    auto env = create(env_name, j.value("description", "Imported environment"));
    if (!env.has_value()) {
        return env;
    }

    auto environment = std::move(env).value();

    // Apply env vars from spec
    if (j.contains("env_vars") && j["env_vars"].is_array()) {
        for (const auto& e : j["env_vars"]) {
            environment.add_env_var(
                e.value("key", ""),
                e.value("value", ""),
                e.value("append", false));
        }
    }

    // Apply hooks
    if (j.contains("hooks") && j["hooks"].is_object()) {
        environment.spec().hooks.on_enter = j["hooks"].value("on_enter", "");
        environment.spec().hooks.on_exit = j["hooks"].value("on_exit", "");
    }

    // Apply python config
    if (j.contains("python") && j["python"].is_object()) {
        environment.spec().python.enabled = j["python"].value("enabled", false);
        environment.spec().python.python_version = j["python"].value("python_version", "");
        if (j["python"].contains("pip_packages") && j["python"]["pip_packages"].is_array()) {
            for (const auto& p : j["python"]["pip_packages"]) {
                if (p.is_string()) {
                    environment.spec().python.pip_packages.push_back(p.get<std::string>());
                }
            }
        }
    }

    auto save_r = environment.save();
    if (!save_r.has_value()) {
        return Result<Environment, SLError>::error(save_r.error());
    }

    return Result<Environment, SLError>::ok(std::move(environment));
}

int GardenManager::run_command(const std::string& cmd) const {
    return std::system(cmd.c_str());
}

Result<void, SLError> GardenManager::install_package(const std::string& env_name,
                                                        const std::string& package_name,
                                                        const std::string& method) {
    auto env = load(env_name);
    if (!env.has_value()) {
        return Result<void, SLError>::error(env.error());
    }

    auto environment = std::move(env).value();
    std::string version = "latest";
    int rc = 0;

    if (method == "pip") {
        // Install into the environment's Python venv
        std::string venv_path = environment.root_dir() + "/venv";

        // Create venv if it doesn't exist
        if (!fs::exists(venv_path + "/bin/pip")) {
            std::string py_cmd = "python3";
            if (!environment.spec().python.python_version.empty()) {
                py_cmd = "python" + environment.spec().python.python_version;
            }
            std::string create_cmd = py_cmd + " -m venv " + venv_path + " 2>&1";
            rc = run_command(create_cmd);
            if (rc != 0) {
                return Result<void, SLError>::error(
                    SLError{SLErrorCode::Internal, "Failed to create Python venv"});
            }
            environment.spec().python.enabled = true;
        }

        std::string pip_cmd = venv_path + "/bin/pip install " + package_name + " 2>&1";
        rc = run_command(pip_cmd);
        if (rc != 0) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "pip install failed for " + package_name});
        }

        // Try to get version
        std::string ver_cmd = venv_path + "/bin/pip show " + package_name +
                              " 2>/dev/null | grep Version | awk '{print $2}'";
        // Simple: just record "pip" as version for now
        environment.spec().python.pip_packages.push_back(package_name);

    } else if (method == "cargo") {
        std::string cargo_cmd = "CARGO_INSTALL_ROOT=" + environment.root_dir() +
                                " cargo install " + package_name + " 2>&1";
        rc = run_command(cargo_cmd);
        if (rc != 0) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "cargo install failed for " + package_name});
        }

    } else if (method == "apt" || method == "pkg") {
        // For apt/pkg, we record the package but note it's a system package
        // Actual installation would require root
        std::cerr << "Note: System package '" << package_name
                  << "' recorded but not installed (requires root).\n";
        std::cerr << "Install manually: sudo apt install " << package_name << "\n";

    } else {
        // Manual: just record it
        std::cerr << "Recorded manual package: " << package_name << "\n";
    }

    environment.add_package(package_name, version, method);
    auto save_r = environment.save();
    if (!save_r.has_value()) {
        return Result<void, SLError>::error(save_r.error());
    }

    return Result<void, SLError>::ok();
}

} // namespace straylight
