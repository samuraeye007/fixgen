#pragma once
#include <optional>
#include <string>
#include <utility>

namespace fixcore {

// Minimal expected-like type (keeps us on C++20).
template <class T>
class Result {
public:
    Result(T v) : value_(std::move(v)) {}
    static Result fail(std::string e) { Result r; r.error_ = std::move(e); return r; }
    explicit operator bool() const { return value_.has_value(); }
    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T* operator->() { return &*value_; }
    const std::string& error() const { return error_; }

private:
    Result() = default;
    std::optional<T> value_;
    std::string error_;
};

}  // namespace fixcore
