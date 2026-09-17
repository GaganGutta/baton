#include "common/crc32c.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace baton {
namespace {

// Test vectors from RFC 3720 (iSCSI), appendix B.4.
TEST(Crc32cTest, Rfc3720Vectors) {
  EXPECT_EQ(crc32c(std::string(32, '\0')), 0x8A9136AAU);
  EXPECT_EQ(crc32c(std::string(32, '\xFF')), 0x62A8AB43U);

  std::string ascending(32, '\0');
  for (int i = 0; i < 32; ++i) ascending[static_cast<size_t>(i)] = static_cast<char>(i);
  EXPECT_EQ(crc32c(ascending), 0x46DD794EU);

  std::string descending(32, '\0');
  for (int i = 0; i < 32; ++i) descending[static_cast<size_t>(i)] = static_cast<char>(31 - i);
  EXPECT_EQ(crc32c(descending), 0x113FDB5CU);
}

TEST(Crc32cTest, StandardCheckValue) { EXPECT_EQ(crc32c("123456789"), 0xE3069283U); }

TEST(Crc32cTest, EmptyInput) {
  EXPECT_EQ(crc32c(""), 0U);
  EXPECT_EQ(crc32c_extend(0x1234U, ""), 0x1234U);
}

TEST(Crc32cTest, ExtendMatchesOneShotAtEverySplit) {
  const std::string data = "The quick brown fox jumps over the lazy dog, twice over.";
  const uint32_t whole = crc32c(data);
  for (size_t split = 0; split <= data.size(); ++split) {
    const std::string_view view = data;
    EXPECT_EQ(crc32c_extend(crc32c(view.substr(0, split)), view.substr(split)), whole)
        << "split at " << split;
  }
}

TEST(Crc32cTest, SoftwareMatchesVectors) {
  EXPECT_EQ(detail::crc32c_software(0, "123456789"), 0xE3069283U);
  EXPECT_EQ(detail::crc32c_software(0, std::string(32, '\0')), 0x8A9136AAU);
}

TEST(Crc32cTest, HardwareAndSoftwareAgreeOnRandomData) {
  if (!detail::crc32c_hardware_available()) GTEST_SKIP() << "no hardware CRC32C on this CPU";
  std::mt19937_64 rng(42);
  for (int round = 0; round < 500; ++round) {
    std::string data(rng() % 300, '\0');  // covers every tail length around the 8-byte loop
    for (char& c : data) c = static_cast<char>(rng());
    const auto seed = static_cast<uint32_t>(rng());
    ASSERT_EQ(detail::crc32c_hardware(seed, data), detail::crc32c_software(seed, data))
        << "round " << round << " size " << data.size();
  }
}

TEST(Crc32cTest, DetectsEverySingleBitFlip) {
  std::string data = "baton durable log record";
  const uint32_t original = crc32c(data);
  for (size_t byte = 0; byte < data.size(); ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      data[byte] = static_cast<char>(static_cast<unsigned char>(data[byte]) ^ (1U << bit));
      EXPECT_NE(crc32c(data), original);
      data[byte] = static_cast<char>(static_cast<unsigned char>(data[byte]) ^ (1U << bit));
    }
  }
}

}  // namespace
}  // namespace baton
