#include "net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>

namespace baton {
namespace {

Status set_nonblocking_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return io_error("fcntl(O_NONBLOCK)", errno);
  }
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return io_error("fcntl(FD_CLOEXEC)", errno);
  return {};
}

void set_option(int fd, int level, int name) {
  const int on = 1;
  // Best effort: a socket without the option still works.
  (void)::setsockopt(fd, level, name, &on, sizeof(on));
}

}  // namespace

Result<Fd> listen_tcp(const std::string& address, uint16_t port, int backlog) {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = AF_INET;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
  if (::inet_pton(AF_INET, address.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    length = sizeof(sockaddr_in);
  } else if (::inet_pton(AF_INET6, address.c_str(), &v6->sin6_addr) == 1) {
    family = AF_INET6;
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    length = sizeof(sockaddr_in6);
  } else {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("bind address '{}' is not an IPv4 or IPv6 literal", address)};
  }

  Fd fd(::socket(family, SOCK_STREAM, 0));
  if (!fd.valid()) return io_error("socket", errno);
  BATON_RETURN_IF_ERROR(set_nonblocking_cloexec(fd.get()));
  set_option(fd.get(), SOL_SOCKET, SO_REUSEADDR);
  if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&storage), length) != 0) {
    return io_error(std::format("bind {}:{}", address, port), errno);
  }
  if (::listen(fd.get(), backlog) != 0) return io_error("listen", errno);
  return fd;
}

Result<uint16_t> local_port(int fd) {
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
    return io_error("getsockname", errno);
  }
  if (storage.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
  }
  return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
}

Result<std::optional<Fd>> accept_connection(int listen_fd) {
  for (;;) {
    Fd fd(::accept(listen_fd, nullptr, nullptr));
    if (fd.valid()) {
      BATON_RETURN_IF_ERROR(set_nonblocking_cloexec(fd.get()));
      set_option(fd.get(), IPPROTO_TCP, TCP_NODELAY);
#ifdef SO_NOSIGPIPE
      set_option(fd.get(), SOL_SOCKET, SO_NOSIGPIPE);
#endif
      return std::optional<Fd>(std::move(fd));
    }
    if (errno == EINTR) continue;
    // ECONNABORTED: the client gave up while queued; nothing to accept after all.
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
      return std::optional<Fd>();
    }
    return io_error("accept", errno);
  }
}

Result<std::pair<Fd, Fd>> make_wakeup_pipe() {
  std::array<int, 2> fds{};
  if (::pipe(fds.data()) != 0) return io_error("pipe", errno);
  Fd read_end(fds[0]);
  Fd write_end(fds[1]);
  BATON_RETURN_IF_ERROR(set_nonblocking_cloexec(read_end.get()));
  BATON_RETURN_IF_ERROR(set_nonblocking_cloexec(write_end.get()));
  return std::pair<Fd, Fd>(std::move(read_end), std::move(write_end));
}

IoResult read_some(int fd, char* buffer, size_t capacity) {
  for (;;) {
    const ssize_t n = ::read(fd, buffer, capacity);
    if (n > 0) return IoResult{.status = IoStatus::kOk, .bytes = static_cast<size_t>(n)};
    if (n == 0) return IoResult{.status = IoStatus::kClosed};
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return IoResult{.status = IoStatus::kWouldBlock};
    return IoResult{.status = IoStatus::kError, .error = errno};
  }
}

IoResult write_some(int fd, std::string_view data) {
#ifdef MSG_NOSIGNAL
  constexpr int kFlags = MSG_NOSIGNAL;
#else
  constexpr int kFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif
  for (;;) {
    const ssize_t n = ::send(fd, data.data(), data.size(), kFlags);
    if (n >= 0) return IoResult{.status = IoStatus::kOk, .bytes = static_cast<size_t>(n)};
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return IoResult{.status = IoStatus::kWouldBlock};
    return IoResult{.status = IoStatus::kError, .error = errno};
  }
}

}  // namespace baton
