// tools/cgroup/cgroup_manager.cpp
// Full cgroup v2 manager implementation for StrayLight OS.

#include "cgroup_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

CgroupManager::CgroupManager() = default;
CgroupManager::~CgroupManager() = default;

std::string CgroupManager::resolve_path(const std::string& cgroup_path) const {
    if (cgroup_path.empty() || cgroup_path == "/") return CGROUP_ROOT;
    std::string path = cgroup_path;
    if (path[0] != '/') path = "/" + path;
    return std::string(CGROUP_ROOT) + path;
}

Result<std::string, std::string> CgroupManager::read_file(const std::string& path) const {
    std::ifstream f(path);
    if (!f.is_open()) return Result<std::string, std::string>::error("cannot read " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Result<std::string, std::string>::ok(content);
}

Result<void, std::string> CgroupManager::write_file(const std::string& path, const std::string& value) const {
    std::ofstream f(path);
    if (!f.is_open()) return Result<void, std::string>::error("cannot write to " + path + ": " + strerror(errno));
    f << value;
    if (f.fail()) return Result<void, std::string>::error("write failed: " + path);
    return Result<void, std::string>::ok();
}

uint64_t CgroupManager::read_uint64(const std::string& path) const {
    auto res = read_file(path);
    if (!res.has_value()) return 0;
    try {
        std::string val = res.value();
        if (val == "max\n" || val == "max") return UINT64_MAX;
        return std::stoull(val);
    } catch (...) { return 0; }
}

int CgroupManager::count_procs(const std::string& cgroup_full_path) const {
    auto res = read_file(cgroup_full_path + "/cgroup.procs");
    if (!res.has_value()) return 0;
    int count = 0;
    std::istringstream stream(res.value());
    std::string line;
    while (std::getline(stream, line)) { if (!line.empty()) ++count; }
    return count;
}

CgroupNode CgroupManager::build_tree(const std::string& path, int depth) const {
    CgroupNode node;
    node.full_path = path;
    node.depth = depth;
    std::string rel = path.substr(std::string(CGROUP_ROOT).size());
    if (rel.empty()) rel = "/";
    auto slash = rel.rfind('/');
    node.name = (slash != std::string::npos && slash + 1 < rel.size()) ? rel.substr(slash + 1) : rel;
    node.process_count = count_procs(path);
    node.memory_current = read_uint64(path + "/memory.current");
    if (!fs::exists(path)) return node;
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_directory()) {
                std::string child_name = entry.path().filename().string();
                if (child_name[0] == '.') continue;
                node.children.push_back(build_tree(entry.path().string(), depth + 1));
            }
        }
    } catch (...) {}
    std::sort(node.children.begin(), node.children.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return node;
}

Result<CgroupNode, std::string> CgroupManager::tree() const {
    if (!fs::exists(CGROUP_ROOT))
        return Result<CgroupNode, std::string>::error("cgroup v2 filesystem not found at " + std::string(CGROUP_ROOT));
    return Result<CgroupNode, std::string>::ok(build_tree(CGROUP_ROOT, 0));
}

Result<CgroupUsage, std::string> CgroupManager::info(const std::string& cgroup_path) const {
    std::string full_path = resolve_path(cgroup_path);
    if (!fs::exists(full_path))
        return Result<CgroupUsage, std::string>::error("cgroup not found: " + cgroup_path);
    CgroupUsage usage;
    usage.path = cgroup_path;
    usage.process_count = count_procs(full_path);
    auto cpu_stat = read_file(full_path + "/cpu.stat");
    if (cpu_stat.has_value()) {
        std::istringstream stream(cpu_stat.value());
        std::string key; uint64_t val;
        while (stream >> key >> val) { if (key == "usage_usec") usage.cpu_usage_us = val; }
    }
    usage.memory_current = read_uint64(full_path + "/memory.current");
    usage.memory_limit = read_uint64(full_path + "/memory.max");
    if (usage.memory_limit != UINT64_MAX && usage.memory_limit > 0)
        usage.memory_percent = 100.0 * static_cast<double>(usage.memory_current) / static_cast<double>(usage.memory_limit);
    auto io_stat = read_file(full_path + "/io.stat");
    if (io_stat.has_value()) {
        std::regex rbytes_re(R"(rbytes=(\d+))"), wbytes_re(R"(wbytes=(\d+))");
        std::smatch m; std::string io = io_stat.value();
        auto it = std::sregex_iterator(io.begin(), io.end(), rbytes_re);
        for (; it != std::sregex_iterator(); ++it) usage.io_read_bytes += std::stoull((*it)[1].str());
        it = std::sregex_iterator(io.begin(), io.end(), wbytes_re);
        for (; it != std::sregex_iterator(); ++it) usage.io_write_bytes += std::stoull((*it)[1].str());
    }
    return Result<CgroupUsage, std::string>::ok(usage);
}

Result<void, std::string> CgroupManager::create(const std::string& path) {
    std::string full_path = resolve_path(path);
    try { fs::create_directories(full_path); }
    catch (const std::exception& e) { return Result<void, std::string>::error("failed to create cgroup: " + std::string(e.what())); }
    std::string parent = fs::path(full_path).parent_path().string();
    auto controllers = read_file(parent + "/cgroup.controllers");
    if (controllers.has_value()) {
        std::string ctrl = controllers.value();
        if (!ctrl.empty() && ctrl.back() == '\n') ctrl.pop_back();
        std::string enable; std::istringstream ss(ctrl); std::string c;
        while (ss >> c) enable += "+" + c + " ";
        if (!enable.empty()) write_file(parent + "/cgroup.subtree_control", enable);
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> CgroupManager::remove(const std::string& path) {
    std::string full_path = resolve_path(path);
    if (!fs::exists(full_path)) return Result<void, std::string>::error("cgroup not found: " + path);
    int procs = count_procs(full_path);
    if (procs > 0) return Result<void, std::string>::error("cgroup has " + std::to_string(procs) + " processes; move them first");
    try { fs::remove(full_path); }
    catch (const std::exception& e) { return Result<void, std::string>::error("failed to remove cgroup: " + std::string(e.what())); }
    return Result<void, std::string>::ok();
}

Result<void, std::string> CgroupManager::move(int pid, const std::string& cgroup_path) {
    std::string full_path = resolve_path(cgroup_path);
    if (!fs::exists(full_path)) return Result<void, std::string>::error("cgroup not found: " + cgroup_path);
    return write_file(full_path + "/cgroup.procs", std::to_string(pid));
}

Result<void, std::string> CgroupManager::set_limits(const std::string& cgroup_path, const CgroupLimits& limits) {
    std::string full_path = resolve_path(cgroup_path);
    if (!fs::exists(full_path)) return Result<void, std::string>::error("cgroup not found: " + cgroup_path);
    if (limits.cpu_max >= 0) {
        auto res = write_file(full_path + "/cpu.max", std::to_string(limits.cpu_max) + " " + std::to_string(limits.cpu_period));
        if (!res.has_value()) return res;
    }
    if (limits.memory_max >= 0) { auto res = write_file(full_path + "/memory.max", std::to_string(limits.memory_max)); if (!res.has_value()) return res; }
    if (limits.memory_high >= 0) { auto res = write_file(full_path + "/memory.high", std::to_string(limits.memory_high)); if (!res.has_value()) return res; }
    if (!limits.io_max.empty()) { auto res = write_file(full_path + "/io.max", limits.io_max); if (!res.has_value()) return res; }
    if (limits.pids_max >= 0) { auto res = write_file(full_path + "/pids.max", std::to_string(limits.pids_max)); if (!res.has_value()) return res; }
    return Result<void, std::string>::ok();
}

Result<CgroupLimits, std::string> CgroupManager::get_limits(const std::string& cgroup_path) const {
    std::string full_path = resolve_path(cgroup_path);
    if (!fs::exists(full_path)) return Result<CgroupLimits, std::string>::error("cgroup not found: " + cgroup_path);
    CgroupLimits limits;
    auto cpu_max = read_file(full_path + "/cpu.max");
    if (cpu_max.has_value()) {
        std::istringstream ss(cpu_max.value()); std::string quota_str, period_str;
        ss >> quota_str >> period_str;
        if (quota_str == "max") limits.cpu_max = -1;
        else { try { limits.cpu_max = std::stoll(quota_str); } catch (...) {} }
        if (!period_str.empty()) { try { limits.cpu_period = std::stoll(period_str); } catch (...) {} }
    }
    auto mem_max = read_file(full_path + "/memory.max");
    if (mem_max.has_value()) { std::string val = mem_max.value(); if (val.find("max") != std::string::npos) limits.memory_max = -1; else { try { limits.memory_max = std::stoll(val); } catch (...) {} } }
    auto mem_high = read_file(full_path + "/memory.high");
    if (mem_high.has_value()) { std::string val = mem_high.value(); if (val.find("max") != std::string::npos) limits.memory_high = -1; else { try { limits.memory_high = std::stoll(val); } catch (...) {} } }
    auto pids_max = read_file(full_path + "/pids.max");
    if (pids_max.has_value()) { std::string val = pids_max.value(); if (val.find("max") != std::string::npos) limits.pids_max = -1; else { try { limits.pids_max = std::stoi(val); } catch (...) {} } }
    return Result<CgroupLimits, std::string>::ok(limits);
}

Result<std::vector<CgroupUsage>, std::string> CgroupManager::usage() const {
    std::vector<CgroupUsage> all;
    if (!fs::exists(CGROUP_ROOT)) return Result<std::vector<CgroupUsage>, std::string>::error("cgroup v2 not available");
    try {
        for (const auto& entry : fs::directory_iterator(CGROUP_ROOT)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            if (name[0] == '.') continue;
            auto info_res = info("/" + name);
            if (info_res.has_value()) all.push_back(info_res.value());
        }
    } catch (...) {}
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.memory_current > b.memory_current; });
    return Result<std::vector<CgroupUsage>, std::string>::ok(all);
}

} // namespace straylight
