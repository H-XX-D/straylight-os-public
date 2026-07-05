// apps/file_manager/operations.h
// File operations — copy, move, delete, rename, create
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <string>

namespace straylight::file_manager {

namespace fs = std::filesystem;

/// Progress callback: (bytes_done, bytes_total) -> should_cancel
using ProgressCallback = std::function<bool(uintmax_t, uintmax_t)>;

/// Status of an async file operation.
struct OperationStatus {
    bool completed = false;
    bool success = false;
    std::string error_message;
    uintmax_t bytes_done = 0;
    uintmax_t bytes_total = 0;
    std::string current_file;
};

/// Async file operation handle.
class AsyncOperation {
public:
    AsyncOperation();
    ~AsyncOperation();

    AsyncOperation(const AsyncOperation&) = delete;
    AsyncOperation& operator=(const AsyncOperation&) = delete;
    AsyncOperation(AsyncOperation&&) noexcept;
    AsyncOperation& operator=(AsyncOperation&&) noexcept;

    /// Get the current status.
    [[nodiscard]] OperationStatus status() const;

    /// Request cancellation.
    void cancel();

    /// Check if cancelled.
    [[nodiscard]] bool is_cancelled() const;

    /// Wait for completion.
    void wait();

    /// Check if completed.
    [[nodiscard]] bool is_done() const;

private:
    friend class Operations;
    std::shared_ptr<std::atomic<bool>> cancel_flag_;
    std::shared_ptr<OperationStatus> status_;
    std::future<void> future_;
};

/// File operations manager.
class Operations {
public:
    /// Copy a file or directory (recursive for dirs).
    static Result<void, std::string> copy(const fs::path& src,
                                           const fs::path& dst);

    /// Async copy with progress reporting.
    static AsyncOperation copy_async(const fs::path& src,
                                      const fs::path& dst);

    /// Move/rename a file or directory.
    static Result<void, std::string> move(const fs::path& src,
                                           const fs::path& dst);

    /// Delete a file or directory (recursive for dirs).
    static Result<void, std::string> remove(const fs::path& path);

    /// Rename a file or directory.
    static Result<void, std::string> rename(const fs::path& path,
                                             const std::string& new_name);

    /// Create a new directory.
    static Result<void, std::string> create_directory(const fs::path& path);

    /// Create a new empty file.
    static Result<void, std::string> create_file(const fs::path& path);

    /// Get the total size of a file or directory tree.
    static Result<uintmax_t, std::string> total_size(const fs::path& path);

    /// Check if a file/directory exists.
    static bool exists(const fs::path& path);

private:
    /// Recursive copy helper.
    static void copy_recursive(const fs::path& src, const fs::path& dst,
                                std::shared_ptr<OperationStatus> status,
                                std::shared_ptr<std::atomic<bool>> cancel);
};

} // namespace straylight::file_manager
