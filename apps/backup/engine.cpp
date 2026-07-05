// apps/backup/engine.cpp
// Incremental backup via rsync fork/exec
#include "engine.h"

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>

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
} // namespace

// ---------------------------------------------------------------------------

std::string Engine::snapshot_name(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", std::localtime(&t));
    return buf;
}

fs::path Engine::manifest_path(const BackupProfile& p) {
    return p.destination / "manifest.json";
}

fs::path Engine::latest_snapshot(const BackupProfile& p) {
    if (!fs::exists(p.destination)) return {};

    std::vector<fs::path> snaps;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(p.destination, ec)) {
        if (entry.is_directory()) {
            // Snapshot dirs match YYYY-MM-DD_HHMMSS
            const std::string name = entry.path().filename().string();
            if (name.size() == 17 && name[10] == '_') {
                snaps.push_back(entry.path());
            }
        }
    }
    if (snaps.empty()) return {};
    std::sort(snaps.begin(), snaps.end());
    return snaps.back();
}

std::vector<std::string> Engine::build_args(const BackupProfile& p,
                                              const fs::path& link_dest,
                                              const fs::path& snapshot_dir) const {
    std::vector<std::string> args;
    args.emplace_back("rsync");
    args.emplace_back("-a");
    args.emplace_back("--info=progress2");
    args.emplace_back("--no-human-readable");

    if (p.compress)       args.emplace_back("-z");
    if (p.delete_removed) args.emplace_back("--delete");

    if (!link_dest.empty() && fs::exists(link_dest)) {
        args.push_back("--link-dest=" + link_dest.string());
    }

    for (const auto& ex : p.excludes) {
        args.push_back("--exclude=" + ex);
    }

    // Ensure source has trailing slash so rsync syncs contents, not the dir itself
    std::string src = p.source.string();
    if (!src.empty() && src.back() != '/') src += '/';
    args.push_back(src);
    args.push_back(snapshot_dir.string() + "/");

    return args;
}

Result<BackupRun, SLError> Engine::exec_rsync(const std::vector<std::string>& args,
                                               ProgressFn prog) {
    int pfd[2];
    if (::pipe(pfd) < 0) {
        return Result<BackupRun, SLError>::error(
            make_err(SLErrorCode::Internal,
                     std::string("pipe: ") + std::strerror(errno)));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pfd[0]); ::close(pfd[1]);
        return Result<BackupRun, SLError>::error(
            make_err(SLErrorCode::Internal,
                     std::string("fork: ") + std::strerror(errno)));
    }

    if (pid == 0) {
        // Child: redirect stdout+stderr to pipe write end
        ::close(pfd[0]);
        ::dup2(pfd[1], STDOUT_FILENO);
        ::dup2(pfd[1], STDERR_FILENO);
        ::close(pfd[1]);

        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        ::execvp(argv[0], const_cast<char**>(argv.data()));
        ::_exit(127);
    }

    ::close(pfd[1]);

    BackupRun run;
    run.timestamp = std::chrono::system_clock::now();

    // Parse rsync --info=progress2 output:
    // "     N  N%   M.NNMBps   h:mm:ss"
    // We accumulate into a line buffer and regex-match progress lines.
    std::string line_buf;
    char rbuf[4096];
    ssize_t n;
    uint64_t last_bytes = 0;
    int last_pct = 0;
    std::string last_file;

    // Simple percent regex:  "  123456  45%  ..."
    static const std::regex pct_re(R"(\s+(\d+)\s+(\d+)%\s+\S+\s+\S+(?:\s+(.+?))?\r?)");

    while ((n = ::read(pfd[0], rbuf, sizeof(rbuf) - 1)) > 0) {
        rbuf[n] = '\0';
        line_buf += rbuf;
        // Process complete lines
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);
            std::smatch m;
            if (std::regex_search(line, m, pct_re)) {
                last_bytes = std::stoull(m[1].str());
                last_pct   = std::stoi(m[2].str());
                if (m.size() > 3 && m[3].matched) last_file = m[3].str();
                if (prog) prog(last_pct, last_bytes, last_file);
            }
        }
    }
    ::close(pfd[0]);

    run.bytes = last_bytes;

    int status = 0;
    ::waitpid(pid, &status, 0);
    run.success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!run.success) {
        run.error_msg = "rsync exited with code " +
                        std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

    return Result<BackupRun, SLError>::ok(run);
}

void Engine::write_manifest(const BackupProfile& p, const BackupRun& run) const {
    const fs::path mpath = manifest_path(p);

    json root = json::array();
    if (fs::exists(mpath)) {
        std::ifstream f(mpath);
        if (f) {
            try { f >> root; } catch (...) { root = json::array(); }
        }
    }

    json entry;
    entry["timestamp"]    = tp_to_epoch(run.timestamp);
    entry["files"]        = run.files;
    entry["bytes"]        = run.bytes;
    entry["success"]      = run.success;
    entry["error_msg"]    = run.error_msg;
    entry["snapshot_dir"] = run.snapshot_dir;
    root.push_back(std::move(entry));

    std::ofstream f(mpath);
    if (f) f << root.dump(2);
}

Result<BackupRun, SLError> Engine::run_backup(const BackupProfile& p, ProgressFn prog) {
    // Ensure destination exists
    std::error_code ec;
    fs::create_directories(p.destination, ec);
    if (ec) {
        return Result<BackupRun, SLError>::error(
            make_err(SLErrorCode::IOError,
                     "Cannot create destination: " + p.destination.string()));
    }

    const fs::path link_dest = latest_snapshot(p);
    const auto now = std::chrono::system_clock::now();
    const fs::path snap_dir = p.destination / snapshot_name(now);

    auto args = build_args(p, link_dest, snap_dir);
    auto run_res = exec_rsync(args, std::move(prog));
    if (!run_res.has_value()) return run_res;

    BackupRun run = run_res.value();
    run.snapshot_dir = snap_dir.string();
    write_manifest(p, run);

    return Result<BackupRun, SLError>::ok(run);
}

Result<std::vector<BackupRun>, SLError> Engine::history(const BackupProfile& p) const {
    const fs::path mpath = manifest_path(p);
    if (!fs::exists(mpath)) {
        return Result<std::vector<BackupRun>, SLError>::ok({});
    }

    std::ifstream f(mpath);
    if (!f) {
        return Result<std::vector<BackupRun>, SLError>::error(
            make_err(SLErrorCode::IOError,
                     "Cannot open manifest: " + mpath.string()));
    }

    json root;
    try { f >> root; }
    catch (const std::exception& ex) {
        return Result<std::vector<BackupRun>, SLError>::error(
            make_err(SLErrorCode::ParseError,
                     std::string("Manifest parse error: ") + ex.what()));
    }

    std::vector<BackupRun> runs;
    for (const auto& je : root) {
        BackupRun r;
        r.timestamp    = epoch_to_tp(je.value("timestamp", int64_t{0}));
        r.files        = je.value("files",    uint64_t{0});
        r.bytes        = je.value("bytes",    uint64_t{0});
        r.success      = je.value("success",  false);
        r.error_msg    = je.value("error_msg", "");
        r.snapshot_dir = je.value("snapshot_dir", "");
        runs.push_back(r);
    }
    return Result<std::vector<BackupRun>, SLError>::ok(runs);
}

Result<void, SLError> Engine::restore(const BackupProfile& p,
                                       const fs::path& to,
                                       std::chrono::system_clock::time_point snapshot) {
    // Find the snapshot dir closest to the requested time
    const std::string target_name = snapshot_name(snapshot);

    if (!fs::exists(p.destination)) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::NotFound,
                     "Destination does not exist: " + p.destination.string()));
    }

    // Collect snapshot dirs
    std::vector<fs::path> snaps;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(p.destination, ec)) {
        if (entry.is_directory()) {
            const std::string name = entry.path().filename().string();
            if (name.size() == 17 && name[10] == '_') snaps.push_back(entry.path());
        }
    }
    if (snaps.empty()) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::NotFound, "No snapshots found"));
    }
    std::sort(snaps.begin(), snaps.end());

    // Pick closest snapshot
    fs::path best_snap = snaps[0];
    for (auto& s : snaps) {
        if (s.filename().string() <= target_name) best_snap = s;
    }

    // Create restore target
    fs::create_directories(to, ec);
    if (ec) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError,
                     "Cannot create restore dir: " + to.string()));
    }

    // rsync snapshot -> to
    std::vector<std::string> args = {
        "rsync", "-a", "--info=progress2",
        best_snap.string() + "/",
        to.string() + "/"
    };
    auto res = exec_rsync(args, {});
    if (!res.has_value()) return Result<void, SLError>::error(res.error());
    if (!res.value().success) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal, res.value().error_msg));
    }
    return Result<void, SLError>::ok();
}

} // namespace straylight::backup
