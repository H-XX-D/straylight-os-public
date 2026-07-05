// lib/common/include/straylight/ipc.h
#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <memory>
#include <string>
#include <string_view>

namespace straylight {

/// A connected IPC socket (either client or accepted server connection).
class STRAYLIGHT_EXPORT IpcConnection {
public:
    ~IpcConnection();
    IpcConnection(IpcConnection&&) noexcept;
    IpcConnection& operator=(IpcConnection&&) noexcept;

    /// Send a length-prefixed message.
    Result<void, std::string> send(std::string_view message);

    /// Receive a length-prefixed message. Blocks until data available.
    Result<std::string, std::string> receive();

    /// Get the underlying file descriptor (for epoll integration).
    [[nodiscard]] int fd() const noexcept;

private:
    friend class IpcServer;
    friend class IpcClient;
    explicit IpcConnection(int fd);

    int fd_ = -1;
};

/// Unix domain socket server.
class STRAYLIGHT_EXPORT IpcServer {
public:
    IpcServer();
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;

    /// Bind to a socket path and listen.
    Result<void, std::string> bind(const std::string& path);

    /// Accept a connection. timeout_ms=0 means block forever.
    Result<std::unique_ptr<IpcConnection>, std::string> accept(int timeout_ms = 0);

private:
    int fd_ = -1;
    std::string path_;
};

/// Unix domain socket client.
class STRAYLIGHT_EXPORT IpcClient : public IpcConnection {
public:
    IpcClient();

    /// Connect to a server at the given socket path.
    Result<void, std::string> connect(const std::string& path);
};

} // namespace straylight
