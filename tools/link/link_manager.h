// tools/link/link_manager.h
// Intelligent symlink and resource management across StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace straylight {

class LinkManager {
public:
    struct ManagedLink {
        std::string link_path;
        std::string target_path;
        std::string owner;
        std::string tag;
        std::chrono::system_clock::time_point created;
        bool alive = true;
    };

    struct GraphEdge {
        std::string from;
        std::string to;
    };

    LinkManager();
    ~LinkManager();

    /// Create a tracked symlink from link_path pointing to target_path.
    Result<ManagedLink, std::string> create(const std::string& target,
                                             const std::string& link_path,
                                             const std::string& tag = "");

    /// Auto-symlink dotfiles from a git repo directory into $HOME.
    Result<std::vector<ManagedLink>, std::string> dotfiles(const std::string& repo_dir);

    /// Find all broken symlinks across the given root (default /).
    Result<std::vector<std::string>, std::string> audit(const std::string& root = "/") const;

    /// Return a textual dependency graph of all managed links.
    Result<std::string, std::string> graph() const;

    /// Watch a directory and auto-link new files matching a glob pattern.
    /// Blocks until interrupted.
    Result<void, std::string> watch(const std::string& dir,
                                     const std::string& pattern = "*",
                                     const std::string& dest_dir = "");

    /// Restore all managed links from the registry.
    Result<int, std::string> restore();

    /// List all managed links.
    std::vector<ManagedLink> list() const;

    /// Remove a managed link from the registry (and optionally the symlink).
    Result<void, std::string> remove(const std::string& link_path, bool delete_symlink = true);

private:
    static constexpr const char* kRegistryDir = "/.config/straylight";
    static constexpr const char* kRegistryFile = "links.json";

    std::string registry_path() const;
    std::string home_dir() const;
    Result<void, std::string> load_registry();
    Result<void, std::string> save_registry() const;
    void register_link(const ManagedLink& link);

    std::vector<ManagedLink> links_;
    bool registry_loaded_ = false;
};

} // namespace straylight
