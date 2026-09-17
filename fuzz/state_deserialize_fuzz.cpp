// Fuzz target: State::deserialize(), the body of a snapshot.
//
// Properties: arbitrary bytes are either rejected or produce a State whose
// invariants hold once derived state is built, and which re-serializes to bytes
// that load again. Never a crash, never an oversized allocation.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/check.h"
#include "state/state.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto loaded = baton::State::deserialize(input, {});
  if (!loaded.ok()) return 0;

  baton::State& state = **loaded;
  baton::Status invariants = state.check_invariants();
  BATON_CHECK(invariants.ok(), "after load: {}", invariants.error().to_string());

  state.set_now(baton::WallTime{1'700'000'000'000}, baton::MonoTime{1'000'000});
  state.end_replay(/*lease_grace_ms=*/1'000);
  invariants = state.check_invariants();
  BATON_CHECK(invariants.ok(), "after rebuild: {}", invariants.error().to_string());

  std::string bytes;
  state.serialize(bytes);
  const auto again = baton::State::deserialize(bytes, {});
  BATON_CHECK(again.ok(), "own serialization does not load: {}", again.error().to_string());
  return 0;
}
