#pragma once

#include <filesystem>
#include <fstream>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace straylight::test {

/// Parses systemd .service files and builds a dependency graph.
/// Used by integration tests to verify startup ordering.
class ServiceGraph {
public:
    /// Load all .service files from a directory.
    void load_service_dir(const std::filesystem::path& dir) {
        for (auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".service") {
                parse_service_file(entry.path());
            }
        }
    }

    /// Get all straylight dependencies for a given service.
    std::vector<std::string> dependencies(const std::string& service) const {
        auto it = after_.find(service);
        if (it == after_.end()) return {};
        std::vector<std::string> result;
        for (auto& dep : it->second) {
            // Only include straylight services
            if (dep.find("straylight-") != std::string::npos) {
                result.push_back(dep);
            }
        }
        return result;
    }

    /// Check if `service` depends on `dep` (via After=).
    bool has_dep(const std::string& service, const std::string& dep) const {
        auto it = after_.find(service);
        if (it == after_.end()) return false;
        return it->second.count(dep) > 0;
    }

    /// Detect cycles using Kahn's algorithm on straylight services only.
    bool has_cycle() const {
        // Build adjacency and in-degree for straylight services only
        std::unordered_map<std::string, std::vector<std::string>> adj;
        std::unordered_map<std::string, int> in_degree;

        for (auto& [svc, _] : after_) {
            if (svc.find("straylight-") == std::string::npos) continue;
            if (in_degree.find(svc) == in_degree.end()) in_degree[svc] = 0;
        }

        for (auto& [svc, deps] : after_) {
            if (svc.find("straylight-") == std::string::npos) continue;
            for (auto& dep : deps) {
                if (dep.find("straylight-") == std::string::npos) continue;
                adj[dep].push_back(svc);
                in_degree[svc]++;
            }
        }

        std::queue<std::string> q;
        for (auto& [node, deg] : in_degree) {
            if (deg == 0) q.push(node);
        }

        size_t visited = 0;
        while (!q.empty()) {
            auto node = q.front();
            q.pop();
            visited++;
            for (auto& next : adj[node]) {
                if (--in_degree[next] == 0) q.push(next);
            }
        }

        return visited != in_degree.size();
    }

private:
    // service name -> set of After= dependencies
    std::unordered_map<std::string, std::set<std::string>> after_;

    void parse_service_file(const std::filesystem::path& path) {
        std::ifstream f(path);
        std::string service_name = path.filename().string();
        std::string line;
        bool in_unit = false;

        while (std::getline(f, line)) {
            if (line == "[Unit]") {
                in_unit = true;
                continue;
            }
            if (line.starts_with("[") && line != "[Unit]") {
                in_unit = false;
                continue;
            }
            if (in_unit && line.starts_with("After=")) {
                auto value = line.substr(6);
                std::istringstream iss(value);
                std::string dep;
                while (iss >> dep) {
                    after_[service_name].insert(dep);
                }
            }
        }
    }
};

} // namespace straylight::test
