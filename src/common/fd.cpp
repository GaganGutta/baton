#include "common/fd.h"

#include <unistd.h>

namespace baton {

void Fd::reset(int fd) {
  if (fd_ >= 0) {
    // The result is deliberately ignored. On Linux the descriptor is released
    // even when close() reports EINTR, so retrying could close a descriptor
    // that another thread has just been handed. Durability never relies on
    // close(): everything that must survive is fsynced explicitly first.
    ::close(fd_);
  }
  fd_ = fd;
}

}  // namespace baton
