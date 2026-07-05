// tools/garden/environment.cpp
#include "environment.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf{};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

std::string Environment::spec_path() const {
    return root_dir_ + "/garden.json";
}

std::string Environment::bin_dir() const {
    return root_dir_ + "/bin";
}

std::string Environment::lib_dir() const {
    return root_dir_ + "/lib";
}

Result<void, SLError> Environment::create_directories() {
    std::error_code ec;
    fs::create_directories(root_dir_, ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot create " + root_dir_ + ": " + ec.message()});
    }

    fs::create_directories(root_dir_ + "/bin", ec);
    fs::create_directories(root_dir_ + "/lib", ec);
    fs::create_directories(root_dir_ + "/include", ec);
    fs::create_directories(root_dir_ + "/share", ec);
    fs::create_directories(root_dir_ + "/etc", ec);
    fs::create_directories(root_dir_ + "/tmp", ec);

    return Result<void, SLError>::ok();
}

nlohmann::json Environment::to_json() const {
    nlohmann::json j;
    j["name"] = spec_.name;
    j["description"] = spec_.description;
    j["created_at"] = spec_.created_at;
    j["modified_at"] = spec_.modified_at;

    // Environment variables
    nlohmann::json env_arr = nlohmann::json::array();
    for (const auto& ev : spec_.env_vars) {
        nlohmann::json e;
        e["key"] = ev.key;
        e["value"] = ev.value;
        e["append"] = ev.append;
        env_arr.push_back(e);
    }
    j["env_vars"] = env_arr;

    // Path prepend
    j["path_prepend"] = spec_.path_prepend;

    // Shell hooks
    j["hooks"] = {
        {"on_enter", spec_.hooks.on_enter},
        {"on_exit", spec_.hooks.on_exit}
    };

    // Python venv
    j["python"] = {
        {"enabled", spec_.python.enabled},
        {"python_version", spec_.python.python_version},
        {"pip_packages", spec_.python.pip_packages}
    };

    // Packages
    nlohmann::json pkg_arr = nlohmann::json::array();
    for (const auto& pkg : spec_.packages) {
        nlohmann::json p;
        p["name"] = pkg.name;
        p["version"] = pkg.version;
        p["install_method"] = pkg.install_method;
        p["installed_at"] = pkg.installed_at;
        pkg_arr.push_back(p);
    }
    j["packages"] = pkg_arr;

    return j;
}

EnvironmentSpec Environment::from_json(const nlohmann::json& j) {
    EnvironmentSpec spec;
    spec.name = j.value("name", "");
    spec.description = j.value("description", "");
    spec.created_at = j.value("created_at", "");
    spec.modified_at = j.value("modified_at", "");

    if (j.contains("env_vars") && j["env_vars"].is_array()) {
        for (const auto& e : j["env_vars"]) {
            EnvVar ev;
            ev.key = e.value("key", "");
            ev.value = e.value("value", "");
            ev.append = e.value("append", false);
            spec.env_vars.push_back(ev);
        }
    }

    if (j.contains("path_prepend") && j["path_prepend"].is_array()) {
        for (const auto& p : j["path_prepend"]) {
            if (p.is_string()) spec.path_prepend.push_back(p.get<std::string>());
        }
    }

    if (j.contains("hooks") && j["hooks"].is_object()) {
        spec.hooks.on_enter = j["hooks"].value("on_enter", "");
        spec.hooks.on_exit = j["hooks"].value("on_exit", "");
    }

    if (j.contains("python") && j["python"].is_object()) {
        spec.python.enabled = j["python"].value("enabled", false);
        spec.python.python_version = j["python"].value("python_version", "");
        if (j["python"].contains("pip_packages") && j["python"]["pip_packages"].is_array()) {
            for (const auto& p : j["python"]["pip_packages"]) {
                if (p.is_string()) spec.python.pip_packages.push_back(p.get<std::string>());
            }
        }
    }

    if (j.contains("packages") && j["packages"].is_array()) {
        for (const auto& p : j["packages"]) {
            InstalledPackage pkg;
            pkg.name = p.value("name", "");
            pkg.version = p.value("version", "");
            pkg.install_method = p.value("install_method", "");
            pkg.installed_at = p.value("installed_at", "");
            spec.packages.push_back(pkg);
        }
    }

    return spec;
}

Result<Environment, SLError> Environment::create(const std::string& base_dir,
                                                    const std::string& name,
                                                    const std::string& description) {
    if (name.empty()) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::InvalidArgument, "Environment name cannot be empty"});
    }

    // Validate name (alphanumeric, dashes, underscores only)
    for (char c : name) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') {
            return Result<Environment, SLError>::error(
                SLError{SLErrorCode::InvalidArgument,
                        "Invalid character '" + std::string(1, c) + "' in environment name"});
        }
    }

    Environment env;
    env.base_dir_ = base_dir;
    env.root_dir_ = base_dir + "/" + name;

    if (fs::exists(env.root_dir_)) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::AlreadyExists,
                    "Environment '" + name + "' already exists at " + env.root_dir_});
    }

    env.spec_.name = name;
    env.spec_.description = description;
    env.spec_.created_at = now_iso();
    env.spec_.modified_at = env.spec_.created_at;

    // Default path prepend: the env's own bin dir
    env.spec_.path_prepend.push_back(env.bin_dir());

    auto dir_result = env.create_directories();
    if (!dir_result.has_value()) {
        return Result<Environment, SLError>::error(dir_result.error());
    }

    auto save_result = env.save();
    if (!save_result.has_value()) {
        return Result<Environment, SLError>::error(save_result.error());
    }

    return Result<Environment, SLError>::ok(std::move(env));
}

Result<Environment, SLError> Environment::load(const std::string& base_dir,
                                                  const std::string& name) {
    Environment env;
    env.base_dir_ = base_dir;
    env.root_dir_ = base_dir + "/" + name;

    if (!fs::exists(env.root_dir_)) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Environment '" + name + "' not found at " + env.root_dir_});
    }

    std::ifstream f(env.spec_path());
    if (!f.is_open()) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + env.spec_path()});
    }

    try {
        nlohmann::json j;
        f >> j;
        env.spec_ = from_json(j);
    } catch (const std::exception& e) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::ParseError,
                    "Failed to parse " + env.spec_path() + ": " + e.what()});
    }

    return Result<Environment, SLError>::ok(std::move(env));
}

Result<void, SLError> Environment::save() const {
    spec_.modified_at; // will be set by caller if needed

    std::ofstream f(spec_path());
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot write " + spec_path()});
    }

    f << to_json().dump(4);
    return Result<void, SLError>::ok();
}

Result<void, SLError> Environment::destroy() {
    std::error_code ec;
    fs::remove_all(root_dir_, ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "Cannot remove " + root_dir_ + ": " + ec.message()});
    }
    return Result<void, SLError>::ok();
}

Result<Environment, SLError> Environment::clone(const std::string& new_name) const {
    if (new_name.empty()) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::InvalidArgument, "New name cannot be empty"});
    }

    std::string new_root = base_dir_ + "/" + new_name;
    if (fs::exists(new_root)) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::AlreadyExists,
                    "Environment '" + new_name + "' already exists"});
    }

    // Copy the directory tree
    std::error_code ec;
    fs::copy(root_dir_, new_root, fs::copy_options::recursive, ec);
    if (ec) {
        return Result<Environment, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "Failed to copy " + root_dir_ + " to " + new_root + ": " + ec.message()});
    }

    // Load the new environment and update its name
    auto loaded = Environment::load(base_dir_, new_name);
    if (!loaded.has_value()) {
        return loaded;
    }

    auto env = std::move(loaded).value();
    env.spec_.name = new_name;
    env.spec_.created_at = now_iso();
    env.spec_.modified_at = env.spec_.created_at;
    env.spec_.description = "Clone of " + spec_.name;

    // Update path_prepend to point to new bin dir
    for (auto& p : env.spec_.path_prepend) {
        if (p == bin_dir()) {
            p = env.bin_dir();
        }
    }

    auto save_result = env.save();
    if (!save_result.has_value()) {
        return Result<Environment, SLError>::error(save_result.error());
    }

    return Result<Environment, SLError>::ok(std::move(env));
}

void Environment::add_env_var(const std::string& key, const std::string& value, bool append) {
    // Remove existing entry for this key first
    remove_env_var(key);

    EnvVar ev;
    ev.key = key;
    ev.value = value;
    ev.append = append;
    spec_.env_vars.push_back(ev);
    spec_.modified_at = now_iso();
}

void Environment::remove_env_var(const std::string& key) {
    spec_.env_vars.erase(
        std::remove_if(spec_.env_vars.begin(), spec_.env_vars.end(),
                        [&key](const EnvVar& ev) { return ev.key == key; }),
        spec_.env_vars.end());
    spec_.modified_at = now_iso();
}

void Environment::add_package(const std::string& name, const std::string& version,
                               const std::string& method) {
    remove_package(name);

    InstalledPackage pkg;
    pkg.name = name;
    pkg.version = version;
    pkg.install_method = method;
    pkg.installed_at = now_iso();
    spec_.packages.push_back(pkg);
    spec_.modified_at = now_iso();
}

void Environment::remove_package(const std::string& name) {
    spec_.packages.erase(
        std::remove_if(spec_.packages.begin(), spec_.packages.end(),
                        [&name](const InstalledPackage& p) { return p.name == name; }),
        spec_.packages.end());
    spec_.modified_at = now_iso();
}

std::string Environment::generate_enter_script() const {
    std::ostringstream ss;

    // Save the original state for deactivation
    ss << "export _GARDEN_OLD_PATH=\"$PATH\"\n";
    ss << "export _GARDEN_OLD_PS1=\"$PS1\"\n";
    ss << "export _GARDEN_NAME=\"" << spec_.name << "\"\n";
    ss << "export _GARDEN_ROOT=\"" << root_dir_ << "\"\n";
    ss << "export STRAYLIGHT_GARDEN_NUMA_PROFILE=\"${STRAYLIGHT_GARDEN_NUMA_PROFILE:-garden}\"\n";
    ss << "export STRAYLIGHT_NUMA_PROFILE=\"$STRAYLIGHT_GARDEN_NUMA_PROFILE\"\n";
    ss << "\n";

    // Set environment variables
    for (const auto& ev : spec_.env_vars) {
        if (ev.append) {
            ss << "export " << ev.key << "=\"${" << ev.key << ":+$" << ev.key << ":}" << ev.value << "\"\n";
        } else {
            ss << "export " << ev.key << "=\"" << ev.value << "\"\n";
        }
        // Save old values for restore
        ss << "export _GARDEN_OLD_" << ev.key << "=\"${" << ev.key << "}\"\n";
    }

    // Prepend paths to PATH
    std::string path_additions;
    for (const auto& p : spec_.path_prepend) {
        if (!path_additions.empty()) path_additions += ":";
        path_additions += p;
    }
    if (!path_additions.empty()) {
        ss << "export PATH=\"" << path_additions << ":$PATH\"\n";
    }

    // Set LD_LIBRARY_PATH
    ss << "export LD_LIBRARY_PATH=\"" << lib_dir() << "${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n";

    // Update prompt
    ss << "export PS1=\"(garden:" << spec_.name << ") $PS1\"\n";

    // Python venv activation
    if (spec_.python.enabled) {
        std::string venv_path = root_dir_ + "/venv";
        ss << "if [ -f \"" << venv_path << "/bin/activate\" ]; then\n";
        ss << "  source \"" << venv_path << "/bin/activate\"\n";
        ss << "fi\n";
    }

    // NUMA-aware command wrapper.  Garden activation is intentionally still
    // a shell environment; commands opt into placement through garden-run.
    ss << "if command -v straylight-numa-run >/dev/null 2>&1; then\n";
    ss << "  garden-run() { straylight-numa-run \"$STRAYLIGHT_GARDEN_NUMA_PROFILE\" \"$@\"; }\n";
    ss << "  export -f garden-run 2>/dev/null || true\n";
    ss << "fi\n";
    ss << "\n";

    // On-enter hook
    if (!spec_.hooks.on_enter.empty()) {
        ss << "# On-enter hook\n";
        ss << spec_.hooks.on_enter << "\n";
    }

    ss << "\n";
    ss << "echo \"Entered garden: " << spec_.name << "\"\n";

    return ss.str();
}

std::string Environment::generate_exit_script() const {
    std::ostringstream ss;

    // On-exit hook
    if (!spec_.hooks.on_exit.empty()) {
        ss << "# On-exit hook\n";
        ss << spec_.hooks.on_exit << "\n";
    }

    // Deactivate Python venv
    if (spec_.python.enabled) {
        ss << "if type deactivate >/dev/null 2>&1; then deactivate; fi\n";
    }

    // Restore environment variables
    for (const auto& ev : spec_.env_vars) {
        ss << "export " << ev.key << "=\"$_GARDEN_OLD_" << ev.key << "\"\n";
        ss << "unset _GARDEN_OLD_" << ev.key << "\n";
    }

    // Restore PATH and PS1
    ss << "export PATH=\"$_GARDEN_OLD_PATH\"\n";
    ss << "export PS1=\"$_GARDEN_OLD_PS1\"\n";
    ss << "unset _GARDEN_OLD_PATH _GARDEN_OLD_PS1 _GARDEN_NAME _GARDEN_ROOT\n";
    ss << "unset STRAYLIGHT_GARDEN_NUMA_PROFILE STRAYLIGHT_NUMA_PROFILE\n";
    ss << "unset -f garden-run 2>/dev/null || true\n";
    ss << "unset LD_LIBRARY_PATH\n"; // Simplified; production would save/restore

    ss << "echo \"Exited garden: " << spec_.name << "\"\n";

    return ss.str();
}

Result<void, SLError> Environment::export_spec(const std::string& output_path) const {
    std::ofstream f(output_path);
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot write " + output_path});
    }

    f << to_json().dump(4);
    return Result<void, SLError>::ok();
}

} // namespace straylight
