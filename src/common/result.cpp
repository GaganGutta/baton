#include "common/result.h"

#include <cstring>
#include <format>

namespace baton {

std::string_view to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::kInvalidArgument:
      return "invalid argument";
    case ErrorCode::kNotFound:
      return "not found";
    case ErrorCode::kAlreadyExists:
      return "already exists";
    case ErrorCode::kFailedPrecondition:
      return "failed precondition";
    case ErrorCode::kStaleToken:
      return "stale token";
    case ErrorCode::kLimitExceeded:
      return "limit exceeded";
    case ErrorCode::kUnauthenticated:
      return "unauthenticated";
    case ErrorCode::kProtocol:
      return "protocol error";
    case ErrorCode::kIo:
      return "io error";
    case ErrorCode::kCorruption:
      return "corruption";
    case ErrorCode::kUnavailable:
      return "unavailable";
    case ErrorCode::kInternal:
      return "internal error";
  }
  return "unknown";
}

std::string Error::to_string() const {
  return std::format("{}: {}", baton::to_string(code_), message_);
}

Error io_error(std::string_view what, int saved_errno) {
  return Error{ErrorCode::kIo, std::format("{}: {}", what, std::strerror(saved_errno))};
}

}  // namespace baton
