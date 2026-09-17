#include "net/poller.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <string>

#include "net/socket.h"

namespace baton {
namespace {

// A plain blocking client socket connected to 127.0.0.1:port.
Fd connect_to(uint16_t port) {
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  EXPECT_EQ(::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  return fd;
}

std::vector<PollEvent> wait(Poller& poller, int timeout_ms) {
  std::vector<PollEvent> events;
  EXPECT_TRUE(poller.wait(timeout_ms, events).ok());
  return events;
}

TEST(PollerTest, TimesOutWithNoEvents) {
  auto poller = Poller::create();
  ASSERT_TRUE(poller.ok()) << poller.error().to_string();
  EXPECT_TRUE(wait(**poller, 10).empty());
}

TEST(PollerTest, WakeupPipeMakesTheLoopRunnable) {
  auto poller = Poller::create();
  auto pipe = make_wakeup_pipe();
  ASSERT_TRUE(poller.ok() && pipe.ok());
  auto& [read_end, write_end] = *pipe;
  ASSERT_TRUE((*poller)->add(read_end.get(), true, false).ok());
  EXPECT_TRUE(wait(**poller, 0).empty());

  ASSERT_EQ(::write(write_end.get(), "x", 1), 1);
  const auto events = wait(**poller, 1000);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].fd, read_end.get());
  EXPECT_TRUE(events[0].readable);

  // Level-triggered: still readable until drained.
  EXPECT_EQ(wait(**poller, 0).size(), 1U);
  std::array<char, 8> buffer{};
  EXPECT_EQ(read_some(read_end.get(), buffer.data(), buffer.size()).bytes, 1U);
  EXPECT_EQ(read_some(read_end.get(), buffer.data(), buffer.size()).status, IoStatus::kWouldBlock);
  EXPECT_TRUE(wait(**poller, 0).empty());
}

TEST(PollerTest, AcceptReadWriteOverLoopback) {
  auto poller = Poller::create();
  auto listener = listen_tcp("127.0.0.1", 0);
  ASSERT_TRUE(poller.ok() && listener.ok()) << listener.error().to_string();
  const uint16_t port = local_port(listener->get()).value();
  ASSERT_NE(port, 0);
  ASSERT_TRUE((*poller)->add(listener->get(), true, false).ok());
  EXPECT_FALSE(accept_connection(listener->get()).value().has_value()) << "nobody is connecting";

  const Fd client = connect_to(port);
  ASSERT_EQ(wait(**poller, 1000).size(), 1U);
  auto accepted = accept_connection(listener->get());
  ASSERT_TRUE(accepted.ok() && accepted->has_value());
  const Fd server_side = std::move(**accepted);

  // Interest in writability only fires when asked for.
  ASSERT_TRUE((*poller)->add(server_side.get(), true, false).ok());
  EXPECT_TRUE(wait(**poller, 0).empty());
  ASSERT_TRUE((*poller)->modify(server_side.get(), true, true).ok());
  auto events = wait(**poller, 1000);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(events[0].writable);
  EXPECT_FALSE(events[0].readable);
  ASSERT_TRUE((*poller)->modify(server_side.get(), true, false).ok());

  ASSERT_EQ(::write(client.get(), "ping", 4), 4);
  events = wait(**poller, 1000);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(events[0].readable);
  std::array<char, 16> buffer{};
  const IoResult got = read_some(server_side.get(), buffer.data(), buffer.size());
  EXPECT_EQ(std::string(buffer.data(), got.bytes), "ping");

  EXPECT_EQ(write_some(server_side.get(), "pong").bytes, 4U);
  EXPECT_EQ(::read(client.get(), buffer.data(), buffer.size()), 4);

  (*poller)->remove(server_side.get());
  ASSERT_EQ(::write(client.get(), "more", 4), 4);
  EXPECT_TRUE(wait(**poller, 20).empty()) << "a removed descriptor reports nothing";
}

TEST(PollerTest, PeerCloseIsReportedAndReadSeesEof) {
  auto poller = Poller::create();
  auto listener = listen_tcp("127.0.0.1", 0);
  ASSERT_TRUE(poller.ok() && listener.ok());
  ASSERT_TRUE((*poller)->add(listener->get(), true, false).ok());
  Fd client = connect_to(local_port(listener->get()).value());
  ASSERT_EQ(wait(**poller, 1000).size(), 1U);
  const Fd server_side = std::move(*accept_connection(listener->get()).value());
  ASSERT_TRUE((*poller)->add(server_side.get(), true, false).ok());

  client.reset();  // the client goes away
  const auto events = wait(**poller, 1000);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(events[0].readable || events[0].hangup);
  std::array<char, 8> buffer{};
  EXPECT_EQ(read_some(server_side.get(), buffer.data(), buffer.size()).status, IoStatus::kClosed);
  // Writing to a closed peer must not raise SIGPIPE and kill the process.
  (void)write_some(server_side.get(), "late");
  (void)write_some(server_side.get(), "later");
}

TEST(SocketTest, ListenRejectsHostNames) {
  const auto listener = listen_tcp("localhost", 0);
  ASSERT_FALSE(listener.ok());
  EXPECT_EQ(listener.error().code(), ErrorCode::kInvalidArgument);
}

TEST(SocketTest, ListensOnIpv6Loopback) {
  const auto listener = listen_tcp("::1", 0);
  if (!listener.ok()) GTEST_SKIP() << "no IPv6 loopback here: " << listener.error().to_string();
  EXPECT_NE(local_port(listener->get()).value(), 0);
}

}  // namespace
}  // namespace baton
