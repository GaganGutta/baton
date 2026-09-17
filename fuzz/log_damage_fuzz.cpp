// Fuzz target: build a valid multi-segment log, then let the fuzzer damage it.
//
// Raw-byte fuzzing cannot get past the record checksums, so this target is
// structure-aware. The input is a little program:
//
//   byte 0        records per segment (1..8)
//   byte 1        number of records   (0..63)
//   then repeated damage operations:
//     0 seg off bit   flip one bit
//     1 seg off       truncate the segment
//     2 seg off len s overwrite a run with pseudo-random garbage (seed s)
//     3 seg           delete the segment
//     4 seg len s     append pseudo-random garbage (seed s)
//
// Garbage comes from a PRNG rather than straight from the input on purpose:
// libFuzzer's compare tracing can learn checksum values, and with raw control
// over the bytes it could forge a record with a valid CRC. A forged record is
// not a recovery bug (a checksum is not a MAC), so the oracle below would cry
// wolf. PRNG output cannot be steered into a valid CRC.
//
// Whatever the damage, recovery must either refuse to start or deliver an
// intact prefix of the original records: never a damaged record, never a
// record out of order, never anything after a gap. And it must not crash.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/check.h"
#include "log/format.h"
#include "log/recovery.h"
#include "testing/sim_fs.h"

namespace {

constexpr const char* kDir = "d";

class Input {
 public:
  Input(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool empty() const { return pos_ >= size_; }
  uint8_t byte() { return empty() ? 0 : data_[pos_++]; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

// xorshift64*: cheap, deterministic garbage.
class Garbage {
 public:
  explicit Garbage(uint8_t seed) : state_(0x9E3779B97F4A7C15ULL ^ seed) {}
  char next() {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return static_cast<char>((state_ * 0x2545F4914F6CDD1DULL) >> 56U);
  }

 private:
  uint64_t state_;
};

std::string payload_for(baton::Lsn lsn) {
  return "record-" + std::to_string(lsn) + std::string(lsn % 40, 'x');
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Input in(data, size);
  const unsigned per_segment = 1 + (in.byte() % 8);
  const unsigned total = in.byte() % 64;

  // Build the pristine log.
  std::vector<std::string> names;
  std::vector<std::string> segments;
  for (baton::Lsn lsn = 1; lsn <= total; ++lsn) {
    if ((lsn - 1) % per_segment == 0) {
      names.push_back(baton::segment_file_name(lsn));
      segments.emplace_back();
      baton::append_segment_header(segments.back(), lsn);
    }
    baton::append_record(segments.back(), lsn, static_cast<uint8_t>(lsn % 5), payload_for(lsn));
  }

  // Apply the damage program.
  std::vector<bool> deleted(segments.size(), false);
  while (!in.empty() && !segments.empty()) {
    const uint8_t op = in.byte() % 5;
    const size_t seg = in.byte() % segments.size();
    std::string& bytes = segments[seg];
    const size_t offset =
        bytes.empty() ? 0 : ((size_t{in.byte()} << 8U) | in.byte()) % bytes.size();
    switch (op) {
      case 0:
        if (!bytes.empty()) {
          bytes[offset] =
              static_cast<char>(static_cast<uint8_t>(bytes[offset]) ^ (1U << (in.byte() % 8)));
        }
        break;
      case 1:
        bytes.resize(offset);
        break;
      case 2: {
        const size_t len = std::min<size_t>(in.byte(), bytes.size() - offset);
        Garbage garbage(in.byte());
        for (size_t i = 0; i < len; ++i) bytes[offset + i] = garbage.next();
        break;
      }
      case 3:
        deleted[seg] = true;
        break;
      default: {
        const size_t len = in.byte();
        Garbage garbage(in.byte());
        for (size_t i = 0; i < len; ++i) bytes.push_back(garbage.next());
        break;
      }
    }
  }

  baton::SimFs fs;
  BATON_CHECK(fs.create_dir_if_missing(kDir).ok());
  for (size_t i = 0; i < segments.size(); ++i) {
    if (!deleted[i]) fs.write_file(baton::join_path(kDir, names[i]), segments[i]);
  }

  baton::Lsn expected = 1;
  const auto recovered =
      baton::recover_log(fs, kDir, {}, [&](const baton::LogRecordView& r) -> baton::Status {
        BATON_CHECK(r.lsn == expected, "delivered LSN {} but expected {}", r.lsn, expected);
        BATON_CHECK(r.lsn <= total, "delivered LSN {} which was never written", r.lsn);
        BATON_CHECK(r.payload == payload_for(r.lsn), "delivered a damaged payload for LSN {}",
                    r.lsn);
        BATON_CHECK(r.type == r.lsn % 5, "delivered a damaged type for LSN {}", r.lsn);
        ++expected;
        return {};
      });
  if (recovered.ok()) BATON_CHECK(recovered->last_lsn == expected - 1);
  return 0;
}
