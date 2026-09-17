#pragma once

#include "common/fs.h"

namespace baton {

// The real file system. Stateless, so one instance can be shared freely.
class PosixFs final : public FileSystem {
 public:
  Status create_dir_if_missing(const std::string& dir) override;
  Result<std::vector<std::string>> list_dir(const std::string& dir) override;
  Result<std::unique_ptr<WritableFile>> open_append(const std::string& path,
                                                    OpenMode mode) override;
  Result<std::string> read_file(const std::string& path) override;
  Result<std::unique_ptr<ReadableFile>> open_read(const std::string& path) override;
  Result<uint64_t> file_size(const std::string& path) override;
  Status truncate(const std::string& path, uint64_t size) override;
  Status rename(const std::string& from, const std::string& to) override;
  Status remove(const std::string& path) override;
  Status sync_dir(const std::string& dir) override;
  Result<std::unique_ptr<DirLock>> lock_dir(const std::string& dir) override;
  Result<uint64_t> available_bytes(const std::string& dir) override;
};

}  // namespace baton
