#pragma once

// A unique temporary directory that is removed when the object goes away.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace baton {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "baton-test-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    if (created == nullptr) std::abort();
    path_ = created;
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

}  // namespace baton
