// apps/file_manager/operations.cpp
// File operations implementation using std::filesystem with async support
#include "operations.h"

#include <fstream>
#include <thread>

namespace straylight::file_manager {

AsyncOperation::AsyncOperation()
    : cancel_flag_(std::make_shared<std::atomic<bool>>(false)),
      status_(std::make_shared<OperationStatus>()) {}

AsyncOperation::~AsyncOperation() {
    if (future_.valid()) {
        cancel();
        future_.wait();
    }
}

AsyncOperation::AsyncOperation(AsyncOperation&& o) noexcept
    : cancel_flag_(std::move(o.cancel_flag_)),
      status_(std::move(o.status_)),
      future_(std::move(o.future_)) {}

AsyncOperation& AsyncOperation::operator=(AsyncOperation&& o) noexcept {
    if (this != &o) {
        if (future_.valid()) {
            cancel();
            future_.wait();
        }
        cancel_flag_ = std::move(o.cancel_flag_);
        status_ = std::move(o.status_);
        future_ = std::move(o.future_);
    }
    return *this;
}

OperationStatus AsyncOperation::status() const {
    if (!status_) return {};
    return *status_;
}

void AsyncOperation::cancel() {
    if (cancel_flag_) {
        cancel_flag_->store(true, std::memory_order_relaxed);
    }
}

bool AsyncOperation::is_cancelled() const {
    return cancel_flag_ && cancel_flag_->load(std::memory_order_relaxed);
}

void AsyncOperation::wait() {
    if (future_.valid()) {
        future_.wait();
    }
}

bool AsyncOperation::is_done() const {
    if (!status_) return true;
    return status_->completed;
}

Result<void, std::string> Operations::copy(const fs::path& src,
                                            const fs::path& dst) {
    std::error_code ec;

    if (!fs::exists(src, ec)) {
        return Result<void, std::string>::error(
            "Source does not exist: " + src.string());
    }

    if (fs::is_directory(src)) {
        fs::copy(src, dst, fs::copy_options::recursive |
                           fs::copy_options::overwrite_existing, ec);
    } else {
        // Ensure parent directory exists
        if (dst.has_parent_path()) {
            fs::create_directories(dst.parent_path(), ec);
        }
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }

    if (ec) {
        return Result<void, std::string>::error(
            "Copy failed: " + ec.message());
    }

    return Result<void, std::string>::ok();
}

AsyncOperation Operations::copy_async(const fs::path& src,
                                       const fs::path& dst) {
    AsyncOperation op;

    // Calculate total size
    auto total = total_size(src);
    if (total.has_value()) {
        op.status_->bytes_total = total.value();
    }

    auto status = op.status_;
    auto cancel = op.cancel_flag_;

    op.future_ = std::async(std::launch::async,
        [src, dst, status, cancel]() {
            copy_recursive(src, dst, status, cancel);
            status->completed = true;
            if (!cancel->load(std::memory_order_relaxed) &&
                status->error_message.empty()) {
                status->success = true;
            }
        });

    return op;
}

void Operations::copy_recursive(const fs::path& src, const fs::path& dst,
                                  std::shared_ptr<OperationStatus> status,
                                  std::shared_ptr<std::atomic<bool>> cancel) {
    if (cancel->load(std::memory_order_relaxed)) return;

    std::error_code ec;

    if (fs::is_directory(src, ec)) {
        fs::create_directories(dst, ec);
        if (ec) {
            status->error_message = "Failed to create directory: " + ec.message();
            return;
        }

        for (const auto& entry : fs::directory_iterator(src, ec)) {
            if (cancel->load(std::memory_order_relaxed)) return;

            fs::path dest_entry = dst / entry.path().filename();
            copy_recursive(entry.path(), dest_entry, status, cancel);

            if (!status->error_message.empty()) return;
        }
    } else if (fs::is_regular_file(src, ec)) {
        status->current_file = src.filename().string();

        // Ensure parent exists
        if (dst.has_parent_path()) {
            fs::create_directories(dst.parent_path(), ec);
        }

        // Copy file in chunks for progress tracking
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(dst, std::ios::binary);

        if (!in.is_open() || !out.is_open()) {
            status->error_message =
                "Failed to open file for copy: " + src.string();
            return;
        }

        constexpr size_t chunk_size = 1024 * 1024; // 1MB chunks
        std::vector<char> buffer(chunk_size);

        while (in && !cancel->load(std::memory_order_relaxed)) {
            in.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
            auto bytes_read = in.gcount();
            if (bytes_read > 0) {
                out.write(buffer.data(), bytes_read);
                status->bytes_done += static_cast<uintmax_t>(bytes_read);
            }
            if (bytes_read < static_cast<std::streamsize>(chunk_size)) break;
        }

        // Copy permissions
        auto perms = fs::status(src, ec).permissions();
        fs::permissions(dst, perms, ec);
    } else if (fs::is_symlink(src, ec)) {
        auto target = fs::read_symlink(src, ec);
        if (!ec) {
            fs::create_symlink(target, dst, ec);
        }
    }
}

Result<void, std::string> Operations::move(const fs::path& src,
                                            const fs::path& dst) {
    std::error_code ec;

    if (!fs::exists(src, ec)) {
        return Result<void, std::string>::error(
            "Source does not exist: " + src.string());
    }

    // Try rename first (fast, same filesystem)
    fs::rename(src, dst, ec);
    if (!ec) {
        return Result<void, std::string>::ok();
    }

    // Fall back to copy + delete (cross-filesystem)
    auto copy_result = copy(src, dst);
    if (!copy_result.has_value()) {
        return copy_result;
    }

    return remove(src);
}

Result<void, std::string> Operations::remove(const fs::path& path) {
    std::error_code ec;

    if (!fs::exists(path, ec)) {
        return Result<void, std::string>::error(
            "Path does not exist: " + path.string());
    }

    if (fs::is_directory(path, ec)) {
        auto removed = fs::remove_all(path, ec);
        (void)removed;
    } else {
        fs::remove(path, ec);
    }

    if (ec) {
        return Result<void, std::string>::error(
            "Remove failed: " + ec.message());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> Operations::rename(const fs::path& path,
                                              const std::string& new_name) {
    if (new_name.empty() || new_name.find('/') != std::string::npos) {
        return Result<void, std::string>::error(
            "Invalid name: " + new_name);
    }

    fs::path new_path = path.parent_path() / new_name;
    std::error_code ec;

    if (fs::exists(new_path, ec)) {
        return Result<void, std::string>::error(
            "A file with that name already exists");
    }

    fs::rename(path, new_path, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "Rename failed: " + ec.message());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> Operations::create_directory(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "Failed to create directory: " + ec.message());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> Operations::create_file(const fs::path& path) {
    if (fs::exists(path)) {
        return Result<void, std::string>::error(
            "File already exists: " + path.string());
    }

    // Ensure parent directory exists
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        return Result<void, std::string>::error(
            "Failed to create file: " + path.string());
    }
    file.close();

    return Result<void, std::string>::ok();
}

Result<uintmax_t, std::string> Operations::total_size(const fs::path& path) {
    std::error_code ec;

    if (!fs::exists(path, ec)) {
        return Result<uintmax_t, std::string>::error(
            "Path does not exist: " + path.string());
    }

    uintmax_t total = 0;

    if (fs::is_directory(path, ec)) {
        for (const auto& entry :
             fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec)) {
                total += entry.file_size(ec);
            }
        }
    } else {
        total = fs::file_size(path, ec);
    }

    if (ec) {
        return Result<uintmax_t, std::string>::error(
            "Failed to calculate size: " + ec.message());
    }

    return Result<uintmax_t, std::string>::ok(total);
}

bool Operations::exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

} // namespace straylight::file_manager
