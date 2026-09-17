#ifdef __linux__

#include <sys/epoll.h>

#include <array>
#include <cerrno>

#include "common/fd.h"
#include "net/poller.h"

namespace baton {
namespace {

uint32_t interest(bool want_read, bool want_write) {
  uint32_t events = 0;
  if (want_read) events |= EPOLLIN;
  if (want_write) events |= EPOLLOUT;
  return events;
}

class EpollPoller final : public Poller {
 public:
  explicit EpollPoller(Fd epoll_fd) : epoll_fd_(std::move(epoll_fd)) {}

  Status add(int fd, bool want_read, bool want_write) override {
    return control(EPOLL_CTL_ADD, fd, want_read, want_write);
  }

  Status modify(int fd, bool want_read, bool want_write) override {
    return control(EPOLL_CTL_MOD, fd, want_read, want_write);
  }

  void remove(int fd) override { ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr); }

  Status wait(int timeout_ms, std::vector<PollEvent>& events) override {
    std::array<epoll_event, 256> ready{};
    const int n =
        ::epoll_wait(epoll_fd_.get(), ready.data(), static_cast<int>(ready.size()), timeout_ms);
    if (n < 0) {
      if (errno == EINTR) return {};
      return io_error("epoll_wait", errno);
    }
    for (int i = 0; i < n; ++i) {
      const epoll_event& e = ready[static_cast<size_t>(i)];
      events.push_back(PollEvent{.fd = e.data.fd,
                                 .readable = (e.events & EPOLLIN) != 0,
                                 .writable = (e.events & EPOLLOUT) != 0,
                                 .hangup = (e.events & (EPOLLERR | EPOLLHUP)) != 0});
    }
    return {};
  }

 private:
  Status control(int op, int fd, bool want_read, bool want_write) {
    epoll_event event{};
    event.events = interest(want_read, want_write);
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), op, fd, &event) != 0) return io_error("epoll_ctl", errno);
    return {};
  }

  Fd epoll_fd_;
};

}  // namespace

Result<std::unique_ptr<Poller>> Poller::create() {
  Fd fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!fd.valid()) return io_error("epoll_create1", errno);
  return std::unique_ptr<Poller>(std::make_unique<EpollPoller>(std::move(fd)));
}

}  // namespace baton

#endif  // __linux__
