#include "common/crc32c.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>

#include "common/check.h"

#ifdef __x86_64__
#include <nmmintrin.h>
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace baton {
namespace {

// The 8-byte fast paths load words with memcpy and rely on byte order.
static_assert(std::endian::native == std::endian::little, "baton supports little-endian hosts");

constexpr uint32_t kPolynomialReflected = 0x82F63B78;

using Tables = std::array<std::array<uint32_t, 256>, 8>;

// Slicing-by-8 tables: table[0] is the classic byte-at-a-time table and
// table[k] advances a byte that is k positions further from the end.
constexpr Tables make_tables() {
  Tables t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit)
      c = ((c & 1U) != 0) ? ((c >> 1U) ^ kPolynomialReflected) : (c >> 1U);
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t k = 1; k < t.size(); ++k) {
      t[k][i] = (t[k - 1][i] >> 8U) ^ t[0][t[k - 1][i] & 0xFFU];
    }
  }
  return t;
}

constexpr Tables kTables = make_tables();

#ifdef __x86_64__

__attribute__((target("sse4.2"))) uint32_t hardware_impl(uint32_t crc, const unsigned char* p,
                                                         size_t n) {
  uint64_t c = ~crc;  // zero-extended: the upper 32 bits must start clear
  while (n >= 8) {
    uint64_t word = 0;
    std::memcpy(&word, p, 8);
    c = _mm_crc32_u64(c, word);
    p += 8;
    n -= 8;
  }
  auto c32 = static_cast<uint32_t>(c);
  while (n > 0) {
    c32 = _mm_crc32_u8(c32, *p);
    ++p;
    --n;
  }
  return ~c32;
}

// Returns int on GCC and bool on Clang; the implicit conversion covers both.
bool hardware_available_impl() { return __builtin_cpu_supports("sse4.2"); }

#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)

uint32_t hardware_impl(uint32_t crc, const unsigned char* p, size_t n) {
  uint32_t c = ~crc;
  while (n >= 8) {
    uint64_t word = 0;
    std::memcpy(&word, p, 8);
    c = __crc32cd(c, word);
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    c = __crc32cb(c, *p);
    ++p;
    --n;
  }
  return ~c;
}

bool hardware_available_impl() { return true; }

#else

uint32_t hardware_impl(uint32_t /*crc*/, const unsigned char* /*p*/, size_t /*n*/) {
  BATON_UNREACHABLE("no hardware CRC32C on this platform");
}

bool hardware_available_impl() { return false; }

#endif

using Impl = uint32_t (*)(uint32_t, std::string_view);

Impl select_impl() {
  return detail::crc32c_hardware_available() ? &detail::crc32c_hardware : &detail::crc32c_software;
}

}  // namespace

namespace detail {

uint32_t crc32c_software(uint32_t crc, std::string_view data) {
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  size_t n = data.size();
  uint32_t c = ~crc;
  while (n >= 8) {
    uint64_t word = 0;
    std::memcpy(&word, p, 8);
    word ^= c;
    c = kTables[7][word & 0xFFU] ^ kTables[6][(word >> 8U) & 0xFFU] ^
        kTables[5][(word >> 16U) & 0xFFU] ^ kTables[4][(word >> 24U) & 0xFFU] ^
        kTables[3][(word >> 32U) & 0xFFU] ^ kTables[2][(word >> 40U) & 0xFFU] ^
        kTables[1][(word >> 48U) & 0xFFU] ^ kTables[0][(word >> 56U) & 0xFFU];
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    c = kTables[0][(c ^ *p) & 0xFFU] ^ (c >> 8U);
    ++p;
    --n;
  }
  return ~c;
}

bool crc32c_hardware_available() { return hardware_available_impl(); }

uint32_t crc32c_hardware(uint32_t crc, std::string_view data) {
  BATON_CHECK(crc32c_hardware_available());
  return hardware_impl(crc, reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

}  // namespace detail

uint32_t crc32c_extend(uint32_t crc, std::string_view data) {
  static const Impl impl = select_impl();
  return impl(crc, data);
}

uint32_t crc32c(std::string_view data) { return crc32c_extend(0, data); }

}  // namespace baton
