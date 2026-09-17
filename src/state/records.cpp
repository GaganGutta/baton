#include "state/records.h"

#include <format>
#include <limits>

#include "common/codec.h"

namespace baton {
namespace {

// Every payload starts with this byte so that fields can be added later.
constexpr uint8_t kPayloadVersion = 1;

void put_time(ByteWriter& w, WallTime t) { w.svarint(t.ms); }

// Timestamps outside [0, kMaxWallTimeMs] are rejected, so arithmetic on decoded
// times (deadline - now, expiry + grace) can never overflow.
WallTime get_time(ByteReader& r, bool& in_range) {
  const int64_t ms = r.svarint();
  if (ms < 0 || ms > kMaxWallTimeMs) in_range = false;
  return WallTime{ms};
}

// Narrowing reads fail the reader instead of wrapping around.
uint32_t get_u32(ByteReader& r, bool& in_range) {
  const uint64_t v = r.varint();
  if (v > std::numeric_limits<uint32_t>::max()) in_range = false;
  return static_cast<uint32_t>(v);
}

int32_t get_i32(ByteReader& r, bool& in_range) {
  const int64_t v = r.svarint();
  if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
    in_range = false;
  }
  return static_cast<int32_t>(v);
}

struct Encoder {
  ByteWriter& w;

  void operator()(const JobEnqueued& r) const {
    w.varint(r.id);
    w.bytes(r.queue);
    w.bytes(r.payload);
    w.svarint(r.priority);
    put_time(w, r.run_at);
    w.varint(r.max_attempts);
    w.varint(r.backoff_base_ms);
    w.varint(r.backoff_cap_ms);
    w.bytes(r.idem_key);
    put_time(w, r.idem_expires_at);
    put_time(w, r.at);
  }
  void operator()(const JobLeased& r) const {
    w.varint(r.id);
    w.varint(r.token);
    put_time(w, r.lease_expires_at);
    put_time(w, r.at);
  }
  void operator()(const LeaseExtended& r) const {
    w.varint(r.id);
    w.varint(r.token);
    put_time(w, r.lease_expires_at);
    put_time(w, r.at);
  }
  void operator()(const JobSucceeded& r) const {
    w.varint(r.id);
    w.varint(r.token);
    put_time(w, r.at);
  }
  void operator()(const AttemptFailed& r) const {
    w.varint(r.id);
    w.varint(r.token);
    w.u8(static_cast<uint8_t>(r.reason));
    w.bytes(r.error);
    put_time(w, r.at);
    w.u8(r.dead ? 1 : 0);
    put_time(w, r.retry_at);
  }
  void operator()(const JobCancelled& r) const {
    w.varint(r.id);
    put_time(w, r.at);
  }
  void operator()(const DeadJobRetried& r) const {
    w.varint(r.id);
    put_time(w, r.run_at);
    put_time(w, r.at);
  }
  void operator()(const JobsPurged& r) const {
    w.varint(r.ids.size());
    for (const JobId id : r.ids) w.varint(id);
  }
};

Error bad_record(uint8_t type, std::string_view what) {
  return Error{ErrorCode::kCorruption, std::format("record type {}: {}", type, what)};
}

}  // namespace

RecordType type_of(const Record& record) {
  // The variant alternatives are declared in wire order, starting at 1.
  return static_cast<RecordType>(record.index() + 1);
}

void encode_record(const Record& record, std::string& out) {
  ByteWriter w(out);
  w.u8(kPayloadVersion);
  std::visit(Encoder{w}, record);
}

Result<Record> decode_record(uint8_t type, std::string_view payload) {
  ByteReader r(payload);
  if (r.u8() != kPayloadVersion || !r.ok()) return bad_record(type, "unknown payload version");

  bool in_range = true;
  Record record;
  switch (static_cast<RecordType>(type)) {
    case RecordType::kJobEnqueued: {
      JobEnqueued v;
      v.id = r.varint();
      v.queue = r.bytes();
      v.payload = r.bytes();
      v.priority = get_i32(r, in_range);
      v.run_at = get_time(r, in_range);
      v.max_attempts = get_u32(r, in_range);
      v.backoff_base_ms = get_u32(r, in_range);
      v.backoff_cap_ms = get_u32(r, in_range);
      v.idem_key = r.bytes();
      v.idem_expires_at = get_time(r, in_range);
      v.at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kJobLeased: {
      JobLeased v;
      v.id = r.varint();
      v.token = r.varint();
      v.lease_expires_at = get_time(r, in_range);
      v.at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kLeaseExtended: {
      LeaseExtended v;
      v.id = r.varint();
      v.token = r.varint();
      v.lease_expires_at = get_time(r, in_range);
      v.at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kJobSucceeded: {
      JobSucceeded v;
      v.id = r.varint();
      v.token = r.varint();
      v.at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kAttemptFailed: {
      AttemptFailed v;
      v.id = r.varint();
      v.token = r.varint();
      const uint8_t reason = r.u8();
      if (reason > static_cast<uint8_t>(FailureReason::kLeaseExpired)) {
        return bad_record(type, "unknown failure reason");
      }
      v.reason = static_cast<FailureReason>(reason);
      v.error = r.bytes();
      v.at = get_time(r, in_range);
      const uint8_t dead = r.u8();
      if (dead > 1) return bad_record(type, "invalid outcome");
      v.dead = dead == 1;
      v.retry_at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kJobCancelled: {
      JobCancelled v;
      v.id = r.varint();
      v.at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kDeadJobRetried: {
      DeadJobRetried v;
      v.id = r.varint();
      v.run_at = get_time(r, in_range);
      v.at = get_time(r, in_range);
      record = v;
      break;
    }
    case RecordType::kJobsPurged: {
      JobsPurged v;
      const uint64_t count = r.varint();
      // Every id takes at least one byte, so a count beyond the remaining input
      // is corrupt; checking first keeps a hostile count from reserving memory.
      if (count > r.remaining()) return bad_record(type, "id count exceeds payload");
      v.ids.reserve(static_cast<size_t>(count));
      for (uint64_t i = 0; i < count; ++i) v.ids.push_back(r.varint());
      record = std::move(v);
      break;
    }
    default:
      return bad_record(type, "unknown record type");
  }

  if (!r.ok()) return bad_record(type, "truncated payload");
  if (!in_range) return bad_record(type, "field out of range");
  if (!r.at_end()) return bad_record(type, "trailing bytes");
  return record;
}

}  // namespace baton
