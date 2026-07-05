// apps/backup/scheduler.cpp
// Background scheduler for backup profiles
#include "scheduler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace straylight::backup {

namespace {
using json = nlohmann::json;

inline SLError make_err(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}

int64_t tp_to_epoch(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count();
}
std::chrono::system_clock::time_point epoch_to_tp(int64_t s) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{s}};
}

std::string interval_to_str(Interval iv) {
    switch (iv) {
        case Interval::Hourly: return "hourly";
        case Interval::Daily:  return "daily";
        case Interval::Weekly: return "weekly";
        case Interval::Custom: return "custom";
    }
    return "daily";
}

Interval str_to_interval(const std::string& s) {
    if (s == "hourly") return Interval::Hourly;
    if (s == "weekly") return Interval::Weekly;
    if (s == "custom") return Interval::Custom;
    return Interval::Daily;
}
} // namespace

fs::path Scheduler::schedules_path() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".config" / "straylight"
                         : fs::path("/tmp");
    std::error_code ec;
    fs::create_directories(base, ec);
    return base / "backup-schedules.json";
}

Result<void, SLError> Scheduler::load() {
    const fs::path path = schedules_path();
    if (!fs::exists(path)) return Result<void, SLError>::ok();

    std::ifstream f(path);
    if (!f) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError,
                     "Cannot open schedules: " + path.string()));
    }

    json root;
    try { f >> root; }
    catch (const std::exception& ex) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::ParseError,
                     std::string("Schedules parse error: ") + ex.what()));
    }

    std::lock_guard lock(mtx_);
    scheds_.clear();
    for (const auto& js : root.value("schedules", json::array())) {
        Schedule s;
        s.profile_name    = js.value("profile_name", "");
        s.interval        = str_to_interval(js.value("interval", "daily"));
        s.custom_interval = std::chrono::seconds{js.value("custom_secs", int64_t{0})};
        s.hour            = js.value("hour",    2);
        s.weekday         = js.value("weekday", 0);
        s.enabled         = js.value("enabled", true);
        s.last_run        = epoch_to_tp(js.value("last_run", int64_t{0}));
        scheds_.push_back(std::move(s));
    }
    return Result<void, SLError>::ok();
}

Result<void, SLError> Scheduler::save() const {
    json root;
    json jarr = json::array();

    std::lock_guard lock(mtx_);
    for (const auto& s : scheds_) {
        json js;
        js["profile_name"] = s.profile_name;
        js["interval"]     = interval_to_str(s.interval);
        js["custom_secs"]  = static_cast<int64_t>(s.custom_interval.count());
        js["hour"]         = s.hour;
        js["weekday"]      = s.weekday;
        js["enabled"]      = s.enabled;
        js["last_run"]     = tp_to_epoch(s.last_run);
        jarr.push_back(std::move(js));
    }
    root["schedules"] = std::move(jarr);

    std::ofstream f(schedules_path());
    if (!f) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError,
                     "Cannot write schedules file"));
    }
    f << root.dump(2);
    return Result<void, SLError>::ok();
}

void Scheduler::add(Schedule s) {
    std::lock_guard lock(mtx_);
    // Remove any existing entry for the same profile
    scheds_.erase(
        std::remove_if(scheds_.begin(), scheds_.end(),
                       [&](const Schedule& x){ return x.profile_name == s.profile_name; }),
        scheds_.end());
    scheds_.push_back(std::move(s));
}

void Scheduler::remove(const std::string& profile_name) {
    std::lock_guard lock(mtx_);
    scheds_.erase(
        std::remove_if(scheds_.begin(), scheds_.end(),
                       [&](const Schedule& s){ return s.profile_name == profile_name; }),
        scheds_.end());
}

bool Scheduler::is_due(const Schedule& s) const {
    if (!s.enabled) return false;

    const auto now = std::chrono::system_clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - s.last_run);

    switch (s.interval) {
        case Interval::Hourly:
            return age >= std::chrono::hours(1);

        case Interval::Daily:
            return age >= std::chrono::hours(24);

        case Interval::Weekly:
            if (age < std::chrono::hours(24 * 7)) return false;
            {
                // Also check we're on the right weekday + hour
                auto t = std::chrono::system_clock::to_time_t(now);
                const std::tm* tm = std::localtime(&t);
                return tm->tm_wday == s.weekday && tm->tm_hour == s.hour;
            }

        case Interval::Custom:
            return s.custom_interval.count() > 0 && age >= s.custom_interval;
    }
    return false;
}

std::vector<std::string> Scheduler::overdue() const {
    std::lock_guard lock(mtx_);
    std::vector<std::string> result;
    for (const auto& s : scheds_) {
        if (is_due(s)) result.push_back(s.profile_name);
    }
    return result;
}

void Scheduler::start(Engine& engine) {
    if (running_.exchange(true)) return; // already running

    thread_ = std::thread([this, &engine] {
        while (running_.load(std::memory_order_relaxed)) {
            {
                std::lock_guard lock(mtx_);
                for (auto& s : scheds_) {
                    if (!is_due(s)) continue;
                    // Build a minimal profile from the schedule name
                    // (real app would look up the full profile from a store)
                    BackupProfile dummy;
                    dummy.name = s.profile_name;
                    // Run backup — errors are silently recorded on the run
                    (void)engine.run_backup(dummy);
                    s.last_run = std::chrono::system_clock::now();
                }
            }
            // Sleep 60 s in 1-second increments so stop() is responsive
            for (int i = 0; i < 60 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void Scheduler::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

} // namespace straylight::backup
