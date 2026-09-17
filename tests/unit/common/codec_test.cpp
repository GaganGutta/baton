#include "common/codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace baton {
namespace {

TEST(CodecTest, FixedWidthIsLittleEndian) {
  std::string out;
  ByteWriter w(out);
  w.u32(0x04030201U);
  w.u64(0x0807060504030201ULL);
  EXPECT_EQ(out, std::string("\x01\x02\x03\x04\x01\x02\x03\x04\x05\x06\x07\x08", 12));

  ByteReader r(out);
  EXPECT_EQ(r.u32(), 0x04030201U);
  EXPECT_EQ(r.u64(), 0x0807060504030201ULL);
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.at_end());
}

TEST(CodecTest, VarintEncodingSizes) {
  const auto encoded_size = [](uint64_t v) {
    std::string out;
    ByteWriter(out).varint(v);
    return out.size();
  };
  EXPECT_EQ(encoded_size(0), 1U);
  EXPECT_EQ(encoded_size(127), 1U);
  EXPECT_EQ(encoded_size(128), 2U);
  EXPECT_EQ(encoded_size(16383), 2U);
  EXPECT_EQ(encoded_size(16384), 3U);
  EXPECT_EQ(encoded_size(std::numeric_limits<uint64_t>::max()), 10U);
}

TEST(CodecTest, VarintRoundTripsBoundaryValues) {
  std::vector<uint64_t> values = {0, 1, 127, 128, 255, 256, 16383, 16384};
  for (unsigned shift = 7; shift < 64; shift += 7) {
    values.push_back((uint64_t{1} << shift) - 1);
    values.push_back(uint64_t{1} << shift);
  }
  values.push_back(std::numeric_limits<uint64_t>::max());

  std::string out;
  ByteWriter w(out);
  for (const uint64_t v : values) w.varint(v);
  ByteReader r(out);
  for (const uint64_t v : values) EXPECT_EQ(r.varint(), v);
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.at_end());
}

TEST(CodecTest, SignedVarintRoundTripsAndKeepsSmallNegativesSmall) {
  const std::vector<int64_t> values = {0,
                                       -1,
                                       1,
                                       -64,
                                       63,
                                       -65,
                                       64,
                                       std::numeric_limits<int64_t>::min(),
                                       std::numeric_limits<int64_t>::max()};
  std::string out;
  ByteWriter w(out);
  for (const int64_t v : values) w.svarint(v);
  ByteReader r(out);
  for (const int64_t v : values) EXPECT_EQ(r.svarint(), v);
  EXPECT_TRUE(r.ok());

  std::string small;
  ByteWriter(small).svarint(-1);
  EXPECT_EQ(small.size(), 1U);
}

TEST(CodecTest, BytesRoundTripIncludingEmbeddedZeros) {
  const std::string binary("a\0b\xFF", 4);
  std::string out;
  ByteWriter w(out);
  w.bytes(binary);
  w.bytes("");
  ByteReader r(out);
  EXPECT_EQ(r.bytes(), binary);
  EXPECT_EQ(r.bytes(), "");
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(r.at_end());
}

TEST(CodecTest, ReadPastEndFailsStickyAndReturnsZeros) {
  ByteReader r(std::string_view("\x01\x02", 2));
  EXPECT_EQ(r.u32(), 0U);
  EXPECT_FALSE(r.ok());
  // Once failed, everything fails, even reads that would have fit.
  EXPECT_EQ(r.u8(), 0U);
  EXPECT_EQ(r.varint(), 0U);
  EXPECT_TRUE(r.bytes().empty());
  EXPECT_FALSE(r.ok());
}

TEST(CodecTest, BytesWithLengthBeyondInputFailsWithoutAllocating) {
  std::string out;
  ByteWriter w(out);
  w.varint(uint64_t{1} << 60U);  // claims an exabyte
  out += "tiny";
  ByteReader r(out);
  EXPECT_TRUE(r.bytes().empty());
  EXPECT_FALSE(r.ok());
}

TEST(CodecTest, OverlongVarintIsRejected) {
  // 11 continuation bytes: longer than any valid uint64 varint.
  ByteReader too_long(std::string_view("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x01", 11));
  too_long.varint();
  EXPECT_FALSE(too_long.ok());

  // 10 bytes whose last byte carries bits that do not fit in 64.
  ByteReader overflow(std::string_view("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x02", 10));
  overflow.varint();
  EXPECT_FALSE(overflow.ok());
}

TEST(CodecTest, TruncatedVarintFails) {
  ByteReader r(std::string_view("\x80\x80", 2));
  r.varint();
  EXPECT_FALSE(r.ok());
}

TEST(CodecTest, RandomizedRoundTrip) {
  std::mt19937_64 rng(7);
  for (int round = 0; round < 200; ++round) {
    const uint64_t a = rng() >> (rng() % 64);
    const auto b = static_cast<int64_t>(rng()) >> (rng() % 64);
    std::string blob(rng() % 50, '\0');
    for (char& c : blob) c = static_cast<char>(rng());

    std::string out;
    ByteWriter w(out);
    w.varint(a);
    w.svarint(b);
    w.bytes(blob);
    w.u8(0xAB);

    ByteReader r(out);
    EXPECT_EQ(r.varint(), a);
    EXPECT_EQ(r.svarint(), b);
    EXPECT_EQ(r.bytes(), blob);
    EXPECT_EQ(r.u8(), 0xAB);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.at_end());
  }
}

}  // namespace
}  // namespace baton
