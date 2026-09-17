#include "net/resp.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace baton {
namespace {

// Encodes a request the way every Redis client does.
std::string encode(const std::vector<std::string>& args) {
  std::string out;
  resp_array_header(out, args.size());
  for (const std::string& arg : args) resp_bulk(out, arg);
  return out;
}

struct Parsed {
  std::vector<std::string> args;
  bool oversized = false;
  bool operator==(const Parsed&) const = default;
};

struct Drained {
  std::vector<Parsed> requests;
  std::string error;        // set if the stream ended in a protocol error
  size_t max_buffered = 0;  // the most bytes the "connection" ever had to hold
};

// Feeds `stream` to a parser in chunks of `chunk` bytes, the way a connection
// does: append to a buffer, parse as many requests as possible, drop consumed
// bytes.
Drained drain(const std::string& stream, size_t chunk, RespLimits limits = {}) {
  Drained out;
  RespParser parser(limits);
  std::string buffer;
  for (size_t offset = 0; offset < stream.size() && out.error.empty(); offset += chunk) {
    buffer.append(stream, offset, chunk);
    out.max_buffered = std::max(out.max_buffered, buffer.size());
    for (;;) {
      RespRequest request;
      const RespParser::Outcome outcome = parser.parse(buffer, request, out.error);
      if (outcome.status == RespParser::Status::kRequest) {
        Parsed parsed;
        parsed.oversized = request.oversized;
        for (const std::string_view arg : request.args) parsed.args.emplace_back(arg);
        out.requests.push_back(std::move(parsed));
      }
      buffer.erase(0, outcome.consumed);
      if (outcome.status != RespParser::Status::kRequest) break;
    }
  }
  return out;
}

TEST(RespParserTest, ParsesASimpleRequest) {
  const Drained d = drain("*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n", 1024);
  ASSERT_TRUE(d.error.empty()) << d.error;
  ASSERT_EQ(d.requests.size(), 1U);
  EXPECT_EQ(d.requests[0].args, (std::vector<std::string>{"PING", "hello"}));
}

TEST(RespParserTest, ArgumentsAreBinarySafe) {
  const std::string binary("a\r\nb\0c*$", 8);
  const Drained d = drain(encode({"ENQUEUE", "q", binary, ""}), 1024);
  ASSERT_TRUE(d.error.empty()) << d.error;
  ASSERT_EQ(d.requests.size(), 1U);
  EXPECT_EQ(d.requests[0].args[2], binary);
  EXPECT_EQ(d.requests[0].args[3], "");
}

TEST(RespParserTest, PipelinedRequestsComeOutInOrder) {
  std::string stream;
  for (int i = 0; i < 50; ++i) stream += encode({"ENQUEUE", "q", "job-" + std::to_string(i)});
  const Drained d = drain(stream, stream.size());
  ASSERT_EQ(d.requests.size(), 50U);
  for (int i = 0; i < 50; ++i)
    EXPECT_EQ(d.requests[static_cast<size_t>(i)].args[2], "job-" + std::to_string(i));
}

TEST(RespParserTest, AnySplitOfTheStreamGivesTheSameRequests) {
  const std::string stream = encode({"ENQUEUE", "emails", std::string(300, 'x'), "KEY", "k"}) +
                             "*0\r\n" + encode({"PING"}) + "*-1\r\n" +
                             encode({"RESERVE", "0", "30000", "a", "b"});
  const Drained whole = drain(stream, stream.size());
  ASSERT_TRUE(whole.error.empty()) << whole.error;
  ASSERT_EQ(whole.requests.size(), 3U) << "empty and null arrays carry no command";
  for (size_t chunk = 1; chunk <= 64; ++chunk) {
    const Drained split = drain(stream, chunk);
    EXPECT_TRUE(split.error.empty()) << "chunk " << chunk << ": " << split.error;
    EXPECT_EQ(split.requests, whole.requests) << "chunk " << chunk;
  }
}

TEST(RespParserTest, IncompleteRequestConsumesNothing) {
  const std::string full = encode({"ACK", "12", "34"});
  RespParser parser;
  for (size_t size = 0; size < full.size(); ++size) {
    RespRequest request;
    std::string error;
    const auto outcome = parser.parse(std::string_view(full).substr(0, size), request, error);
    ASSERT_EQ(outcome.status, RespParser::Status::kNeedMore) << "prefix of " << size;
    ASSERT_EQ(outcome.consumed, 0U);
  }
}

TEST(RespParserTest, ProtocolErrors) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"PING\r\n", "expected '*'"},                 // inline commands are refused
      {"GET / HTTP/1.1\r\n", "expected '*'"},       // so is a browser
      {"*1\r\n+OK\r\n", "expected '$'"},            // not a bulk string
      {"*1\r\n*1\r\n$1\r\nx\r\n", "expected '$'"},  // nested array
      {"*abc\r\n", "invalid multibulk length"},
      {"*1x\r\n", "invalid multibulk length"},
      {"* 1\r\n", "invalid multibulk length"},
      {"*1\n", "invalid multibulk length"},  // 32 bytes never arrive; see below
      {"*99999\r\n", "too many arguments"},
      {"*1\r\n$-1\r\n", "invalid bulk length"},  // null bulk in a request
      {"*1\r\n$x\r\n", "invalid bulk length"},
      {"*1\r\n$99999999999\r\n", "invalid bulk length"},  // beyond the hard limit
      {"*1\r\n$3\r\nabcde", "bulk string not terminated"},
      {"*1\r\n$3\r\nabc\rX", "bulk string not terminated"},
      {"*1\r\n$+3\r\nabc\r\n", "invalid bulk length"},
      {"*1\r\n$ 3\r\nabc\r\n", "invalid bulk length"},
  };
  for (const auto& [stream, expected] : cases) {
    const Drained d = drain(stream + std::string(40, ' '), 1024);  // padding defeats kNeedMore
    EXPECT_NE(d.error.find(expected), std::string::npos)
        << "input: " << stream << "\n  error: " << d.error;
    EXPECT_TRUE(d.requests.empty());
  }
}

TEST(RespParserTest, HeaderLineThatNeverEndsIsAnErrorNotAMemoryLeak) {
  const Drained d = drain("*" + std::string(100, '1'), 7);
  EXPECT_NE(d.error.find("invalid multibulk length"), std::string::npos) << d.error;
  EXPECT_LT(d.max_buffered, 64U);
}

TEST(RespParserTest, ExactlyAtTheLimitsIsAccepted) {
  RespLimits limits;
  limits.max_args = 4;
  limits.max_bulk_bytes = 10;
  const Drained ok = drain(encode({"A", std::string(10, 'x'), "c", "d"}), 5, limits);
  ASSERT_TRUE(ok.error.empty()) << ok.error;
  ASSERT_EQ(ok.requests.size(), 1U);
  EXPECT_FALSE(ok.requests[0].oversized);

  EXPECT_FALSE(drain(encode({"A", "b", "c", "d", "e"}), 5, limits).error.empty());
}

// The property that makes --max-payload safe: an oversized argument is never
// buffered, the request is reported as oversized, and the stream stays in sync.
TEST(RespParserTest, OversizedArgumentIsSkippedWithoutBuffering) {
  RespLimits limits;
  limits.max_bulk_bytes = 1000;
  const std::string huge(200'000, 'H');
  const std::string stream = encode({"PING"}) + encode({"ENQUEUE", "q", huge, "KEY", "k1"}) +
                             encode({"ECHO", "still in sync"});

  for (const size_t chunk : {size_t{1}, size_t{13}, size_t{4096}, stream.size()}) {
    const Drained d = drain(stream, chunk, limits);
    ASSERT_TRUE(d.error.empty()) << d.error;
    ASSERT_EQ(d.requests.size(), 3U) << "chunk " << chunk;
    EXPECT_EQ(d.requests[0].args, (std::vector<std::string>{"PING"}));
    EXPECT_TRUE(d.requests[1].oversized);
    EXPECT_TRUE(d.requests[1].args.empty());
    EXPECT_EQ(d.requests[2].args, (std::vector<std::string>{"ECHO", "still in sync"}));
    if (chunk <= 4096) {
      EXPECT_LT(d.max_buffered, 2 * 4096U) << "the 200 KB argument must never be held in memory";
    }
  }
}

TEST(RespParserTest, OversizedAsTheLastArgumentAndBackToBack) {
  RespLimits limits;
  limits.max_bulk_bytes = 8;
  const std::string stream =
      encode({"A", std::string(50, 'x')}) + encode({std::string(9, 'y'), "b"}) + encode({"ok"});
  const Drained d = drain(stream, 3, limits);
  ASSERT_TRUE(d.error.empty()) << d.error;
  ASSERT_EQ(d.requests.size(), 3U);
  EXPECT_TRUE(d.requests[0].oversized);
  EXPECT_TRUE(d.requests[1].oversized);
  EXPECT_EQ(d.requests[2].args, (std::vector<std::string>{"ok"}));
}

TEST(RespParserTest, ProtocolErrorWhileSkippingIsStillAnError) {
  RespLimits limits;
  limits.max_bulk_bytes = 8;
  const Drained d =
      drain("*3\r\n$1\r\nA\r\n$20\r\n" + std::string(20, 'x') + "\r\n:1\r\n" + std::string(40, ' '),
            5, limits);
  EXPECT_NE(d.error.find("expected '$'"), std::string::npos) << d.error;
}

TEST(RespWriterTest, ProducesValidResp) {
  std::string out;
  resp_simple(out, "OK");
  resp_error(out, "STALE", "token 5 is not\r\nthe current lease");
  resp_integer(out, -42);
  resp_bulk(out, std::string_view("a\0b", 3));
  resp_bulk(out, "");
  resp_array_header(out, 2);
  std::string expected = "+OK\r\n";
  expected += "-STALE token 5 is not  the current lease\r\n";  // CR and LF were neutralized
  expected += ":-42\r\n";
  expected += "$3\r\n" + std::string("a\0b", 3) + "\r\n";
  expected += "$0\r\n\r\n";
  expected += "*2\r\n";
  EXPECT_EQ(out, expected);
}

TEST(RespWriterTest, NullAndMapsDependOnTheProtocolVersion) {
  std::string v2;
  resp_null(v2, /*resp3=*/false);
  resp_map_header(v2, 3, /*resp3=*/false);
  EXPECT_EQ(v2, "*-1\r\n*6\r\n") << "RESP2: null array, and a flat array of 2n";

  std::string v3;
  resp_null(v3, /*resp3=*/true);
  resp_map_header(v3, 3, /*resp3=*/true);
  EXPECT_EQ(v3, "_\r\n%3\r\n") << "RESP3: null, and a map of n";
}

}  // namespace
}  // namespace baton
