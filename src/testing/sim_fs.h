#pragma once

// SimFs: an in-memory FileSystem for testing durability code.
//
// It models the two facts that crash-safety depends on:
//
//   1. Appended bytes are volatile until the file is sync()ed.
//   2. Directory changes (create, rename, remove) are volatile until the
//      directory is sync_dir()ed — even if the file's contents were synced.
//
// crash_image() returns the file system as it could look after a crash.
// capture_crash_image_after() takes that image in the middle of a run, after
// the n-th mutating operation, while the code under test keeps going; a test
// then recovers from the image and compares it with what had been acknowledged
// at the moment of capture. SimFs can also inject sync and append failures and
// a capacity limit (ENOSPC).
//
// Thread-safe: one mutex guards everything, because the log thread and the
// test thread both touch it.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>

#include "common/fs.h"

namespace baton {

enum class CrashMode : uint8_t {
  // Power loss with nothing in flight: unsynced bytes and unsynced directory
  // changes are gone.
  kLoseUnsynced,
  // The process died but the OS kept running: everything written survives.
  kKeepUnsynced,
  // Power loss mid-writeback: each file keeps a random prefix of its unsynced
  // tail, the end of which may be garbage; each directory's unsynced changes
  // either all made it or none did.
  kTorn,
};

class SimFs final : public FileSystem {
 public:
  SimFs() = default;

  // --- FileSystem -----------------------------------------------------------
  Status create_dir_if_missing(const std::string& dir) override;
  Result<std::vector<std::string>> list_dir(const std::string& dir) override;
  Result<std::unique_ptr<WritableFile>> open_append(const std::string& path,
                                                    OpenMode mode) override;
  Result<std::string> read_file(const std::string& path) override;
  Result<uint64_t> file_size(const std::string& path) override;
  Status truncate(const std::string& path, uint64_t size) override;
  Status rename(const std::string& from, const std::string& to) override;
  Status remove(const std::string& path) override;
  Status sync_dir(const std::string& dir) override;

  // --- crash simulation -----------------------------------------------------
  // The file system as it might look after a crash right now. The image is
  // fully durable and independent of this instance.
  std::unique_ptr<SimFs> crash_image(CrashMode mode, uint64_t seed = 0) const;

  // After `ops` more mutating operations, take a crash image and call
  // `on_capture` (from the thread performing the operation, with the SimFs lock
  // held: it must not call back into this SimFs).
  void capture_crash_image_after(uint64_t ops, CrashMode mode, uint64_t seed,
                                 std::function<void()> on_capture);
  // The captured image, or nullptr if the trigger has not fired yet.
  std::unique_ptr<SimFs> take_captured_image();

  uint64_t mutating_ops() const;

  // --- fault injection ------------------------------------------------------
  // The n-th sync() from now fails (n = 1 means the next one). Sticky: every
  // later sync fails too, like a dying disk.
  void fail_sync_after(uint64_t n);
  // The n-th append() from now fails after writing half of its data.
  void fail_append_after(uint64_t n);
  // Total bytes across all files; appends beyond it write what fits and fail
  // with "No space left on device".
  void set_capacity(std::optional<uint64_t> bytes);

  // --- test helpers ---------------------------------------------------------
  // Creates or replaces a file; contents and directory entry are durable.
  void write_file(const std::string& path, std::string contents);
  // Flips one bit of a file in place (both the current and durable view).
  void flip_bit(const std::string& path, uint64_t byte_offset, unsigned bit);
  bool exists(const std::string& path) const;

 private:
  struct SimFile {
    std::string data;
    size_t synced = 0;  // data[0, synced) is durable
  };
  using Entries = std::map<std::string, std::shared_ptr<SimFile>>;
  struct SimDir {
    Entries current;
    Entries durable;
  };
  class Handle;

  static std::pair<std::string, std::string> split(const std::string& path);
  std::shared_ptr<SimFile> find_locked(const std::string& path) const;
  uint64_t used_bytes_locked() const;
  std::unique_ptr<SimFs> build_image_locked(CrashMode mode, uint64_t seed) const;
  void count_op_locked();

  Status handle_append(SimFile& file, std::string_view data);
  Status handle_sync(SimFile& file);

  mutable std::mutex mutex_;
  std::map<std::string, SimDir> dirs_;
  uint64_t ops_ = 0;

  struct Capture {
    uint64_t at_op = 0;
    CrashMode mode = CrashMode::kLoseUnsynced;
    uint64_t seed = 0;
    std::function<void()> on_capture;
  };
  std::optional<Capture> capture_;
  std::unique_ptr<SimFs> captured_;

  std::optional<uint64_t> sync_failure_countdown_;
  bool sync_broken_ = false;
  std::optional<uint64_t> append_failure_countdown_;
  std::optional<uint64_t> capacity_;
};

}  // namespace baton
