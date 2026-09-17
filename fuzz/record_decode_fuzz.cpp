// Fuzz target: the record payload decoder.
//
// Input: one type byte, then the payload. Properties:
//   - decoding never crashes, over-reads or allocates absurdly;
//   - whatever decodes re-encodes to bytes that decode to the same record
//     (so a record can never change meaning by passing through the log twice).

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/check.h"
#include "state/records.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const uint8_t type = data[0];
  const std::string payload(reinterpret_cast<const char*>(data + 1), size - 1);

  const auto decoded = baton::decode_record(type, payload);
  if (!decoded.ok()) return 0;
  BATON_CHECK(static_cast<uint8_t>(baton::type_of(*decoded)) == type);

  std::string first;
  baton::encode_record(*decoded, first);
  const auto again = baton::decode_record(type, first);
  BATON_CHECK(again.ok(), "re-encoded record does not decode: {}", again.error().to_string());
  std::string second;
  baton::encode_record(*again, second);
  BATON_CHECK(first == second, "encoding is not stable");
  return 0;
}
