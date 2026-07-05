#include "entropy_daemon.h"

#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {
namespace {

constexpr const char* kDefaultSocketPath = "/run/straylight/entropy.sock";
constexpr size_t kMaxRequestBytes = 1024 * 1024;

bool send_all(int fd, std::string_view payload) {
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = ::send(fd,
                           payload.data() + sent,
                           payload.size() - sent,
                           MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::optional<nlohmann::json> receive_json_line(int fd, int timeout_ms) {
    std::string payload;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (payload.find('\n') == std::string::npos &&
           payload.size() < kMaxRequestBytes) {
        auto now = std::chrono::steady_clock::now();
        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count());
        if (remaining_ms <= 0) return std::nullopt;

        pollfd pfd{fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, remaining_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return std::nullopt;

        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (n <= 0) return std::nullopt;
        payload.append(buf, static_cast<size_t>(n));
    }

    auto newline = payload.find('\n');
    if (newline != std::string::npos) {
        payload.resize(newline);
    }
    if (payload.empty()) return std::nullopt;
    return nlohmann::json::parse(payload);
}

int read_kernel_int(const char* path, int fallback) {
    std::ifstream f(path);
    int value = fallback;
    if (f) {
        f >> value;
    }
    return value;
}

} // namespace

Result<void, SLError> EntropyDaemon::init(const Config& cfg) {
    health_interval_s_ = cfg.get<int>("entropy.health_interval_s", 60);
    reseed_interval_s_ = cfg.get<int>("entropy.reseed_interval_s", 3600);
    socket_path_ = cfg.get<std::string>(
        "entropy.socket_path",
        cfg.get<std::string>("ipc.socket_path", kDefaultSocketPath));

    // Create hardware entropy source if not injected (normal path).
    if (!source_) {
        source_ = std::make_unique<hw::EntropySource>();
    }

    hardware_rng_available_ = source_->has_hardware_rng();
    SL_INFO("entropy: hardware RNG available: {}",
            hardware_rng_available_ ? "yes" : "no");

    // Seed the AES-backed DRBG from hardware/kernel entropy.
    std::array<uint8_t, 32> seed_buf{};
    auto fill_res = source_->fill(seed_buf.data(), seed_buf.size());
    if (!fill_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::HardwareFault,
                    "entropy: failed to obtain seed: " + fill_res.error()});
    }

    auto seed_res = drbg_.seed(seed_buf);
    std::fill(seed_buf.begin(), seed_buf.end(), 0);
    if (!seed_res.has_value()) {
        return Result<void, SLError>::error(seed_res.error());
    }

    auto socket_res = setup_ipc();
    if (!socket_res.has_value()) {
        return socket_res;
    }

    // Run initial health check.
    auto hc = run_health_check();
    if (!hc.has_value()) {
        SL_WARN("entropy: initial health check failed: {}", hc.error().message());
    }

    auto now = std::chrono::steady_clock::now();
    started_at_ = now;
    last_health_ = now;
    last_reseed_ = now;

    SL_INFO("entropy: daemon initialized (health={}s, reseed={}s, socket={})",
            health_interval_s_, reseed_interval_s_, socket_path_);
    return Result<void, SLError>::ok();
}

Result<void, SLError> EntropyDaemon::tick() {
    auto now = std::chrono::steady_clock::now();

    // Periodic health check.
    auto since_health = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_health_);
    if (since_health.count() >= health_interval_s_) {
        auto hc = run_health_check();
        if (!hc.has_value()) {
            SL_ERROR("entropy: health check failed: {}", hc.error().message());
        }
        last_health_ = now;
    }

    // Periodic reseed.
    auto since_reseed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_reseed_);
    if (since_reseed.count() >= reseed_interval_s_) {
        std::array<uint8_t, 32> fresh{};
        auto fill_res = source_->fill(fresh.data(), fresh.size());
        if (fill_res.has_value()) {
            auto rs = drbg_.reseed(fresh);
            if (rs.has_value()) {
                SL_DEBUG("entropy: DRBG reseeded");
            } else {
                SL_WARN("entropy: reseed failed: {}", rs.error().message());
            }
        } else {
            SL_WARN("entropy: could not read hardware entropy for reseed: {}",
                    fill_res.error());
        }
        std::fill(fresh.begin(), fresh.end(), 0);
        last_reseed_ = now;
    }

    handle_ipc(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    return Result<void, SLError>::ok();
}

void EntropyDaemon::shutdown() {
    SL_INFO("entropy: shutting down");
    teardown_ipc();
    source_.reset();
}

Result<void, SLError> EntropyDaemon::run_health_check() {
    if (!source_) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "entropy source not initialized"});
    }

    ++health_check_count_;
    auto hc = source_->health_check();
    if (!hc.has_value()) {
        ++health_failure_count_;
        last_health_ok_ = false;
        last_health_error_ = hc.error();
        return Result<void, SLError>::error(
            SLError{SLErrorCode::HardwareFault,
                    "entropy health check failed: " + hc.error()});
    }

    last_health_ok_ = true;
    last_health_error_.clear();
    SL_DEBUG("entropy: health check passed");
    return Result<void, SLError>::ok();
}

Result<void, SLError> EntropyDaemon::setup_ipc() {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(socket_path_).parent_path(), ec);

    ::unlink(socket_path_.c_str());
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("entropy socket(): ") + std::strerror(errno)});
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        auto err = std::string("entropy bind(): ") + std::strerror(errno);
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
    }

    if (auto* group = ::getgrnam("straylight")) {
        if (::chown(socket_path_.c_str(), 0, group->gr_gid) != 0) {
            auto err = std::string("entropy chown(): ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            ::unlink(socket_path_.c_str());
            return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
        }
        ::chmod(socket_path_.c_str(), 0660);
    } else {
        ::chmod(socket_path_.c_str(), 0600);
    }

    if (::listen(server_fd_, SOMAXCONN) < 0) {
        auto err = std::string("entropy listen(): ") + std::strerror(errno);
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
    }

    SL_INFO("entropy: IPC listening on {}", socket_path_);
    return Result<void, SLError>::ok();
}

void EntropyDaemon::teardown_ipc() {
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
}

void EntropyDaemon::handle_ipc(int timeout_ms) {
    if (server_fd_ < 0) return;

    pollfd pfd{server_fd_, POLLIN, 0};
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr < 0 && errno == EINTR) return;
    if (pr <= 0) return;

    while (true) {
        int client_fd = ::accept4(server_fd_, nullptr, nullptr,
                                  SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            SL_WARN("entropy: accept4() failed: {}", std::strerror(errno));
            return;
        }

        handle_client(client_fd);
        ::close(client_fd);
    }
}

void EntropyDaemon::handle_client(int fd) {
    nlohmann::json response;
    try {
        auto request = receive_json_line(fd, 500);
        if (!request) {
            response = {{"status", "error"}, {"message", "empty or timed-out request"}};
        } else {
            response = dispatch(*request);
        }
    } catch (const std::exception& e) {
        response = {{"status", "error"}, {"message", e.what()}};
    }

    auto payload = response.dump();
    payload.push_back('\n');
    if (!send_all(fd, payload)) {
        SL_WARN("entropy: failed to send IPC response");
    }
}

nlohmann::json EntropyDaemon::dispatch(const nlohmann::json& request) const {
    const bool wants_jsonrpc = request.contains("jsonrpc");
    auto id = request.value("id", nlohmann::json(nullptr));
    std::string method = request.value("cmd", request.value("method", ""));

    nlohmann::json result;
    if (method == "entropy.drbg_stats" || method == "drbg_stats") {
        result = build_drbg_stats();
    } else if (method == "pool_status" || method == "status") {
        result = build_pool_status();
    } else {
        nlohmann::json err = {
            {"status", "error"},
            {"message", "unknown method: " + method},
        };
        if (wants_jsonrpc) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32601}, {"message", err["message"]}}},
            };
        }
        return err;
    }

    if (wants_jsonrpc) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", result},
        };
    }
    return result;
}

nlohmann::json EntropyDaemon::build_drbg_stats() const {
    auto stats = drbg_.stats();

    nlohmann::json instance = {
        {"name", stats.name},
        {"algorithm", stats.algorithm},
        {"seeded", stats.seeded},
        {"bytes_generated", stats.bytes_generated},
        {"generate_calls", stats.generate_calls},
        {"reseed_count", stats.reseed_count},
        {"health_ok", stats.health_ok},
    };
    if (!stats.last_error.empty()) {
        instance["last_error"] = stats.last_error;
    }

    return {
        {"status", "ok"},
        {"instances", nlohmann::json::array({instance})},
    };
}

nlohmann::json EntropyDaemon::build_pool_status() const {
    const int avail = read_kernel_int("/proc/sys/kernel/random/entropy_avail", 0);
    const int poolsize = read_kernel_int("/proc/sys/kernel/random/poolsize", 256);
    const double pct = poolsize > 0
        ? (static_cast<double>(avail) / static_cast<double>(poolsize)) * 100.0
        : 0.0;
    auto drbg_stats = build_drbg_stats();
    auto stats = drbg_.stats();
    auto now = std::chrono::steady_clock::now();

    nlohmann::json j = {
        {"status", "ok"},
        {"health", (last_health_ok_ && stats.health_ok) ? "ok" : "warn"},
        {"avail", avail},
        {"poolsize", poolsize},
        {"pct", pct},
        {"hardware_rng_available", hardware_rng_available_},
        {"source", {
            {"rdrand", hardware_rng_available_},
            {"urandom_fallback", true},
        }},
        {"kernel_source", build_kernel_source_status()},
        {"drbgs", drbg_stats["instances"]},
        {"health_checks", health_check_count_},
        {"health_failures", health_failure_count_},
        {"health_interval_s", health_interval_s_},
        {"reseed_interval_s", reseed_interval_s_},
        {"socket", socket_path_},
        {"uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(
            now - started_at_).count()},
    };
    if (!last_health_error_.empty()) {
        j["last_health_error"] = last_health_error_;
    }
    return j;
}

nlohmann::json EntropyDaemon::build_kernel_source_status() const {
    std::ifstream f("/proc/straylight/entropy");
    if (!f) {
        return {
            {"online", false},
            {"path", "/proc/straylight/entropy"},
        };
    }

    nlohmann::json j = {
        {"online", true},
        {"path", "/proc/straylight/entropy"},
        {"rdrand", false},
        {"rdseed", false},
        {"jitter", false},
        {"bytes_generated", 0},
        {"rdrand_calls", 0},
        {"rdseed_calls", 0},
        {"jitter_calls", 0},
        {"health_failures", 0},
    };

    std::string line;
    while (std::getline(f, line)) {
        auto value_after_colon = [&]() -> uint64_t {
            auto colon = line.find(':');
            if (colon == std::string::npos) return 0;
            std::istringstream iss(line.substr(colon + 1));
            uint64_t value = 0;
            iss >> value;
            return value;
        };
        if (line.find("RDRAND:") != std::string::npos) {
            j["rdrand"] = line.find("available") != std::string::npos;
        } else if (line.find("RDSEED:") != std::string::npos) {
            j["rdseed"] = line.find("available") != std::string::npos;
        } else if (line.find("Jitter:") != std::string::npos) {
            j["jitter"] = line.find("available") != std::string::npos;
        } else if (line.find("Bytes generated:") != std::string::npos) {
            j["bytes_generated"] = value_after_colon();
        } else if (line.find("RDRAND calls:") != std::string::npos) {
            j["rdrand_calls"] = value_after_colon();
        } else if (line.find("RDSEED calls:") != std::string::npos) {
            j["rdseed_calls"] = value_after_colon();
        } else if (line.find("Jitter calls:") != std::string::npos) {
            j["jitter_calls"] = value_after_colon();
        } else if (line.find("Health failures:") != std::string::npos) {
            j["health_failures"] = value_after_colon();
        }
    }

    return j;
}

} // namespace straylight
