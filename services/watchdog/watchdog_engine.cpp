// services/watchdog/watchdog_engine.cpp
#include "watchdog_engine.h"

#include <straylight/log.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace straylight {

namespace {

std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string exec_command(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get())) {
        result += buffer.data();
    }
    // Strip trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

static std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

struct UnitRef {
    bool user = false;
    std::string name;
};

static UnitRef parse_unit_ref(const std::string& unit_name) {
    static constexpr const char* kUserPrefix = "user:";
    UnitRef ref;
    if (unit_name.rfind(kUserPrefix, 0) == 0) {
        ref.user = true;
        ref.name = unit_name.substr(std::strlen(kUserPrefix));
    } else {
        ref.name = unit_name;
    }
    return ref;
}

static std::string systemctl_base(const UnitRef& ref) {
    if (!ref.user) return "systemctl";
    return "XDG_RUNTIME_DIR=/run/user/1000 "
           "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "
           "systemctl --user";
}

} // namespace

Result<void, SLError> WatchdogEngine::watch(const WatchedService& svc) {
    std::lock_guard lock(mutex_);
    if (services_.count(svc.name)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::AlreadyExists, "Service already watched: " + svc.name});
    }

    services_[svc.name] = svc;
    ServiceState state;
    state.name = svc.name;
    state.pid = resolve_pid(svc.unit_name);
    state.running = pid_alive(state.pid);
    state.last_check = std::chrono::steady_clock::now();
    add_history(state, "Started watching");
    states_[svc.name] = std::move(state);

    SL_INFO("watchdog: now watching '{}'", svc.name);
    return Result<void, SLError>::ok();
}

Result<void, SLError> WatchdogEngine::unwatch(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Service not watched: " + name});
    }
    services_.erase(it);
    states_.erase(name);
    SL_INFO("watchdog: stopped watching '{}'", name);
    return Result<void, SLError>::ok();
}

Result<void, SLError> WatchdogEngine::check_all() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    for (auto& [name, svc] : services_) {
        auto& state = states_[name];
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - state.last_check).count();

        if (elapsed < svc.health_check_interval_seconds) {
            continue;
        }

        state.last_check = now;
        bool healthy = perform_health_check(svc);

        if (healthy) {
            if (!state.running || state.consecutive_failures > 0) {
                add_history(state, "Service recovered");
                SL_INFO("watchdog: '{}' recovered", name);
            }
            state.running = true;
            state.consecutive_failures = 0;
            state.current_escalation = EscalationLevel::Restart;
            state.last_error.clear();
        } else {
            state.running = false;
            state.consecutive_failures++;
            state.last_error = "Health check failed at " + timestamp_now();
            add_history(state, "Health check failed (consecutive: " +
                       std::to_string(state.consecutive_failures) + ")");
            SL_WARN("watchdog: '{}' failed health check ({} consecutive)",
                     name, state.consecutive_failures);
            escalate(svc, state);
        }
    }

    return Result<void, SLError>::ok();
}

Result<bool, SLError> WatchdogEngine::check_service(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) {
        return Result<bool, SLError>::error(
            SLError{SLErrorCode::NotFound, "Service not watched: " + name});
    }

    bool healthy = perform_health_check(it->second);
    auto& state = states_[name];
    state.last_check = std::chrono::steady_clock::now();
    state.running = healthy;

    return Result<bool, SLError>::ok(healthy);
}

std::vector<std::string> WatchdogEngine::list_services() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    names.reserve(services_.size());
    for (const auto& [name, _] : services_) {
        names.push_back(name);
    }
    return names;
}

Result<ServiceState, SLError> WatchdogEngine::get_state(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(name);
    if (it == states_.end()) {
        return Result<ServiceState, SLError>::error(
            SLError{SLErrorCode::NotFound, "Service not watched: " + name});
    }
    return Result<ServiceState, SLError>::ok(it->second);
}

Result<std::vector<std::string>, SLError> WatchdogEngine::get_history(
    const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(name);
    if (it == states_.end()) {
        return Result<std::vector<std::string>, SLError>::error(
            SLError{SLErrorCode::NotFound, "Service not watched: " + name});
    }
    return Result<std::vector<std::string>, SLError>::ok(it->second.history);
}

Result<void, SLError> WatchdogEngine::load_config(const std::filesystem::path& config_dir) {
    namespace fs = std::filesystem;
    if (!fs::exists(config_dir)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Config directory not found: " + config_dir.string()});
    }

    for (const auto& entry : fs::directory_iterator(config_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".json" && ext != ".conf") continue;

        std::ifstream ifs(entry.path());
        if (!ifs) continue;

        try {
            nlohmann::json j;
            ifs >> j;

            WatchedService svc;
            svc.name = j.value("name", entry.path().stem().string());
            svc.unit_name = j.value("unit", svc.name + ".service");
            svc.max_retries = j.value("max_retries", 3);
            svc.backoff_base_seconds = j.value("backoff_seconds", 5);
            svc.health_check_interval_seconds = j.value("check_interval_seconds", 30);

            std::string check_str = j.value("check_type", "proc");
            if (check_str == "http") svc.check_type = HealthCheckType::HttpGet;
            else if (check_str == "socket") svc.check_type = HealthCheckType::UnixSocket;
            else if (check_str == "file") svc.check_type = HealthCheckType::FileTouch;
            else svc.check_type = HealthCheckType::ProcStat;

            svc.check_target = j.value("check_target", "");

            auto res = watch(svc);
            if (!res.has_value()) {
                SL_WARN("watchdog: failed to load watch config '{}': {}",
                        entry.path().string(), res.error().message());
            }
        } catch (const std::exception& e) {
            SL_WARN("watchdog: failed to parse '{}': {}", entry.path().string(), e.what());
        }
    }

    return Result<void, SLError>::ok();
}

bool WatchdogEngine::pid_alive(pid_t pid) const {
    if (pid <= 0) return false;
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    return std::filesystem::exists(stat_path);
}

pid_t WatchdogEngine::resolve_pid(const std::string& unit_name) const {
    auto unit = parse_unit_ref(unit_name);
    std::string cmd = systemctl_base(unit) + " show -p MainPID --value " +
                      shell_quote(unit.name) + " 2>/dev/null";
    std::string result = exec_command(cmd);
    if (result.empty()) return 0;
    try {
        return static_cast<pid_t>(std::stoi(result));
    } catch (...) {
        return 0;
    }
}

bool WatchdogEngine::perform_health_check(const WatchedService& svc) {
    switch (svc.check_type) {
        case HealthCheckType::ProcStat: {
            pid_t pid = resolve_pid(svc.unit_name);
            if (pid <= 0) {
                // Fallback: check if systemd reports it as active
                auto unit = parse_unit_ref(svc.unit_name);
                std::string cmd = systemctl_base(unit) + " is-active " +
                                  shell_quote(unit.name) + " 2>/dev/null";
                std::string result = exec_command(cmd);
                return result == "active";
            }
            return pid_alive(pid);
        }
        case HealthCheckType::HttpGet: {
            if (svc.check_target.empty()) return false;
            std::string cmd = "curl -sf --max-time 5 " + svc.check_target + " >/dev/null 2>&1";
            return std::system(cmd.c_str()) == 0;
        }
        case HealthCheckType::UnixSocket: {
            if (svc.check_target.empty()) return false;
            int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return false;
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, svc.check_target.c_str(), sizeof(addr.sun_path) - 1);
            bool ok = (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                                  sizeof(addr)) == 0);
            ::close(fd);
            return ok;
        }
        case HealthCheckType::FileTouch: {
            if (svc.check_target.empty()) return false;
            struct stat st{};
            if (::stat(svc.check_target.c_str(), &st) != 0) return false;
            auto now = std::time(nullptr);
            // File must have been modified in the last 2 minutes
            return (now - st.st_mtime) < 120;
        }
    }
    return false;
}

Result<void, SLError> WatchdogEngine::restart_service(const std::string& unit_name) {
    auto unit = parse_unit_ref(unit_name);
    std::string cmd = systemctl_base(unit) + " restart " +
                      shell_quote(unit.name) + " 2>&1";
    std::string output = exec_command(cmd);
    // Check if restart succeeded
    std::string check = systemctl_base(unit) + " is-active " +
                        shell_quote(unit.name) + " 2>/dev/null";
    std::string status = exec_command(check);
    if (status == "active") {
        return Result<void, SLError>::ok();
    }
    return Result<void, SLError>::error(
        SLError{SLErrorCode::Internal, "Restart failed: " + output});
}

void WatchdogEngine::send_notification(const std::string& service_name,
                                        const std::string& message) {
    SL_WARN("watchdog: [NOTIFY] {}: {}", service_name, message);
    // Attempt desktop notification via notify-send
    std::string cmd = "notify-send 'StrayLight Watchdog' '" +
                      service_name + ": " + message + "' 2>/dev/null";
    std::system(cmd.c_str());
}

void WatchdogEngine::send_page(const std::string& service_name,
                                const std::string& message) {
    SL_ERROR("watchdog: [PAGE] {}: {}", service_name, message);
    // Write to paging file for external pickup
    std::string page_file = "/var/log/straylight/watchdog-pages.log";
    std::ofstream ofs(page_file, std::ios::app);
    if (ofs) {
        ofs << timestamp_now() << " SERVICE=" << service_name
            << " MSG=" << message << "\n";
    }
}

void WatchdogEngine::add_history(ServiceState& state, const std::string& event) {
    std::string entry = timestamp_now() + " " + event;
    state.history.push_back(std::move(entry));
    // Keep last 100 entries
    if (state.history.size() > 100) {
        state.history.erase(state.history.begin());
    }
}

void WatchdogEngine::escalate(const WatchedService& svc, ServiceState& state) {
    // Determine current escalation step
    int step = state.consecutive_failures - 1;
    if (step < 0) step = 0;
    if (step >= static_cast<int>(svc.escalation_chain.size())) {
        step = static_cast<int>(svc.escalation_chain.size()) - 1;
    }

    EscalationLevel level = svc.escalation_chain[step];
    state.current_escalation = level;

    switch (level) {
        case EscalationLevel::Restart: {
            // Apply exponential backoff
            auto now = std::chrono::steady_clock::now();
            auto since_last = std::chrono::duration_cast<std::chrono::seconds>(
                now - state.last_restart).count();
            int backoff = svc.backoff_base_seconds * (1 << std::min(state.total_restarts, 5));

            if (since_last < backoff && state.total_restarts > 0) {
                SL_INFO("watchdog: '{}' in backoff ({}s remaining)",
                        svc.name, backoff - since_last);
                add_history(state, "In backoff, skipping restart");
                return;
            }

            if (state.total_restarts >= svc.max_retries) {
                add_history(state, "Max retries exceeded, escalating");
                SL_WARN("watchdog: '{}' exceeded max retries ({})", svc.name, svc.max_retries);
                // Force escalate to next level
                if (step + 1 < static_cast<int>(svc.escalation_chain.size())) {
                    state.consecutive_failures = step + 2;
                    escalate(svc, state);
                }
                return;
            }

            SL_INFO("watchdog: restarting '{}' (attempt {})", svc.name, state.total_restarts + 1);
            add_history(state, "Restarting service (attempt " +
                       std::to_string(state.total_restarts + 1) + ")");

            auto res = restart_service(svc.unit_name);
            state.last_restart = now;
            state.total_restarts++;

            if (res.has_value()) {
                state.pid = resolve_pid(svc.unit_name);
                state.running = true;
                state.consecutive_failures = 0;
                add_history(state, "Restart succeeded");
                SL_INFO("watchdog: '{}' restarted successfully", svc.name);
            } else {
                add_history(state, "Restart failed: " + res.error().message());
                SL_ERROR("watchdog: '{}' restart failed: {}", svc.name, res.error().message());
            }
            break;
        }
        case EscalationLevel::Notify: {
            send_notification(svc.name,
                "Service failed after " + std::to_string(state.total_restarts) + " restarts");
            add_history(state, "Notification sent");
            break;
        }
        case EscalationLevel::Page: {
            send_page(svc.name,
                "CRITICAL: Service unrecoverable after " +
                std::to_string(state.total_restarts) + " restarts");
            add_history(state, "Page sent — critical failure");
            break;
        }
    }
}

} // namespace straylight
