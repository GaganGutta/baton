// baton-loadgen: a load generator for job queues.
//
//   baton-loadgen --mode enqueue --connections 50 --depth 1 --duration 10
//   baton-loadgen --mode enqueue --connections 8 --rate 20000 --duration 10
//   baton-loadgen --mode e2e --producers 8 --workers 32 --duration 10
//   baton-loadgen --protocol beanstalkd --port 11300 --mode e2e ...
//
// Modes
//   enqueue   producers only. Closed loop by default: every connection keeps
//             --depth requests in flight (1 = wait for each reply, more =
//             pipelining), which finds the maximum throughput. With --rate the
//             generator is open loop: requests are sent on a fixed schedule,
//             --rate per second over all connections, and latency is measured
//             from the moment a request was *due*, not from when it was sent, so
//             a stalled server cannot hide behind a generator that politely
//             waited for it (coordinated omission).
//   e2e       producers as above plus --workers connections that reserve a job,
//             acknowledge it, and repeat. Reports completed jobs per second and
//             the pickup latency: from the moment the producer sent (or was due
//             to send) the enqueue to the moment a worker had the job in hand.
//             The timestamp travels in the payload; both ends are this process.
//
// One thread per connection, blocking sockets. Latencies go into log-linear
// histograms (at most 3% error) per thread, merged at the end. Nothing is
// recorded during --warmup. The result is one JSON object on stdout.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/histogram.h"
#include "loadgen/protocol.h"

namespace baton::bench {
namespace {

struct Options {
  std::string protocol = "baton";
  std::string host = "127.0.0.1";
  uint16_t port = 7379;
  std::string mode = "enqueue";
  std::string queue = "bench";
  int connections = 8;  // enqueue mode
  int producers = 4;    // e2e mode
  int workers = 16;     // e2e mode
  int depth = 1;
  double rate = 0;  // requests per second over all producers; 0 = closed loop
  double duration = 10;
  double warmup = 2;
  size_t payload = 256;
};

struct Stats {
  Histogram enqueue_latency;  // nanoseconds
  Histogram pickup_latency;
  uint64_t enqueued = 0;
  uint64_t completed = 0;
  uint64_t empty_reserves = 0;
  uint64_t errors = 0;
  std::string first_error;

  void fail(std::string what) {
    ++errors;
    if (first_error.empty()) first_error = std::move(what);
  }
};

std::atomic<bool> g_measuring{false};
std::atomic<bool> g_stopping{false};
constexpr size_t kTimestampDigits = 20;

uint64_t now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

class Socket {
 public:
  Socket() = default;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool connect_to(const std::string& host, uint16_t port, std::string& error) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (fd_ < 0 || ::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
        ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      error = std::format("connect to {}:{}: {}", host, port, std::strerror(errno));
      return false;
    }
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return true;
  }

  bool send_all(std::string_view data) {
    while (!data.empty()) {
      const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
  }

  // Appends what arrives within `timeout_ms` (-1: wait). 1 = data, 0 = timeout,
  // -1 = the connection is gone.
  int receive(std::string& into, int timeout_ms) {
    pollfd waiting{.fd = fd_, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&waiting, 1, timeout_ms);
    if (ready == 0) return 0;
    if (ready < 0) return errno == EINTR ? 0 : -1;
    char buffer[65536];
    const ssize_t n = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (n < 0 && errno == EINTR) return 0;
    if (n <= 0) return -1;
    into.append(buffer, static_cast<size_t>(n));
    return 1;
  }

 private:
  int fd_ = -1;
};

// A connection: socket, protocol, input buffer.
class Connection {
 public:
  Connection(const Options& options, uint64_t id)
      : protocol_(make_protocol(options.protocol, options.queue, id)) {}

  bool open(const Options& options, bool worker, std::string& error) {
    if (!socket_.connect_to(options.host, options.port, error)) return false;
    std::string hello;
    const int replies = protocol_->hello(worker, hello);
    if (!hello.empty() && !socket_.send_all(hello)) {
      error = "the handshake could not be sent";
      return false;
    }
    for (int i = 0; i < replies; ++i) {
      Reply reply;
      if (!read_reply(reply, 10'000) || reply.kind == Reply::Kind::kError) {
        error = "handshake failed: " + reply.error;
        return false;
      }
    }
    return true;
  }

  Protocol& protocol() { return *protocol_; }
  bool send(std::string_view data) { return socket_.send_all(data); }

  // Parses the next complete reply that is already buffered.
  bool next_buffered(Reply& reply) {
    const size_t used = protocol_->parse(std::string_view(in_).substr(position_), reply);
    if (used == 0) return false;
    position_ += used;
    return true;
  }

  // 1 = more bytes arrived, 0 = timeout, -1 = closed.
  int fill(int timeout_ms) {
    if (position_ > (1U << 16U)) {
      in_.erase(0, position_);
      position_ = 0;
    }
    return socket_.receive(in_, timeout_ms);
  }

  // Blocks until a reply is complete. The reply's payload is valid until the next call.
  bool read_reply(Reply& reply, int timeout_ms) {
    while (!next_buffered(reply)) {
      if (fill(timeout_ms) <= 0) {
        reply.kind = Reply::Kind::kError;
        reply.error = "connection closed or timed out";
        return false;
      }
    }
    return true;
  }

 private:
  Socket socket_;
  std::unique_ptr<Protocol> protocol_;
  std::string in_;
  size_t position_ = 0;
};

void stamp(std::string& payload, uint64_t ns) {
  const std::string digits = std::format("{:020}", ns);
  payload.replace(0, kTimestampDigits, digits);
}

void produce(const Options& options, int index, int producer_count, Stats& stats) {
  Connection connection(options, static_cast<uint64_t>(index));
  std::string error;
  if (!connection.open(options, /*worker=*/false, error)) return stats.fail(error);

  const auto interval_ns =
      options.rate > 0 ? static_cast<uint64_t>(1e9 * producer_count / options.rate) : 0;
  // Spread the producers over the first interval instead of firing in lockstep.
  uint64_t due = now_ns() + (interval_ns * static_cast<uint64_t>(index)) /
                                static_cast<uint64_t>(producer_count);
  const auto window = static_cast<size_t>(interval_ns > 0 ? 100'000 : options.depth);

  std::string payload(std::max(options.payload, kTimestampDigits), 'x');
  std::string out;
  std::deque<uint64_t> in_flight;  // when each outstanding request was sent (or due)

  uint64_t stopping_since = 0;
  while (!g_stopping.load(std::memory_order_relaxed) || !in_flight.empty()) {
    uint64_t now = now_ns();
    if (g_stopping.load(std::memory_order_relaxed)) {
      if (stopping_since == 0) stopping_since = now;
      if (now - stopping_since > 10'000'000'000ULL) {
        return stats.fail(std::format("{} replies never arrived", in_flight.size()));
      }
    }
    while (!g_stopping.load(std::memory_order_relaxed) && in_flight.size() < window &&
           (interval_ns == 0 || now >= due)) {
      const uint64_t started = interval_ns == 0 ? now : due;
      stamp(payload, started);
      connection.protocol().enqueue(payload, out);
      in_flight.push_back(started);
      due += interval_ns;
    }
    if (!out.empty()) {
      if (!connection.send(out)) return stats.fail("send failed");
      out.clear();
    }

    int timeout_ms = 1'000;
    if (interval_ns > 0 && !g_stopping.load(std::memory_order_relaxed)) {
      now = now_ns();
      timeout_ms = due > now ? static_cast<int>((due - now) / 1'000'000) : 0;
    }
    if (in_flight.empty() && timeout_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
      continue;
    }
    if (connection.fill(timeout_ms) < 0) return stats.fail("connection closed");

    Reply reply;
    while (!in_flight.empty() && connection.next_buffered(reply)) {
      const uint64_t latency = now_ns() - in_flight.front();
      in_flight.pop_front();
      if (reply.kind == Reply::Kind::kError) {
        stats.fail(reply.error);
      } else if (g_measuring.load(std::memory_order_relaxed)) {
        stats.enqueue_latency.record(latency);
        ++stats.enqueued;
      }
    }
  }
}

void work(const Options& options, int index, Stats& stats) {
  Connection connection(options, static_cast<uint64_t>(1'000'000 + index));
  std::string error;
  if (!connection.open(options, /*worker=*/true, error)) return stats.fail(error);

  std::string out;
  uint64_t last_keepalive = now_ns();
  while (!g_stopping.load(std::memory_order_relaxed)) {
    out.clear();
    connection.protocol().reserve(out);
    Reply job;
    if (!connection.send(out) || !connection.read_reply(job, 30'000)) {
      return stats.fail("reserve: " + job.error);
    }
    if (job.kind == Reply::Kind::kJob) {
      const uint64_t picked_up = now_ns();
      uint64_t sent = 0;
      const bool stamped = job.payload.size() >= kTimestampDigits &&
                           detail::to_u64(job.payload.substr(0, kTimestampDigits), sent) &&
                           sent <= picked_up;
      out.clear();
      connection.protocol().ack(job, out);
      Reply acked;
      if (!connection.send(out) || !connection.read_reply(acked, 30'000)) {
        return stats.fail("ack: " + acked.error);
      }
      if (acked.kind == Reply::Kind::kError) {
        stats.fail("ack: " + acked.error);
      } else if (g_measuring.load(std::memory_order_relaxed)) {
        ++stats.completed;
        if (stamped) stats.pickup_latency.record(picked_up - sent);
      }
    } else if (job.kind == Reply::Kind::kEmpty) {
      ++stats.empty_reserves;
    } else {
      stats.fail("reserve: " + job.error);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (now_ns() - last_keepalive > 10'000'000'000ULL) {
      last_keepalive = now_ns();
      out.clear();
      Reply beat;
      if (connection.protocol().keepalive(out) &&
          (!connection.send(out) || !connection.read_reply(beat, 30'000))) {
        return stats.fail("keepalive failed");
      }
    }
  }
}

std::string latency_json(const Histogram& h) {
  const auto us = [](uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
  return std::format(
      R"({{"count": {}, "mean": {:.1f}, "p50": {:.1f}, "p90": {:.1f}, "p99": {:.1f}, )"
      R"("p999": {:.1f}, "max": {:.1f}}})",
      h.count(), h.mean() / 1000.0, us(h.percentile(0.50)), us(h.percentile(0.90)),
      us(h.percentile(0.99)), us(h.percentile(0.999)), us(h.max()));
}

std::string json_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
  }
  return out;
}

bool parse_options(int argc, char** argv, Options& o) {
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--protocol") {
      o.protocol = value;
    } else if (flag == "--host") {
      o.host = value;
    } else if (flag == "--port") {
      o.port = static_cast<uint16_t>(std::stoi(value));
    } else if (flag == "--mode") {
      o.mode = value;
    } else if (flag == "--queue") {
      o.queue = value;
    } else if (flag == "--connections") {
      o.connections = std::stoi(value);
    } else if (flag == "--producers") {
      o.producers = std::stoi(value);
    } else if (flag == "--workers") {
      o.workers = std::stoi(value);
    } else if (flag == "--depth") {
      o.depth = std::stoi(value);
    } else if (flag == "--rate") {
      o.rate = std::stod(value);
    } else if (flag == "--duration") {
      o.duration = std::stod(value);
    } else if (flag == "--warmup") {
      o.warmup = std::stod(value);
    } else if (flag == "--payload") {
      o.payload = static_cast<size_t>(std::stoul(value));
    } else {
      return false;
    }
  }
  const bool known_protocol =
      o.protocol == "baton" || o.protocol == "beanstalkd" || o.protocol == "faktory";
  return argc % 2 == 1 && known_protocol && (o.mode == "enqueue" || o.mode == "e2e") &&
         o.connections > 0 && o.producers > 0 && o.workers > 0 && o.depth > 0 && o.duration > 0;
}

int run(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    std::cerr << "usage: baton-loadgen [--protocol baton|beanstalkd|faktory] [--host H] "
                 "[--port N]\n"
                 "         [--mode enqueue|e2e] [--connections N] [--producers N] [--workers N]\n"
                 "         [--depth N] [--rate PER_SECOND] [--duration S] [--warmup S] "
                 "[--payload BYTES] [--queue NAME]\n";
    return 2;
  }
  const bool e2e = options.mode == "e2e";
  const int producer_count = e2e ? options.producers : options.connections;
  const int worker_count = e2e ? options.workers : 0;

  std::vector<Stats> stats(static_cast<size_t>(producer_count + worker_count));
  std::vector<std::thread> threads;
  for (int i = 0; i < worker_count; ++i) {
    threads.emplace_back(work, std::cref(options), i,
                         std::ref(stats[static_cast<size_t>(producer_count + i)]));
  }
  for (int i = 0; i < producer_count; ++i) {
    threads.emplace_back(produce, std::cref(options), i, producer_count,
                         std::ref(stats[static_cast<size_t>(i)]));
  }

  std::this_thread::sleep_for(std::chrono::duration<double>(options.warmup));
  const uint64_t began = now_ns();
  g_measuring = true;
  std::this_thread::sleep_for(std::chrono::duration<double>(options.duration));
  g_measuring = false;
  const double seconds = static_cast<double>(now_ns() - began) / 1e9;
  g_stopping = true;
  for (std::thread& thread : threads) thread.join();

  Stats total;
  for (const Stats& s : stats) {
    total.enqueue_latency.merge(s.enqueue_latency);
    total.pickup_latency.merge(s.pickup_latency);
    total.enqueued += s.enqueued;
    total.completed += s.completed;
    total.empty_reserves += s.empty_reserves;
    total.errors += s.errors;
    if (total.first_error.empty()) total.first_error = s.first_error;
  }

  std::cout
      << std::format(
             R"({{"protocol": "{}", "mode": "{}", "producers": {}, "workers": {}, "depth": {}, )"
             R"("rate": {}, "payload": {}, "seconds": {:.2f}, "enqueued": {}, "enqueued_per_s": {:.0f}, )"
             R"("enqueue_latency_us": {}, "completed": {}, "completed_per_s": {:.0f}, )"
             R"("pickup_latency_us": {}, "errors": {}, "first_error": "{}"}})",
             options.protocol, options.mode, producer_count, worker_count,
             options.rate > 0 ? 0 : options.depth, options.rate, options.payload, seconds,
             total.enqueued, static_cast<double>(total.enqueued) / seconds,
             latency_json(total.enqueue_latency), total.completed,
             static_cast<double>(total.completed) / seconds, latency_json(total.pickup_latency),
             total.errors, json_escape(total.first_error))
      << "\n";
  return total.errors == 0 ? 0 : 1;
}

}  // namespace
}  // namespace baton::bench

int main(int argc, char** argv) {
  try {
    return baton::bench::run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "baton-loadgen: " << e.what() << "\n";
    return 2;
  }
}
