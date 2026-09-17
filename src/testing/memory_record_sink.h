#pragma once

// A RecordSink that keeps records in memory, standing in for the durable log in
// tests of the state machine. replay_into() is the test-side equivalent of
// recovery: decode each record and apply it.

#include <string>
#include <vector>

#include "state/engine.h"
#include "state/records.h"
#include "state/state.h"

namespace baton {

class MemoryRecordSink final : public RecordSink {
 public:
  struct Entry {
    Lsn lsn = 0;
    RecordType type{};
    std::string payload;
  };

  Lsn append(RecordType type, std::string_view payload) override {
    entries_.push_back(
        Entry{.lsn = entries_.size() + 1, .type = type, .payload = std::string(payload)});
    return entries_.back().lsn;
  }

  const std::vector<Entry>& entries() const { return entries_; }

  // Applies entries [first, first + count) to `state`.
  Status replay_into(State& state, size_t first = 0, size_t count = SIZE_MAX) const {
    for (size_t i = first; i < entries_.size() && i - first < count; ++i) {
      BATON_ASSIGN_OR_RETURN(
          const Record record,
          decode_record(static_cast<uint8_t>(entries_[i].type), entries_[i].payload));
      BATON_RETURN_IF_ERROR(state.apply(record));
    }
    return {};
  }

 private:
  std::vector<Entry> entries_;
};

}  // namespace baton
