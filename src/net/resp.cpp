#include "net/resp.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace baton {
namespace {

// "*<n>\r\n" and "$<n>\r\n" lines are short; a longer one is not a length.
constexpr size_t kMaxHeaderLine = 32;

enum class LineStatus : uint8_t { kOk, kNeedMore, kBad };

struct HeaderLine {
  LineStatus status = LineStatus::kNeedMore;
  int64_t value = 0;
  size_t end = 0;  // offset just past the CRLF
};

// Reads "<marker><integer>\r\n" at data[pos]. The marker byte has already been
// checked by the caller.
HeaderLine read_header_line(std::string_view data, size_t pos) {
  HeaderLine line;
  const size_t limit = std::min(data.size(), pos + kMaxHeaderLine);
  const size_t cr = data.substr(0, limit).find('\r', pos + 1);
  if (cr == std::string_view::npos) {
    line.status = data.size() - pos >= kMaxHeaderLine ? LineStatus::kBad : LineStatus::kNeedMore;
    return line;
  }
  if (cr + 1 >= data.size()) return line;  // the LF has not arrived yet
  const char* first = data.data() + pos + 1;
  const char* last = data.data() + cr;
  const auto [parsed_to, ec] = std::from_chars(first, last, line.value);
  if (data[cr + 1] != '\n' || first == last || ec != std::errc{} || parsed_to != last) {
    line.status = LineStatus::kBad;
    return line;
  }
  line.status = LineStatus::kOk;
  line.end = cr + 2;
  return line;
}

RespParser::Outcome fail(std::string& error, std::string message) {
  error = std::move(message);
  return RespParser::Outcome{.status = RespParser::Status::kError, .consumed = 0};
}

}  // namespace

RespParser::Outcome RespParser::parse(std::string_view data, RespRequest& request,
                                      std::string& error) {
  request.args.clear();
  request.oversized = false;
  if (discarding_) return parse_discarding(data, 0, request, error);

  // Empty and null arrays carry no command; skip them like Redis does. `start`
  // ends up at the '*' of the first real request: if that request is incomplete,
  // everything before `start` is consumed and the caller retries from there.
  size_t pos = 0;
  size_t start = 0;
  int64_t count = 0;
  for (;;) {
    start = pos;
    if (pos >= data.size()) return Outcome{.status = Status::kNeedMore, .consumed = start};
    if (data[pos] != '*') {
      return fail(error, std::format("expected '*', got byte 0x{:02x}",
                                     static_cast<unsigned char>(data[pos])));
    }
    const HeaderLine header = read_header_line(data, pos);
    if (header.status == LineStatus::kNeedMore) {
      return Outcome{.status = Status::kNeedMore, .consumed = start};
    }
    if (header.status == LineStatus::kBad) return fail(error, "invalid multibulk length");
    pos = header.end;
    count = header.value;
    if (count > 0) break;
  }
  if (std::cmp_greater(count, limits_.max_args)) {
    return fail(error, std::format("too many arguments ({} > {})", count, limits_.max_args));
  }

  request.args.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    if (pos >= data.size()) return Outcome{.status = Status::kNeedMore, .consumed = start};
    if (data[pos] != '$') {
      return fail(error, std::format("expected '$', got byte 0x{:02x}",
                                     static_cast<unsigned char>(data[pos])));
    }
    const HeaderLine header = read_header_line(data, pos);
    if (header.status == LineStatus::kNeedMore) {
      return Outcome{.status = Status::kNeedMore, .consumed = start};
    }
    if (header.status == LineStatus::kBad || header.value < 0 ||
        std::cmp_greater(header.value, limits_.hard_max_bulk_bytes)) {
      return fail(error, "invalid bulk length");
    }
    const auto length = static_cast<uint64_t>(header.value);

    if (length > limits_.max_bulk_bytes) {
      // Too big to buffer: skip it, and the rest of this request, as it arrives.
      request.args.clear();
      discarding_ = true;
      discard_bytes_left_ = length + 2;
      discard_args_left_ = static_cast<size_t>(count - i - 1);
      return parse_discarding(data, header.end, request, error);
    }

    const size_t end = header.end + static_cast<size_t>(length);
    if (end + 2 - start > limits_.max_request_bytes) {
      return fail(error, std::format("request larger than {} bytes", limits_.max_request_bytes));
    }
    if (data.size() < end + 2) return Outcome{.status = Status::kNeedMore, .consumed = start};
    if (data[end] != '\r' || data[end + 1] != '\n') {
      return fail(error, "bulk string not terminated");
    }
    request.args.push_back(data.substr(header.end, static_cast<size_t>(length)));
    pos = end + 2;
  }
  return Outcome{.status = Status::kRequest, .consumed = pos};
}

RespParser::Outcome RespParser::parse_discarding(std::string_view data, size_t pos,
                                                 RespRequest& request, std::string& error) {
  for (;;) {
    if (discard_bytes_left_ > 0) {
      const uint64_t take = std::min<uint64_t>(discard_bytes_left_, data.size() - pos);
      pos += static_cast<size_t>(take);
      discard_bytes_left_ -= take;
      if (discard_bytes_left_ > 0) return Outcome{.status = Status::kNeedMore, .consumed = pos};
    }
    if (discard_args_left_ == 0) {
      discarding_ = false;
      request.oversized = true;
      return Outcome{.status = Status::kRequest, .consumed = pos};
    }

    if (pos >= data.size()) return Outcome{.status = Status::kNeedMore, .consumed = pos};
    if (data[pos] != '$') {
      Outcome outcome = fail(error, std::format("expected '$', got byte 0x{:02x}",
                                                static_cast<unsigned char>(data[pos])));
      outcome.consumed = pos;
      return outcome;
    }
    const HeaderLine header = read_header_line(data, pos);
    if (header.status == LineStatus::kNeedMore) {
      return Outcome{.status = Status::kNeedMore, .consumed = pos};
    }
    if (header.status == LineStatus::kBad || header.value < 0 ||
        std::cmp_greater(header.value, limits_.hard_max_bulk_bytes)) {
      Outcome outcome = fail(error, "invalid bulk length");
      outcome.consumed = pos;
      return outcome;
    }
    pos = header.end;
    discard_bytes_left_ = static_cast<uint64_t>(header.value) + 2;
    --discard_args_left_;
  }
}

// --- replies ------------------------------------------------------------------------

void resp_simple(std::string& out, std::string_view text) {
  out.push_back('+');
  out.append(text);
  out.append("\r\n");
}

void resp_error(std::string& out, std::string_view code, std::string_view message) {
  out.push_back('-');
  out.append(code);
  out.push_back(' ');
  // An error is a single line: a CR or LF inside the message would end it early
  // and desynchronize the client.
  for (const char c : message) out.push_back(c == '\r' || c == '\n' ? ' ' : c);
  out.append("\r\n");
}

void resp_integer(std::string& out, int64_t value) {
  out.push_back(':');
  out.append(std::to_string(value));
  out.append("\r\n");
}

void resp_bulk(std::string& out, std::string_view data) {
  out.push_back('$');
  out.append(std::to_string(data.size()));
  out.append("\r\n");
  out.append(data);
  out.append("\r\n");
}

void resp_null_array(std::string& out) { out.append("*-1\r\n"); }

void resp_array_header(std::string& out, size_t count) {
  out.push_back('*');
  out.append(std::to_string(count));
  out.append("\r\n");
}

}  // namespace baton
