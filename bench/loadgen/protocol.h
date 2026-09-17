#pragma once

// The three wire protocols the load generator speaks, reduced to what a queue
// benchmark needs: say hello, enqueue a payload, reserve a job, acknowledge it.
//
//   baton       RESP2 requests and replies (docs/protocol.md)
//   beanstalkd  its text protocol: put / reserve-with-timeout / delete
//   faktory     inline commands with JSON arguments, RESP replies: PUSH / FETCH / ACK
//
// Replies are parsed incrementally from a byte stream; every reply of all three
// protocols says what it is, so no per-request bookkeeping is needed beyond
// "replies arrive in the order of their requests".

#include <charconv>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace baton::bench {

struct Reply {
  enum class Kind : uint8_t { kOk, kJob, kEmpty, kError };
  Kind kind = Kind::kOk;
  uint64_t id = 0;           // baton and beanstalkd: job id
  uint64_t token = 0;        // baton: lease token
  std::string jid;           // faktory: job id
  std::string_view payload;  // kJob: a view into the parsed buffer
  std::string error;
};

class Protocol {
 public:
  Protocol() = default;
  Protocol(const Protocol&) = delete;
  Protocol& operator=(const Protocol&) = delete;
  virtual ~Protocol() = default;

  // What a new connection sends first, and how many replies that produces
  // (including anything the server says unasked).
  virtual int hello(bool worker, std::string& out) = 0;
  virtual void enqueue(std::string_view payload, std::string& out) = 0;
  virtual void reserve(std::string& out) = 0;  // blocks server-side for about a second
  virtual void ack(const Reply& job, std::string& out) = 0;
  // Workers call this now and then; protocols that need heartbeats send one.
  virtual bool keepalive(std::string& /*out*/) { return false; }

  // Parses the reply at the front of `data`. Returns the bytes it occupies, or
  // 0 if it is not complete yet.
  virtual size_t parse(std::string_view data, Reply& reply) = 0;
};

namespace detail {

inline bool to_u64(std::string_view text, uint64_t& value) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc{} && end == text.data() + text.size();
}

inline bool to_i64(std::string_view text, int64_t& value) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc{} && end == text.data() + text.size();
}

// One RESP value: its type byte, integer or length, and (for bulk strings) body.
struct RespValue {
  char type = 0;
  int64_t number = 0;
  std::string_view text;
};

// Parses a non-array RESP value or an array header at `data[pos...]`. Returns
// the position after it, or 0 if incomplete.
inline size_t parse_resp_value(std::string_view data, size_t pos, RespValue& value) {
  if (pos >= data.size()) return 0;
  const size_t line_end = data.find("\r\n", pos);
  if (line_end == std::string_view::npos) return 0;
  value.type = data[pos];
  const std::string_view line = data.substr(pos + 1, line_end - pos - 1);
  size_t next = line_end + 2;
  switch (value.type) {
    case '+':
    case '-':
      value.text = line;
      return next;
    case ':':
    case '*':
      return to_i64(line, value.number) ? next : std::string_view::npos;
    case '$': {
      if (!to_i64(line, value.number)) return std::string_view::npos;
      if (value.number < 0) return next;
      const auto length = static_cast<size_t>(value.number);
      if (data.size() < next + length + 2) return 0;
      value.text = data.substr(next, length);
      return next + length + 2;
    }
    default:
      return std::string_view::npos;
  }
}

inline void resp_command(std::string& out, std::initializer_list<std::string_view> args) {
  out += std::format("*{}\r\n", args.size());
  for (const std::string_view arg : args) {
    out += std::format("${}\r\n", arg.size());
    out += arg;
    out += "\r\n";
  }
}

inline size_t protocol_error(Reply& reply, std::string_view data) {
  reply.kind = Reply::Kind::kError;
  reply.error = "unparseable reply: " + std::string(data.substr(0, 60));
  return data.size();
}

}  // namespace detail

// --- baton ----------------------------------------------------------------------------------

class BatonProtocol final : public Protocol {
 public:
  explicit BatonProtocol(std::string queue) : queue_(std::move(queue)) {}

  int hello(bool /*worker*/, std::string& /*out*/) override { return 0; }

  void enqueue(std::string_view payload, std::string& out) override {
    detail::resp_command(out, {"ENQUEUE", queue_, payload});
  }
  void reserve(std::string& out) override {
    detail::resp_command(out, {"RESERVE", "1000", "30000", queue_});
  }
  void ack(const Reply& job, std::string& out) override {
    detail::resp_command(out, {"ACK", std::to_string(job.id), std::to_string(job.token)});
  }

  size_t parse(std::string_view data, Reply& reply) override {
    detail::RespValue value;
    size_t pos = detail::parse_resp_value(data, 0, value);
    if (pos == 0) return 0;
    if (pos == std::string_view::npos) return detail::protocol_error(reply, data);
    reply = Reply{};
    switch (value.type) {
      case '+':
        return pos;
      case ':':
        reply.id = static_cast<uint64_t>(value.number);
        return pos;
      case '-':
        reply.kind = Reply::Kind::kError;
        reply.error = std::string(value.text);
        return pos;
      case '*': {
        if (value.number < 0) {
          reply.kind = Reply::Kind::kEmpty;
          return pos;
        }
        // RESERVE: id, token, queue, payload, attempt, max attempts, expiry.
        reply.kind = Reply::Kind::kJob;
        for (int64_t i = 0; i < value.number; ++i) {
          detail::RespValue element;
          pos = detail::parse_resp_value(data, pos, element);
          if (pos == 0) return 0;
          if (pos == std::string_view::npos) return detail::protocol_error(reply, data);
          if (i == 0) reply.id = static_cast<uint64_t>(element.number);
          if (i == 1) reply.token = static_cast<uint64_t>(element.number);
          if (i == 3) reply.payload = element.text;
        }
        return pos;
      }
      default:
        return detail::protocol_error(reply, data);
    }
  }

 private:
  std::string queue_;
};

// --- beanstalkd -----------------------------------------------------------------------------

class BeanstalkdProtocol final : public Protocol {
 public:
  explicit BeanstalkdProtocol(std::string tube) : tube_(std::move(tube)) {}

  int hello(bool worker, std::string& out) override {
    out += std::format("{} {}\r\n", worker ? "watch" : "use", tube_);
    return 1;
  }
  void enqueue(std::string_view payload, std::string& out) override {
    out += std::format("put 0 0 30 {}\r\n", payload.size());  // priority, delay, time-to-run
    out += payload;
    out += "\r\n";
  }
  void reserve(std::string& out) override { out += "reserve-with-timeout 1\r\n"; }
  void ack(const Reply& job, std::string& out) override {
    out += std::format("delete {}\r\n", job.id);
  }

  size_t parse(std::string_view data, Reply& reply) override {
    const size_t line_end = data.find("\r\n");
    if (line_end == std::string_view::npos) return 0;
    const std::string_view line = data.substr(0, line_end);
    const size_t space = line.find(' ');
    const std::string_view word = line.substr(0, space);
    const std::string_view rest = space == std::string_view::npos ? "" : line.substr(space + 1);
    reply = Reply{};
    if (word == "INSERTED") {
      detail::to_u64(rest, reply.id);
      return line_end + 2;
    }
    if (word == "USING" || word == "WATCHING" || word == "DELETED") return line_end + 2;
    if (word == "TIMED_OUT" || word == "DEADLINE_SOON") {
      reply.kind = Reply::Kind::kEmpty;
      return line_end + 2;
    }
    if (word == "RESERVED") {
      const size_t split = rest.find(' ');
      uint64_t bytes = 0;
      if (split == std::string_view::npos || !detail::to_u64(rest.substr(0, split), reply.id) ||
          !detail::to_u64(rest.substr(split + 1), bytes)) {
        return detail::protocol_error(reply, data);
      }
      if (data.size() < line_end + 2 + bytes + 2) return 0;
      reply.kind = Reply::Kind::kJob;
      reply.payload = data.substr(line_end + 2, bytes);
      return line_end + 2 + bytes + 2;
    }
    reply.kind = Reply::Kind::kError;
    reply.error = std::string(line);
    return line_end + 2;
  }

 private:
  std::string tube_;
};

// --- faktory --------------------------------------------------------------------------------

class FaktoryProtocol final : public Protocol {
 public:
  FaktoryProtocol(std::string queue, uint64_t connection_id)
      : queue_(std::move(queue)), wid_(std::format("loadgen{:08x}", connection_id)) {}

  int hello(bool worker, std::string& out) override {
    // The server greets first ("+HI {...}"); HELLO is answered with "+OK".
    if (worker) {
      out += std::format(R"(HELLO {{"hostname":"loadgen","wid":"{}","pid":1,"labels":[],"v":2}})",
                         wid_);
    } else {
      out += R"(HELLO {"v":2})";
    }
    out += "\r\n";
    return 2;
  }
  void enqueue(std::string_view payload, std::string& out) override {
    out += std::format(
        R"(PUSH {{"jid":"{}{:012x}","jobtype":"bench","queue":"{}","args":["{}"],"retry":3}})",
        wid_, ++sequence_, queue_, payload);
    out += "\r\n";
  }
  void reserve(std::string& out) override { out += std::format("FETCH {}\r\n", queue_); }
  void ack(const Reply& job, std::string& out) override {
    out += std::format(R"(ACK {{"jid":"{}"}})", job.jid);
    out += "\r\n";
  }
  bool keepalive(std::string& out) override {
    out += std::format(R"(BEAT {{"wid":"{}"}})", wid_);
    out += "\r\n";
    return true;
  }

  size_t parse(std::string_view data, Reply& reply) override {
    detail::RespValue value;
    const size_t pos = detail::parse_resp_value(data, 0, value);
    if (pos == 0) return 0;
    if (pos == std::string_view::npos) return detail::protocol_error(reply, data);
    reply = Reply{};
    if (value.type == '+') return pos;
    if (value.type == '-') {
      reply.kind = Reply::Kind::kError;
      reply.error = std::string(value.text);
      return pos;
    }
    if (value.type != '$') return detail::protocol_error(reply, data);
    if (value.number < 0) {
      reply.kind = Reply::Kind::kEmpty;
      return pos;
    }
    // A job, as JSON. The generator wrote it, so plain searching is enough.
    reply.kind = Reply::Kind::kJob;
    reply.jid = std::string(between(value.text, R"("jid":")", "\""));
    reply.payload = between(value.text, R"("args":[")", "\"");
    return pos;
  }

 private:
  static std::string_view between(std::string_view text, std::string_view open,
                                  std::string_view close) {
    const size_t start = text.find(open);
    if (start == std::string_view::npos) return {};
    const size_t from = start + open.size();
    const size_t end = text.find(close, from);
    return end == std::string_view::npos ? std::string_view{} : text.substr(from, end - from);
  }

  std::string queue_;
  std::string wid_;
  uint64_t sequence_ = 0;
};

inline std::unique_ptr<Protocol> make_protocol(std::string_view name, const std::string& queue,
                                               uint64_t connection_id) {
  if (name == "baton") return std::make_unique<BatonProtocol>(queue);
  if (name == "beanstalkd") return std::make_unique<BeanstalkdProtocol>(queue);
  if (name == "faktory") return std::make_unique<FaktoryProtocol>(queue, connection_id);
  return nullptr;
}

}  // namespace baton::bench
