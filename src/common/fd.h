#pragma once

#include <utility>

namespace baton {

// Owns a POSIX file descriptor and closes it on destruction. Move-only.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { reset(); }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) reset(std::exchange(other.fd_, -1));
    return *this;
  }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  // Gives up ownership without closing.
  int release() { return std::exchange(fd_, -1); }

  // Closes the current descriptor (if any) and takes ownership of `fd`.
  void reset(int fd = -1);

 private:
  int fd_ = -1;
};

}  // namespace baton
