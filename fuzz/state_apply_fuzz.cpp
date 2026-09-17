// Fuzz target: State::apply() over arbitrary record sequences, as recovery would
// see them from a log that is checksummed but otherwise hostile.
//
// Input: repeated [type u8][length u16 LE][payload]. Properties:
//   - apply() either accepts a record or rejects it and changes nothing; it
//     never crashes;
//   - after the sequence, end_replay() builds derived state and every structural
//     invariant holds;
//   - the state survives a serialize/deserialize round trip unchanged.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/check.h"
#include "state/records.h"
#include "state/state.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  baton::State state;
  state.begin_replay();

  size_t pos = 0;
  while (pos + 3 <= input.size()) {
    const auto type = static_cast<uint8_t>(input[pos]);
    const size_t length = static_cast<uint8_t>(input[pos + 1]) |
                          (static_cast<size_t>(static_cast<uint8_t>(input[pos + 2])) << 8U);
    pos += 3;
    if (length > input.size() - pos) break;
    const std::string_view payload = input.substr(pos, length);
    pos += length;

    const auto record = baton::decode_record(type, payload);
    if (!record.ok()) continue;

    std::string before;
    state.serialize(before);
    if (!state.apply(*record).ok()) {
      std::string after;
      state.serialize(after);
      BATON_CHECK(before == after, "a rejected record changed the state");
    }
  }

  state.set_now(baton::WallTime{1'700'000'000'000}, baton::MonoTime{1'000'000});
  state.end_replay(/*lease_grace_ms=*/1'000);
  const baton::Status invariants = state.check_invariants();
  BATON_CHECK(invariants.ok(), "{}", invariants.error().to_string());

  std::string bytes;
  state.serialize(bytes);
  const auto loaded = baton::State::deserialize(bytes, {});
  BATON_CHECK(loaded.ok(), "own serialization does not load: {}", loaded.error().to_string());
  std::string reserialized;
  (*loaded)->serialize(reserialized);
  BATON_CHECK(bytes == reserialized, "serialization is not stable");
  return 0;
}
