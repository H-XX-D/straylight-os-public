/**
 * StrayLight Fuse — Fusion Engine (implementation)
 */

#include "fusion_engine.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::fuse {

FusionEngine::FusionEngine(SharedRegionManager& region_mgr)
    : region_mgr_(region_mgr) {}

Result<std::string, std::string> FusionEngine::create_session(
        pid_t pid1, pid_t pid2,
        const std::vector<size_t>& shared_regions) {
    // Validate both processes are alive
    if (!process_alive(pid1)) {
        return Result<std::string, std::string>::error(
            "process " + std::to_string(pid1) + " not found");
    }
    if (!process_alive(pid2)) {
        return Result<std::string, std::string>::error(
            "process " + std::to_string(pid2) + " not found");
    }

    if (pid1 == pid2) {
        return Result<std::string, std::string>::error(
            "cannot fuse a process with itself");
    }

    if (shared_regions.empty()) {
        return Result<std::string, std::string>::error(
            "at least one shared region required");
    }

    // Verify address spaces are compatible
    auto maps1 = read_address_space(pid1);
    auto maps2 = read_address_space(pid2);
    if (!maps1) {
        return Result<std::string, std::string>::error(
            "cannot read address space for pid " + std::to_string(pid1) +
            ": " + maps1.err());
    }
    if (!maps2) {
        return Result<std::string, std::string>::error(
            "cannot read address space for pid " + std::to_string(pid2) +
            ": " + maps2.err());
    }

    std::lock_guard<std::mutex> lock(mu_);

    // Check for existing fusion between these PIDs
    for (const auto& [_, s] : sessions_) {
        if (s.active &&
            ((s.pid1 == pid1 && s.pid2 == pid2) ||
             (s.pid1 == pid2 && s.pid2 == pid1))) {
            return Result<std::string, std::string>::error(
                "processes are already fused in session " + s.session_id);
        }
    }

    // Create shared regions
    std::vector<std::string> region_ids;
    size_t total_bytes = 0;

    for (size_t sz : shared_regions) {
        // Page-align the size
        size_t aligned = (sz + 4095) & ~4095ULL;
        auto r = region_mgr_.create_region(aligned);
        if (!r) {
            // Cleanup already-created regions
            for (const auto& rid : region_ids) {
                region_mgr_.release_region(rid);
            }
            return Result<std::string, std::string>::error(
                "failed to create shared region: " + r.err());
        }

        std::string rid = r.value();

        // Map into both processes
        auto m1 = region_mgr_.map_into_process(pid1, rid);
        if (!m1) {
            for (const auto& prev_rid : region_ids) {
                region_mgr_.release_region(prev_rid);
            }
            region_mgr_.release_region(rid);
            return Result<std::string, std::string>::error(
                "failed to map region into pid " + std::to_string(pid1) +
                ": " + m1.err());
        }

        auto m2 = region_mgr_.map_into_process(pid2, rid);
        if (!m2) {
            for (const auto& prev_rid : region_ids) {
                region_mgr_.release_region(prev_rid);
            }
            region_mgr_.release_region(rid);
            return Result<std::string, std::string>::error(
                "failed to map region into pid " + std::to_string(pid2) +
                ": " + m2.err());
        }

        region_ids.push_back(rid);
        total_bytes += aligned;
    }

    // Build session
    std::string session_id = "fuse-" + std::to_string(++session_counter_);

    FusionSession session{};
    session.session_id = session_id;
    session.pid1 = pid1;
    session.pid2 = pid2;
    session.region_ids = std::move(region_ids);
    session.total_shared_bytes = total_bytes;
    session.active = true;
    session.metrics.started_at = std::chrono::steady_clock::now();
    session.metrics.last_activity = session.metrics.started_at;

    sessions_[session_id] = std::move(session);
    return Result<std::string, std::string>::ok(session_id);
}

VoidResult<> FusionEngine::destroy_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return VoidResult<>::error("session not found: " + session_id);
    }

    auto& session = it->second;

    // Release all shared regions
    for (const auto& rid : session.region_ids) {
        // Release twice: once for each process mapping, once for creation
        region_mgr_.release_region(rid);
        region_mgr_.release_region(rid);
        region_mgr_.release_region(rid);
    }

    session.active = false;
    sessions_.erase(it);
    return VoidResult<>::ok();
}

Result<FusionSession, std::string> FusionEngine::get_session(
        const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return Result<FusionSession, std::string>::error(
            "session not found: " + session_id);
    }
    return Result<FusionSession, std::string>::ok(it->second);
}

std::vector<FusionSession> FusionEngine::list_sessions() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<FusionSession> result;
    result.reserve(sessions_.size());
    for (const auto& [_, s] : sessions_) {
        if (s.active) result.push_back(s);
    }
    return result;
}

FusionEngine::AggregateStats FusionEngine::get_stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    AggregateStats stats{};
    for (const auto& [_, s] : sessions_) {
        if (!s.active) continue;
        stats.active_sessions++;
        stats.total_regions += s.region_ids.size();
        stats.total_shared_bytes += s.total_shared_bytes;
        stats.total_messages += s.metrics.messages_exchanged;
    }
    return stats;
}

bool FusionEngine::is_fused(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [_, s] : sessions_) {
        if (s.active && (s.pid1 == pid || s.pid2 == pid)) return true;
    }
    return false;
}

void FusionEngine::reap_dead_sessions() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> dead;

    for (const auto& [id, s] : sessions_) {
        if (!s.active) continue;
        if (!process_alive(s.pid1) || !process_alive(s.pid2)) {
            dead.push_back(id);
        }
    }

    for (const auto& id : dead) {
        auto& session = sessions_[id];
        for (const auto& rid : session.region_ids) {
            region_mgr_.release_region(rid);
        }
        session.active = false;
        sessions_.erase(id);
    }
}

// ── Private helpers ─────────────────────────────────────────────────

bool FusionEngine::process_alive(pid_t pid) const {
    return kill(pid, 0) == 0;
}

Result<std::vector<std::pair<uint64_t,uint64_t>>, std::string>
FusionEngine::read_address_space(pid_t pid) const {
    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(path);
    if (!maps) {
        return Result<std::vector<std::pair<uint64_t,uint64_t>>, std::string>::error(
            "cannot open " + path);
    }

    std::vector<std::pair<uint64_t,uint64_t>> ranges;
    std::string line;
    while (std::getline(maps, line)) {
        uint64_t start = 0, end = 0;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
            ranges.emplace_back(start, end);
        }
    }

    return Result<std::vector<std::pair<uint64_t,uint64_t>>, std::string>::ok(
        std::move(ranges));
}

} // namespace straylight::fuse
