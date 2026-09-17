#pragma once

// Result<T> is how baton reports expected failures. See docs/design.md
// ("Error handling") for why baton uses values instead of exceptions.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "common/check.h"

namespace baton {

enum class ErrorCode : uint8_t {
  kInvalidArgument,     // malformed input from a caller or client
  kNotFound,            // the named entity does not exist
  kAlreadyExists,       // the named entity exists and may not be replaced
  kFailedPrecondition,  // the entity is in the wrong state for this operation
  kStaleToken,          // a lease token that is no longer the current one
  kLimitExceeded,       // a configured limit (payload, memory, connections) was hit
  kUnauthenticated,     // AUTH required or wrong password
  kProtocol,            // bytes on the wire are not valid RESP
  kIo,                  // the operating system reported an I/O failure
  kCorruption,          // persistent data failed validation
  kUnavailable,         // temporarily unable to serve (e.g. shutting down)
  kInternal,            // should not happen; reported instead of aborting only at boundaries
};

std::string_view to_string(ErrorCode code);

class [[nodiscard]] Error {
 public:
  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }

  // "<code>: <message>", for logs and test output.
  std::string to_string() const;

 private:
  ErrorCode code_;
  std::string message_;
};

// Builds an Error from the current errno, e.g. "open /x/y: No such file or directory".
Error io_error(std::string_view what, int saved_errno);

template <class T>
class [[nodiscard]] Result {
 public:
  // Implicit on purpose: `return value;` and `return Error{...};` both read naturally.
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const { return v_.index() == 0; }
  explicit operator bool() const { return ok(); }

  T& value() & {
    BATON_CHECK(ok(), "Result::value() on error: {}", error().to_string());
    return std::get<0>(v_);
  }
  const T& value() const& {
    BATON_CHECK(ok(), "Result::value() on error: {}", error().to_string());
    return std::get<0>(v_);
  }
  T&& value() && {
    BATON_CHECK(ok(), "Result::value() on error: {}", error().to_string());
    return std::get<0>(std::move(v_));
  }

  const Error& error() const {
    BATON_CHECK(!ok(), "Result::error() on success");
    return std::get<1>(v_);
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::in_place, std::move(error)) {}

  bool ok() const { return !error_.has_value(); }
  explicit operator bool() const { return ok(); }

  const Error& error() const {
    BATON_CHECK(error_.has_value(), "Result::error() on success");
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

using Status = Result<void>;

}  // namespace baton

#define BATON_CONCAT_INNER(a, b) a##b
#define BATON_CONCAT(a, b) BATON_CONCAT_INNER(a, b)

// Propagates the error of a Status/Result expression to the caller.
#define BATON_RETURN_IF_ERROR(expr)                        \
  do {                                                     \
    auto baton_status_ = (expr);                           \
    if (!baton_status_.ok()) return baton_status_.error(); \
  } while (false)

// BATON_ASSIGN_OR_RETURN(auto x, compute()); declares x or returns the error.
// `lhs` is a declaration and `tmp` an identifier, so neither can be parenthesized.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define BATON_ASSIGN_OR_RETURN(lhs, expr) \
  BATON_ASSIGN_OR_RETURN_IMPL(BATON_CONCAT(baton_result_, __LINE__), lhs, expr)
#define BATON_ASSIGN_OR_RETURN_IMPL(tmp, lhs, expr) \
  auto tmp = (expr);                                \
  if (!tmp.ok()) return tmp.error();                \
  lhs = std::move(tmp).value()
// NOLINTEND(bugprone-macro-parentheses)
