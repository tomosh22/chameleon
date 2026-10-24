#pragma once

#include <optional>
#include <utility>

#include "cml/util/diag.h"
#include "cml/util/fail_fast.h"

namespace cml {

// Result<T>: either a T or a Diag. The only error-handling vehicle in core/ and agent/.
//
//   Result<int> parse(...);
//   auto r = parse(...);
//   if (!r) { log(r.error().format()); return r.error(); }
//   use(*r);
//
// Reading value() of a failed result (or error() of a successful one) is a programmer error and
// aborts via cml::fail_fast.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}  // NOLINT: implicit by design
    Result(Diag error) : error_(std::move(error)) {}  // NOLINT: implicit by design

    bool has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        require_value();
        return *value_;
    }
    const T& value() const& {
        require_value();
        return *value_;
    }
    T&& value() && {
        require_value();
        return std::move(*value_);
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    const Diag& error() const {
        if (value_.has_value()) {
            fail_fast("Result::error() called on a successful result");
        }
        return error_;
    }

    template <class U>
    T value_or(U&& fallback) const& {
        return value_.has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
    }

private:
    void require_value() const {
        if (!value_.has_value()) {
            fail_fast("Result::value() called on a failed result");
        }
    }

    std::optional<T> value_;
    Diag error_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Diag error) : error_(std::move(error)), failed_(true) {}  // NOLINT: implicit by design

    bool has_value() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return has_value(); }

    const Diag& error() const {
        if (!failed_) {
            fail_fast("Result::error() called on a successful result");
        }
        return error_;
    }

private:
    Diag error_;
    bool failed_ = false;
};

}  // namespace cml
