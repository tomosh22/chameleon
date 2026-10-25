#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include "cml/util/diag.h"
#include "cml/util/fail_fast.h"

namespace cml {

namespace detail {
// A failed Result must always carry a real diagnostic (CML1xxx..CML5xxx).
inline Diag require_valid_diag(Diag error) {
    if (!error.valid()) {
        fail_fast("Result constructed from an invalid Diag (code outside CML1xxx..CML5xxx)");
    }
    return error;
}
}  // namespace detail

// Result<T>: either a T or a Diag, never both and never neither. The only error-handling vehicle
// in core/ and agent/.
//
//   Result<int> parse(...);
//   auto r = parse(...);
//   if (!r) { log(r.error().format()); return r.error(); }
//   use(*r);
//
// Reading value() of a failed result (or error() of a successful one) is a programmer error and
// aborts via cml::fail_fast, as does constructing a failed Result from an invalid Diag.
// T must not be Diag.
template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Diag>, "Result<Diag> is ambiguous");

public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT: implicit
    Result(Diag error)                                                      // NOLINT: implicit
        : storage_(std::in_place_index<1>, detail::require_valid_diag(std::move(error))) {}

    bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        require_value();
        return *std::get_if<0>(&storage_);
    }
    const T& value() const& {
        require_value();
        return *std::get_if<0>(&storage_);
    }
    T&& value() && {
        require_value();
        return std::move(*std::get_if<0>(&storage_));
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    const Diag& error() const {
        if (has_value()) {
            fail_fast("Result::error() called on a successful result");
        }
        return *std::get_if<1>(&storage_);
    }

    template <class U>
    T value_or(U&& fallback) const& {
        return has_value() ? *std::get_if<0>(&storage_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    void require_value() const {
        if (!has_value()) {
            fail_fast("Result::value() called on a failed result");
        }
    }

    std::variant<T, Diag> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Diag error)  // NOLINT: implicit by design
        : storage_(std::in_place_index<1>, detail::require_valid_diag(std::move(error))) {}

    bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    const Diag& error() const {
        if (has_value()) {
            fail_fast("Result::error() called on a successful result");
        }
        return *std::get_if<1>(&storage_);
    }

private:
    std::variant<std::monostate, Diag> storage_;
};

}  // namespace cml
