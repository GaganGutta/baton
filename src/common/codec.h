#pragma once

// Binary encoding helpers shared by the log, snapshots and record payloads.
//
// Byte strings are std::string / std::string_view throughout baton (payloads
// arrive from the network as bytes and std::string is the cheapest owning
// container for them).
//
// ByteReader never reads out of bounds and never allocates based on a length
// it has not validated, so it is safe on untrusted input. It uses a sticky
// failure flag: decode a whole structure, then check ok() once. After a
// failure every read returns a zero value.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace baton {

class ByteWriter {
 public:
  explicit ByteWriter(std::string& out) : out_(out) {}

  void u8(uint8_t v) { out_.push_back(static_cast<char>(v)); }

  // Fixed-width little-endian.
  void u32(uint32_t v) { fixed(v); }
  void u64(uint64_t v) { fixed(v); }

  // LEB128: 7 bits per byte, least significant group first.
  void varint(uint64_t v) {
    while (v >= 0x80) {
      out_.push_back(static_cast<char>((v & 0x7FU) | 0x80U));
      v >>= 7U;
    }
    out_.push_back(static_cast<char>(v));
  }

  // Zigzag then varint, so small negative numbers stay small.
  void svarint(int64_t v) {
    varint((static_cast<uint64_t>(v) << 1U) ^ static_cast<uint64_t>(v >> 63));
  }

  // Varint length, then the bytes.
  void bytes(std::string_view v) {
    varint(v.size());
    out_.append(v);
  }

  void raw(std::string_view v) { out_.append(v); }

 private:
  template <class T>
  void fixed(T v) {
    std::array<char, sizeof(T)> buf{};
    std::memcpy(buf.data(), &v, sizeof(T));  // little-endian hosts only; see crc32c.cpp
    out_.append(buf.data(), buf.size());
  }

  std::string& out_;
};

class ByteReader {
 public:
  explicit ByteReader(std::string_view in) : in_(in) {}

  bool ok() const { return ok_; }
  bool at_end() const { return in_.empty(); }
  size_t remaining() const { return in_.size(); }

  uint8_t u8() {
    if (!need(1)) return 0;
    const auto v = static_cast<uint8_t>(in_.front());
    in_.remove_prefix(1);
    return v;
  }

  uint32_t u32() { return fixed<uint32_t>(); }
  uint64_t u64() { return fixed<uint64_t>(); }

  uint64_t varint() {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (!need(1)) return 0;
      const auto byte = static_cast<uint8_t>(in_.front());
      in_.remove_prefix(1);
      // The 10th byte may only contribute the single remaining bit.
      if (shift == 63 && byte > 1) return fail();
      result |= static_cast<uint64_t>(byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0) return result;
    }
    return fail();  // more than 10 bytes
  }

  int64_t svarint() {
    const uint64_t z = varint();
    return static_cast<int64_t>((z >> 1U) ^ (~(z & 1U) + 1));
  }

  // A length-prefixed byte string. The view points into the input.
  std::string_view bytes() {
    const uint64_t n = varint();
    if (!ok_ || n > in_.size()) {
      fail();
      return {};
    }
    return raw(static_cast<size_t>(n));
  }

  std::string_view raw(size_t n) {
    if (!need(n)) return {};
    const std::string_view v = in_.substr(0, n);
    in_.remove_prefix(n);
    return v;
  }

 private:
  bool need(size_t n) {
    if (ok_ && in_.size() >= n) return true;
    fail();
    return false;
  }

  uint64_t fail() {
    ok_ = false;
    in_ = {};
    return 0;
  }

  template <class T>
  T fixed() {
    if (!need(sizeof(T))) return 0;
    T v{};
    // The size was checked by need(); this is a fixed-width load, not a C string.
    std::memcpy(&v, in_.data(), sizeof(T));  // NOLINT(bugprone-suspicious-stringview-data-usage)
    in_.remove_prefix(sizeof(T));
    return v;
  }

  std::string_view in_;
  bool ok_ = true;
};

}  // namespace baton
