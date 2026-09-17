// Writes seed inputs for the fuzz targets, so they start from valid structures
// instead of having to discover the formats byte by byte.
//
//   fuzz_make_seeds <corpus-root>   creates <corpus-root>/<target>/seed-*

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "log/format.h"

namespace {

void write_seed(const std::filesystem::path& dir, const std::string& name,
                const std::string& bytes) {
  std::filesystem::create_directories(dir);
  std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string segment_with_records(int count) {
  std::string data;
  baton::append_segment_header(data, 1);
  for (int i = 1; i <= count; ++i) {
    baton::append_record(data, static_cast<baton::Lsn>(i), static_cast<uint8_t>(i),
                         "payload-" + std::to_string(i));
  }
  return data;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: fuzz_make_seeds <corpus-root>\n";
    return 2;
  }
  const std::filesystem::path root = argv[1];

  const std::string three = segment_with_records(3);
  write_seed(root / "fuzz_log_segment", "seed-empty-segment", segment_with_records(0));
  write_seed(root / "fuzz_log_segment", "seed-three-records", three);
  write_seed(root / "fuzz_log_segment", "seed-torn-tail", three.substr(0, three.size() - 5));
  write_seed(root / "fuzz_log_segment", "seed-torn-header", three.substr(0, 20));

  // Damage programs: [records per segment, record count, ops...]
  write_seed(root / "fuzz_log_damage", "seed-pristine", std::string("\x03\x0A", 2));
  write_seed(root / "fuzz_log_damage", "seed-flip-mid",
             std::string("\x03\x0A\x00\x01\x00\x30\x02", 7));
  write_seed(root / "fuzz_log_damage", "seed-truncate-last",
             std::string("\x03\x0A\x01\x03\x00\x40", 6));
  write_seed(root / "fuzz_log_damage", "seed-delete-middle", std::string("\x02\x0A\x03\x02", 4));
  write_seed(root / "fuzz_log_damage", "seed-append-garbage",
             std::string("\x04\x0C\x04\x02\x20\x07", 6));
  return 0;
}
