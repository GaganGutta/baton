#if defined(__APPLE__) || defined(__FreeBSD__)

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

#include <array>
#include <cerrno>
#include <unordered_map>

#include "common/fd.h"
#include "net/poller.h"

namespace baton {
namespace {

// kqueue tracks read and write interest as two separate filters, so this keeps
// what is currently registered per descriptor and applies the difference.
class KqueuePoller final : public Poller {
 public:
  explicit KqueuePoller(Fd kqueue_fd) : kqueue_fd_(std::move(kqueue_fd)) {}

  Status add(int fd, bool want_read, bool want_write) override {
    registered_[fd] = Interest{};
    return modify(fd, want_read, want_write);
  }

  Status modify(int fd, bool want_read, bool want_write) override {
    Interest& current = registered_[fd];
    std::array<struct kevent, 2> changes{};
    int count = 0;
    if (want_read != current.read) {
      EV_SET(&changes[static_cast<size_t>(count++)], fd, EVFILT_READ,
             want_read ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    }
    if (want_write != current.write) {
      EV_SET(&changes[static_cast<size_t>(count++)], fd, EVFILT_WRITE,
             want_write ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    }
    if (count > 0 && ::kevent(kqueue_fd_.get(), changes.data(), count, nullptr, 0, nullptr) != 0) {
      return io_error("kevent", errno);
    }
    current = Interest{.read = want_read, .write = want_write};
    return {};
  }

  void remove(int fd) override {
    (void)modify(fd, false, false);
    registered_.erase(fd);
  }

  Status wait(int timeout_ms, std::vector<PollEvent>& events) override {
    std::array<struct kevent, 256> ready{};
    timespec timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000;
    const int n = ::kevent(kqueue_fd_.get(), nullptr, 0, ready.data(),
                           static_cast<int>(ready.size()), timeout_ms < 0 ? nullptr : &timeout);
    if (n < 0) {
      if (errno == EINTR) return {};
      return io_error("kevent", errno);
    }
    for (int i = 0; i < n; ++i) {
      const struct kevent& e = ready[static_cast<size_t>(i)];
      events.push_back(PollEvent{.fd = static_cast<int>(e.ident),
                                 .readable = e.filter == EVFILT_READ,
                                 .writable = e.filter == EVFILT_WRITE,
                                 .hangup = (e.flags & (EV_EOF | EV_ERROR)) != 0});
    }
    return {};
  }

 private:
  struct Interest {
    bool read = false;
    bool write = false;
  };

  Fd kqueue_fd_;
  std::unordered_map<int, Interest> registered_;
};

}  // namespace

Result<std::unique_ptr<Poller>> Poller::create() {
  Fd fd(::kqueue());
  if (!fd.valid()) return io_error("kqueue", errno);
  return std::unique_ptr<Poller>(std::make_unique<KqueuePoller>(std::move(fd)));
}

}  // namespace baton

#endif  // __APPLE__ || __FreeBSD__
