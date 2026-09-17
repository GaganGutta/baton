#include "common/check.h"

#include <cstdio>
#include <cstdlib>

namespace baton::detail {

void check_failed(const char* file, int line, const char* expr, const std::string& message) {
  // Written straight to stderr rather than through the logger: the logger may
  // be the thing that is broken, and abort() must follow no matter what. For
  // the same reason the results of the stdio calls are deliberately ignored.
  if (message.empty()) {
    (void)std::fprintf(stderr, "FATAL %s:%d: check failed: %s\n", file, line, expr);
  } else {
    (void)std::fprintf(stderr, "FATAL %s:%d: check failed: %s: %s\n", file, line, expr,
                       message.c_str());
  }
  (void)std::fflush(stderr);
  std::abort();
}

}  // namespace baton::detail
