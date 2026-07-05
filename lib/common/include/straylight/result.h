// lib/common/include/straylight/result.h
#pragma once

#include <functional>
#include <stdexcept>
#include <variant>

namespace straylight {

/// Result<T, E> — a value-or-error type inspired by Rust's Result and C++23 std::expected.
/// Use this instead of exceptions for recoverable errors.
template <typename T, typename E>
class Result {
public:
    /// Create a successful result.
    static Result ok(T value) { return Result(std::move(value)); }

    /// Create an error result.
    static Result error(E err) { return Result(err_tag{}, std::move(err)); }

    /// Returns true if this result contains a value (not an error).
    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    /// Allows if(result) as a shorthand for has_value().
    explicit operator bool() const noexcept { return has_value(); }

    /// Returns the contained value. Throws if this is an error.
    [[nodiscard]] const T& value() const& {
        if (!has_value()) {
            throw std::logic_error("Result::value() called on error");
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        if (!has_value()) {
            throw std::logic_error("Result::value() called on error");
        }
        return std::get<T>(std::move(storage_));
    }

    /// Returns the contained error. Throws if this is a value.
    [[nodiscard]] const E& error() const& {
        if (has_value()) {
            throw std::logic_error("Result::error() called on value");
        }
        return std::get<err_wrapper>(storage_).err;
    }

    /// Returns value if ok, or the provided default if error.
    [[nodiscard]] T value_or(T default_val) const& {
        return has_value() ? std::get<T>(storage_) : std::move(default_val);
    }

    /// Transform the value with f, pass through errors unchanged.
    template <typename F>
    auto map(F&& f) const -> Result<decltype(f(std::declval<T>())), E> {
        using U = decltype(f(std::declval<T>()));
        if (has_value()) {
            return Result<U, E>::ok(f(std::get<T>(storage_)));
        }
        return Result<U, E>::error(std::get<err_wrapper>(storage_).err);
    }

    /// Chain operations that return Result.
    template <typename F>
    auto and_then(F&& f) const -> decltype(f(std::declval<T>())) {
        using R = decltype(f(std::declval<T>()));
        if (has_value()) {
            return f(std::get<T>(storage_));
        }
        return R::error(std::get<err_wrapper>(storage_).err);
    }

private:
    struct err_tag {};
    struct err_wrapper { E err; };

    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(err_tag, E err) : storage_(err_wrapper{std::move(err)}) {}

    std::variant<T, err_wrapper> storage_;
};

/// Specialization for void value type (used by IPC, config, etc.).
template <typename E>
class Result<void, E> {
public:
    static Result ok() { return Result(true); }
    static Result error(E err) { return Result(std::move(err)); }
    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const E& error() const& {
        if (ok_) throw std::logic_error("Result::error() called on ok");
        return err_;
    }
private:
    explicit Result(bool) : ok_(true) {}
    explicit Result(E err) : ok_(false), err_(std::move(err)) {}
    bool ok_ = false;
    E err_{};
};

/// Convenience alias: VoidResult<E> == Result<void, E>
template <typename E = std::string>
using VoidResult = Result<void, E>;

} // namespace straylight
