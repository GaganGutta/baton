#pragma once

// BATON_CHECK guards invariants: conditions that can only be false if baton
// itself has a bug (or memory is corrupted). A failed check prints the location
// and aborts. It is always on, in every build type: continuing with a broken
// invariant in a system whose job is durability is worse than crashing, because
// the durable log lets a restart recover to a known-good state.
//
// Expected runtime failures (bad input, I/O errors, limits) are never checks;
// they are reported through Result<T> (see result.h).

#include <format>
#include <string>

namespace baton::detail {

[[noreturn]] void check_failed(const char* file, int line, const char* expr,
                               const std::string& message = {});

}  // namespace baton::detail

#define BATON_CHECK(cond, ...)                                                       \
  do {                                                                               \
    if (!(cond)) [[unlikely]] {                                                      \
      ::baton::detail::check_failed(__FILE__, __LINE__,                              \
                                    #cond __VA_OPT__(, ::std::format(__VA_ARGS__))); \
    }                                                                                \
  } while (false)

// Marks code paths that must be unreachable, e.g. after an exhaustive switch.
#define BATON_UNREACHABLE(...)                      \
  ::baton::detail::check_failed(__FILE__, __LINE__, \
                                "unreachable" __VA_OPT__(, ::std::format(__VA_ARGS__)))
