#pragma once

// The file-system operations baton needs, behind an interface so durability
// code can be tested against SimFs (src/testing/sim_fs.h), which simulates
// crashes, torn writes and I/O errors deterministically.
//
// The interface is deliberately tiny and append-oriented: baton only ever
// appends to files, replaces them atomically via rename, or deletes them.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"

namespace baton {

class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  virtual ~WritableFile() = default;

  // Appends all of `data` or returns an error. After an error the file may
  // contain a prefix of `data`.
  virtual Status append(std::string_view data) = 0;

  // Blocks until everything appended so far is on stable storage
  // (fdatasync on Linux, F_FULLFSYNC on macOS).
  virtual Status sync() = 0;

  // Current size in bytes, including what was there when the file was opened.
  virtual uint64_t size() const = 0;
};

// Held for as long as a process owns a data directory; released on destruction.
class DirLock {
 public:
  DirLock() = default;
  DirLock(const DirLock&) = delete;
  DirLock& operator=(const DirLock&) = delete;
  virtual ~DirLock() = default;
};

enum class OpenMode : uint8_t {
  kCreateNew,      // fail if the file already exists
  kAppendExisting  // fail if the file does not exist
};

class FileSystem {
 public:
  FileSystem() = default;
  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  virtual ~FileSystem() = default;

  virtual Status create_dir_if_missing(const std::string& dir) = 0;

  // File names (not paths) in `dir`, in unspecified order, without "." and "..".
  virtual Result<std::vector<std::string>> list_dir(const std::string& dir) = 0;

  virtual Result<std::unique_ptr<WritableFile>> open_append(const std::string& path,
                                                            OpenMode mode) = 0;

  virtual Result<std::string> read_file(const std::string& path) = 0;
  virtual Result<uint64_t> file_size(const std::string& path) = 0;

  // Shrinks the file to `size` bytes and makes the new length durable.
  virtual Status truncate(const std::string& path, uint64_t size) = 0;

  // Atomically replaces `to` with `from`. Durable only after sync_dir().
  virtual Status rename(const std::string& from, const std::string& to) = 0;

  // Durable only after sync_dir().
  virtual Status remove(const std::string& path) = 0;

  // Makes creations, renames and removals in `dir` durable.
  virtual Status sync_dir(const std::string& dir) = 0;

  // Takes an exclusive, non-blocking lock on `dir` so that two servers can never
  // write the same log. Fails with kFailedPrecondition if it is already held.
  virtual Result<std::unique_ptr<DirLock>> lock_dir(const std::string& dir) = 0;

  // Free space on the file system holding `dir`.
  virtual Result<uint64_t> available_bytes(const std::string& dir) = 0;
};

// "dir/name", tolerating a trailing slash on dir.
std::string join_path(std::string_view dir, std::string_view name);

}  // namespace baton
