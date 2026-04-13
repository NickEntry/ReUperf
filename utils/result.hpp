#ifndef RESULT_HPP
#define RESULT_HPP

#include <string>
#include <optional>
#include <functional>

template<typename T>
struct Result {
    T value;
    std::string error_msg;
    bool success;

    static Result ok(T val) {
        return Result{val, "", true};
    }

    static Result err(std::string msg) {
        return Result{T{}, std::move(msg), false};
    }

    bool has_value() const { return success; }
    const T& operator*() const { return value; }
    const T* operator->() const { return &value; }

    explicit operator bool() const { return success; }

    template<typename U>
    Result<U> map(std::function<U(T)> f) const {
        if (success) {
            return Result<U>::ok(f(value));
        } else {
            return Result<U>::err(error_msg);
        }
    }

    template<typename U>
    Result<U> flat_map(std::function<Result<U>(T)> f) const {
        if (success) {
            return f(value);
        } else {
            return Result<U>::err(error_msg);
        }
    }

    T value_or(T default_value) const {
        return success ? value : default_value;
    }

    std::optional<T> optional() const {
        return success ? std::make_optional(value) : std::nullopt;
    }
};

template<>
struct Result<void> {
    std::string error_msg;
    bool success;

    static Result ok() {
        return Result{"", true};
    }

    static Result err(std::string msg) {
        return Result{std::move(msg), false};
    }

    explicit operator bool() const { return success; }
};

template<typename T>
Result(T) -> Result<T>;

#endif
