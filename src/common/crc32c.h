#pragma once

// CRC32C (Castagnoli): the checksum on every log record and snapshot section.
// Chosen over CRC32 for better error detection at the same cost and for
// hardware support on x86-64 (SSE4.2) and ARMv8.

#include <cstdint>
#include <string_view>

namespace baton {

uint32_t crc32c(std::string_view data);

// Continues a checksum over more data:
//   crc32c_extend(crc32c(a), b) == crc32c(a + b)
uint32_t crc32c_extend(uint32_t crc, std::string_view data);

namespace detail {

// Exposed so tests can compare the implementations against each other.
uint32_t crc32c_software(uint32_t crc, std::string_view data);
bool crc32c_hardware_available();
uint32_t crc32c_hardware(uint32_t crc, std::string_view data);  // requires availability

}  // namespace detail
}  // namespace baton
