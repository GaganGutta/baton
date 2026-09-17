#include "common/posix_fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <format>

#include "common/fd.h"

namespace baton {
namespace {

// Flushes file data to stable storage. On macOS fsync() only reaches the
// drive's cache, so F_FULLFSYNC is required for real durability.
int sync_data(int fd) {
#ifdef __APPLE__
  return ::fcntl(fd, F_FULLFSYNC) == -1 ? -1 : 0;
#else
  return ::fdatasync(fd);
#endif
}

// EINTR means the wait was interrupted, not that writeback failed, so retrying
// is safe. Any other failure is reported to the caller, and the log treats it
// as fatal (docs/design.md, "What happens when the disk misbehaves").
int retry_on_eintr_sync(int fd) {
  int rc = 0;
  do {
    rc = sync_data(fd);
  } while (rc != 0 && errno == EINTR);
  return rc;
}

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string path, Fd fd, uint64_t size)
      : path_(std::move(path)), fd_(std::move(fd)), size_(size) {}

  Status append(std::string_view data) override {
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
      const ssize_t n = ::write(fd_.get(), p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        return io_error(std::format("write {}", path_), errno);
      }
      p += n;
      left -= static_cast<size_t>(n);
      size_ += static_cast<uint64_t>(n);
    }
    return {};
  }

  Status sync() override {
    if (retry_on_eintr_sync(fd_.get()) != 0) return io_error(std::format("fsync {}", path_), errno);
    return {};
  }

  uint64_t size() const override { return size_; }

 private:
  std::string path_;
  Fd fd_;
  uint64_t size_;
};

// flock() is released by the kernel when the descriptor closes, including when
// the process dies, so a crashed server never leaves a stale lock behind.
class PosixDirLock final : public DirLock {
 public:
  explicit PosixDirLock(Fd fd) : fd_(std::move(fd)) {}

 private:
  Fd fd_;
};

}  // namespace

std::string join_path(std::string_view dir, std::string_view name) {
  std::string path(dir);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(name);
  return path;
}

Status PosixFs::create_dir_if_missing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) return {};
  return io_error(std::format("mkdir {}", dir), errno);
}

Result<std::vector<std::string>> PosixFs::list_dir(const std::string& dir) {
  DIR* raw = ::opendir(dir.c_str());
  if (raw == nullptr) return io_error(std::format("opendir {}", dir), errno);
  const std::unique_ptr<DIR, int (*)(DIR*)> handle(raw, &::closedir);

  std::vector<std::string> names;
  for (;;) {
    errno = 0;
    const dirent* entry = ::readdir(handle.get());
    if (entry == nullptr) {
      if (errno != 0) return io_error(std::format("readdir {}", dir), errno);
      break;
    }
    const std::string_view name = entry->d_name;
    if (name != "." && name != "..") names.emplace_back(name);
  }
  return names;
}

Result<std::unique_ptr<WritableFile>> PosixFs::open_append(const std::string& path, OpenMode mode) {
  int flags = O_WRONLY | O_APPEND | O_CLOEXEC;
  if (mode == OpenMode::kCreateNew) flags |= O_CREAT | O_EXCL;
  Fd fd(::open(path.c_str(), flags, 0644));
  if (!fd.valid()) return io_error(std::format("open {}", path), errno);

  struct stat st{};
  if (::fstat(fd.get(), &st) != 0) return io_error(std::format("fstat {}", path), errno);
  return std::unique_ptr<WritableFile>(
      std::make_unique<PosixWritableFile>(path, std::move(fd), static_cast<uint64_t>(st.st_size)));
}

Result<std::string> PosixFs::read_file(const std::string& path) {
  const Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd.valid()) return io_error(std::format("open {}", path), errno);

  struct stat st{};
  if (::fstat(fd.get(), &st) != 0) return io_error(std::format("fstat {}", path), errno);

  std::string data;
  data.resize(static_cast<size_t>(st.st_size));
  size_t got = 0;
  while (got < data.size()) {
    const ssize_t n = ::read(fd.get(), data.data() + got, data.size() - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      return io_error(std::format("read {}", path), errno);
    }
    if (n == 0) break;  // the file shrank underneath us; return what is there
    got += static_cast<size_t>(n);
  }
  data.resize(got);
  return data;
}

Result<uint64_t> PosixFs::file_size(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return io_error(std::format("stat {}", path), errno);
  return static_cast<uint64_t>(st.st_size);
}

Status PosixFs::truncate(const std::string& path, uint64_t size) {
  const Fd fd(::open(path.c_str(), O_WRONLY | O_CLOEXEC));
  if (!fd.valid()) return io_error(std::format("open {}", path), errno);
  if (::ftruncate(fd.get(), static_cast<off_t>(size)) != 0) {
    return io_error(std::format("ftruncate {}", path), errno);
  }
  // A size change is metadata; fdatasync is specified to flush metadata that
  // is needed to read the data back, which includes the length.
  if (retry_on_eintr_sync(fd.get()) != 0) return io_error(std::format("fsync {}", path), errno);
  return {};
}

Status PosixFs::rename(const std::string& from, const std::string& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return io_error(std::format("rename {} -> {}", from, to), errno);
  }
  return {};
}

Status PosixFs::remove(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return io_error(std::format("unlink {}", path), errno);
  return {};
}

Status PosixFs::sync_dir(const std::string& dir) {
  const Fd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!fd.valid()) return io_error(std::format("open dir {}", dir), errno);
  // Directories need a full fsync: the entries are the metadata we care about.
  int rc = 0;
  do {
    rc = ::fsync(fd.get());
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) return io_error(std::format("fsync dir {}", dir), errno);
  return {};
}

Result<std::unique_ptr<DirLock>> PosixFs::lock_dir(const std::string& dir) {
  const std::string path = join_path(dir, "LOCK");
  Fd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.valid()) return io_error(std::format("open {}", path), errno);
  if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      return Error{ErrorCode::kFailedPrecondition,
                   std::format("data directory {} is in use by another baton process", dir)};
    }
    return io_error(std::format("flock {}", path), errno);
  }
  return std::unique_ptr<DirLock>(std::make_unique<PosixDirLock>(std::move(fd)));
}

Result<uint64_t> PosixFs::available_bytes(const std::string& dir) {
  struct statvfs info{};
  if (::statvfs(dir.c_str(), &info) != 0) return io_error(std::format("statvfs {}", dir), errno);
  return static_cast<uint64_t>(info.f_bavail) * static_cast<uint64_t>(info.f_frsize);
}

}  // namespace baton
