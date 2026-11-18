#pragma once

#include <g90/error.hpp>
#include <optional>
#include <utility>

namespace g90 {

/// @brief Discriminated success/failure wrapper used throughout the public API.
///
/// On success, `value` is set and `error` is empty. On failure, `error` is set
/// and `value` is empty. Check with `ok()` before reading `value`. On failure,
/// use `err()` for the `Error` code.
///
/// Construct with `Result<T>::Ok(...)` or `Result<T>::Err(...)`. The type is
/// `[[nodiscard]]` — callers must check the outcome.
template<class T>
struct [[nodiscard]] Result {
  /// Set when `ok()` is false.
  std::optional<Error> error;
  /// Set when `ok()` is true.
  std::optional<T> value;

  /// @return @c true when the operation succeeded and `value` is available.
  [[nodiscard]] bool ok() const noexcept { return value.has_value(); }

  /// @brief Failure code for this result.
  /// @return The stored `Error` when `!ok()`; on success returns
  ///         `Error::NotImplemented` only as a sentinel (do not rely on it).
  [[nodiscard]] Error err() const { return error.value_or(Error::NotImplemented); }

  /// @brief Construct a successful result.
  /// @param v Payload value moved into the result.
  [[nodiscard]] static Result Ok(T v) {
    Result r;
    r.value = std::move(v);
    // error remains nullopt — success has no error code
    return r;
  }

  /// @brief Construct a failed result.
  /// @param e Error code describing the failure.
  [[nodiscard]] static Result Err(Error e) {
    Result r;
    r.error = e;
    return r;
  }
};

/// @brief `Result` specialization for operations with no return payload.
///
/// On success, `success` is @c true and `error` is empty. Use the same
/// `ok()` / `err()` contract as `Result<T>`.
template<>
struct [[nodiscard]] Result<void> {
  std::optional<Error> error;
  bool success = false;

  /// @return @c true when the operation succeeded.
  [[nodiscard]] bool ok() const noexcept { return success; }

  /// @brief Failure code when `!ok()`.
  [[nodiscard]] Error err() const { return error.value_or(Error::NotImplemented); }

  /// @brief Construct a successful void result.
  [[nodiscard]] static Result Ok() {
    Result r;
    r.success = true;
    return r;
  }

  /// @brief Construct a failed void result.
  /// @param e Error code describing the failure.
  [[nodiscard]] static Result Err(Error e) {
    Result r;
    r.error = e;
    r.success = false;
    return r;
  }
};

} // namespace g90
