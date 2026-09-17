#include "testing/sim_fs.h"

#include <algorithm>
#include <format>
#include <utility>

#include "common/check.h"

namespace baton {

// A writable handle keeps the file alive even if it is renamed or removed,
// like a POSIX file descriptor does.
class SimFs::Handle final : public WritableFile {
 public:
  Handle(SimFs& fs, std::shared_ptr<SimFile> file, std::string name)
      : fs_(fs), file_(std::move(file)), name_(std::move(name)) {}

  Status append(std::string_view data) override { return fs_.handle_append(*file_, data); }
  Status sync() override { return fs_.handle_sync(*file_, name_); }
  uint64_t size() const override {
    const std::scoped_lock lock(fs_.mutex_);
    return file_->data.size();
  }

 private:
  SimFs& fs_;
  std::shared_ptr<SimFile> file_;
  std::string name_;  // as opened; a later rename does not change it
};

std::pair<std::string, std::string> SimFs::split(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  BATON_CHECK(slash != std::string::npos, "SimFs paths must be dir/name, got '{}'", path);
  return {path.substr(0, slash), path.substr(slash + 1)};
}

std::shared_ptr<SimFs::SimFile> SimFs::find_locked(const std::string& path) const {
  const auto [dir, name] = split(path);
  const auto d = dirs_.find(dir);
  if (d == dirs_.end()) return nullptr;
  const auto f = d->second.current.find(name);
  return f == d->second.current.end() ? nullptr : f->second;
}

uint64_t SimFs::used_bytes_locked() const {
  uint64_t total = 0;
  for (const auto& [dir_name, dir] : dirs_) {
    for (const auto& [name, file] : dir.current) total += file->data.size();
  }
  return total;
}

void SimFs::count_op_locked() {
  ++ops_;
  if (capture_ && ops_ == capture_->at_op) {
    captured_ = build_image_locked(capture_->mode, capture_->seed);
    const std::function<void()> callback = std::move(capture_->on_capture);
    capture_.reset();
    if (callback) callback();
  }
}

// --- FileSystem ---------------------------------------------------------------

Status SimFs::create_dir_if_missing(const std::string& dir) {
  const std::scoped_lock lock(mutex_);
  dirs_.try_emplace(dir);  // directory creation itself is treated as durable
  return {};
}

Result<std::vector<std::string>> SimFs::list_dir(const std::string& dir) {
  const std::scoped_lock lock(mutex_);
  const auto d = dirs_.find(dir);
  if (d == dirs_.end())
    return Error{ErrorCode::kIo, std::format("opendir {}: no such directory", dir)};
  std::vector<std::string> names;
  names.reserve(d->second.current.size());
  for (const auto& [name, file] : d->second.current) names.push_back(name);
  return names;
}

Result<std::unique_ptr<WritableFile>> SimFs::open_append(const std::string& path, OpenMode mode) {
  const std::scoped_lock lock(mutex_);
  const auto [dir, name] = split(path);
  const auto d = dirs_.find(dir);
  if (d == dirs_.end())
    return Error{ErrorCode::kIo, std::format("open {}: no such directory", path)};

  auto existing = d->second.current.find(name);
  if (mode == OpenMode::kCreateNew) {
    if (existing != d->second.current.end()) {
      return Error{ErrorCode::kIo, std::format("open {}: File exists", path)};
    }
    auto file = std::make_shared<SimFile>();
    d->second.current.emplace(name, file);
    d->second.pending.push_back(
        DirOp{.kind = DirOp::Kind::kLink, .name = name, .new_name = {}, .file = file});
    count_op_locked();
    return std::unique_ptr<WritableFile>(std::make_unique<Handle>(*this, std::move(file), name));
  }
  if (existing == d->second.current.end()) {
    return Error{ErrorCode::kIo, std::format("open {}: No such file or directory", path)};
  }
  return std::unique_ptr<WritableFile>(std::make_unique<Handle>(*this, existing->second, name));
}

Result<std::string> SimFs::read_file(const std::string& path) {
  const std::scoped_lock lock(mutex_);
  const auto file = find_locked(path);
  if (!file) return Error{ErrorCode::kIo, std::format("open {}: No such file or directory", path)};
  return file->data;
}

// Reads see the file as it was when it was opened, like a descriptor on a file
// that is later replaced by rename.
class SimFs::Reader final : public ReadableFile {
 public:
  explicit Reader(std::string data) : data_(std::move(data)) {}

  Result<size_t> read(size_t max, std::string& out) override {
    const size_t n = std::min(max, data_.size() - offset_);
    out.append(data_, offset_, n);
    offset_ += n;
    return n;
  }

 private:
  std::string data_;
  size_t offset_ = 0;
};

Result<std::unique_ptr<ReadableFile>> SimFs::open_read(const std::string& path) {
  const std::scoped_lock lock(mutex_);
  const auto file = find_locked(path);
  if (!file) return Error{ErrorCode::kIo, std::format("open {}: No such file or directory", path)};
  return std::unique_ptr<ReadableFile>(std::make_unique<Reader>(file->data));
}

Result<uint64_t> SimFs::file_size(const std::string& path) {
  const std::scoped_lock lock(mutex_);
  const auto file = find_locked(path);
  if (!file) return Error{ErrorCode::kIo, std::format("stat {}: No such file or directory", path)};
  return static_cast<uint64_t>(file->data.size());
}

Status SimFs::truncate(const std::string& path, uint64_t size) {
  const std::scoped_lock lock(mutex_);
  const auto file = find_locked(path);
  if (!file) return Error{ErrorCode::kIo, std::format("open {}: No such file or directory", path)};
  BATON_CHECK(size <= file->data.size(), "SimFs::truncate can only shrink");
  file->data.resize(static_cast<size_t>(size));
  file->synced = file->data.size();  // the interface promises a durable truncate
  count_op_locked();
  return {};
}

Status SimFs::rename(const std::string& from, const std::string& to) {
  const std::scoped_lock lock(mutex_);
  const auto [from_dir, from_name] = split(from);
  const auto [to_dir, to_name] = split(to);
  BATON_CHECK(from_dir == to_dir, "SimFs only renames within one directory");
  const auto d = dirs_.find(from_dir);
  if (d == dirs_.end())
    return Error{ErrorCode::kIo, std::format("rename {}: no such directory", from)};
  const auto f = d->second.current.find(from_name);
  if (f == d->second.current.end()) {
    return Error{ErrorCode::kIo, std::format("rename {}: No such file or directory", from)};
  }
  std::shared_ptr<SimFile> file = f->second;
  d->second.current.erase(f);
  d->second.current[to_name] = file;
  d->second.pending.push_back(DirOp{.kind = DirOp::Kind::kRename,
                                    .name = from_name,
                                    .new_name = to_name,
                                    .file = std::move(file)});
  count_op_locked();
  return {};
}

Status SimFs::remove(const std::string& path) {
  const std::scoped_lock lock(mutex_);
  const auto [dir, name] = split(path);
  const auto d = dirs_.find(dir);
  if (d == dirs_.end() || d->second.current.erase(name) == 0) {
    return Error{ErrorCode::kIo, std::format("unlink {}: No such file or directory", path)};
  }
  d->second.pending.push_back(
      DirOp{.kind = DirOp::Kind::kUnlink, .name = name, .new_name = {}, .file = nullptr});
  count_op_locked();
  return {};
}

Status SimFs::sync_dir(const std::string& dir) {
  const std::scoped_lock lock(mutex_);
  const auto d = dirs_.find(dir);
  if (d == dirs_.end())
    return Error{ErrorCode::kIo, std::format("fsync dir {}: no such directory", dir)};
  d->second.durable = d->second.current;
  d->second.pending.clear();
  count_op_locked();
  return {};
}

Status SimFs::handle_append(SimFile& file, std::string_view data) {
  const std::scoped_lock lock(mutex_);
  if (append_failure_countdown_ && --*append_failure_countdown_ == 0) {
    append_failure_countdown_.reset();
    file.data.append(data.substr(0, data.size() / 2));
    count_op_locked();
    return Error{ErrorCode::kIo, "write: Input/output error (injected)"};
  }
  if (capacity_) {
    const uint64_t used = used_bytes_locked();
    const uint64_t room = *capacity_ > used ? *capacity_ - used : 0;
    if (data.size() > room) {
      file.data.append(data.substr(0, static_cast<size_t>(room)));
      count_op_locked();
      return Error{ErrorCode::kIo, "write: No space left on device (injected)"};
    }
  }
  file.data.append(data);
  count_op_locked();
  return {};
}

Status SimFs::handle_sync(SimFile& file, const std::string& name) {
  std::unique_lock lock(mutex_);
  sync_gate_.wait(lock, [&] { return !syncs_held_ || !name.starts_with(held_prefix_); });
  if (sync_failure_countdown_ && --*sync_failure_countdown_ == 0) {
    sync_failure_countdown_.reset();
    sync_broken_ = true;
  }
  if (sync_broken_) {
    count_op_locked();
    return Error{ErrorCode::kIo, "fsync: Input/output error (injected)"};
  }
  file.synced = file.data.size();
  count_op_locked();
  return {};
}

void SimFs::hold_syncs(std::string_view name_prefix) {
  const std::scoped_lock lock(mutex_);
  syncs_held_ = true;
  held_prefix_ = std::string(name_prefix);
}

void SimFs::release_syncs() {
  {
    const std::scoped_lock lock(mutex_);
    syncs_held_ = false;
  }
  sync_gate_.notify_all();
}

class SimFs::Lock final : public DirLock {
 public:
  Lock(SimFs& fs, std::string dir) : fs_(fs), dir_(std::move(dir)) {}
  ~Lock() override {
    const std::scoped_lock lock(fs_.mutex_);
    fs_.locked_dirs_.erase(dir_);
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  SimFs& fs_;
  std::string dir_;
};

Result<std::unique_ptr<DirLock>> SimFs::lock_dir(const std::string& dir) {
  const std::scoped_lock lock(mutex_);
  if (!locked_dirs_.insert(dir).second) {
    return Error{ErrorCode::kFailedPrecondition,
                 std::format("data directory {} is in use by another baton process", dir)};
  }
  return std::unique_ptr<DirLock>(std::make_unique<Lock>(*this, dir));
}

Result<uint64_t> SimFs::available_bytes(const std::string& /*dir*/) {
  const std::scoped_lock lock(mutex_);
  if (!capacity_) return uint64_t{1} << 40U;
  const uint64_t used = used_bytes_locked();
  return *capacity_ > used ? *capacity_ - used : 0;
}

// --- crash simulation ---------------------------------------------------------

std::unique_ptr<SimFs> SimFs::build_image_locked(CrashMode mode, uint64_t seed) const {
  std::mt19937_64 rng(seed);
  auto image = std::make_unique<SimFs>();

  for (const auto& [dir_name, dir] : dirs_) {
    // Which directory entries survive. POSIX promises nothing about unsynced
    // creates, renames and removes, not even their order, so a torn crash keeps
    // an arbitrary subset of them (each applied atomically, in order).
    Entries surviving = mode == CrashMode::kKeepUnsynced ? dir.current : dir.durable;
    if (mode == CrashMode::kTorn) {
      for (const DirOp& op : dir.pending) {
        if ((rng() & 1U) == 0) continue;
        if (op.kind != DirOp::Kind::kLink) surviving.erase(op.name);
        if (op.kind == DirOp::Kind::kLink) surviving[op.name] = op.file;
        if (op.kind == DirOp::Kind::kRename) surviving[op.new_name] = op.file;
      }
    }

    SimDir& out_dir = image->dirs_[dir_name];
    for (const auto& [name, file] : surviving) {
      size_t keep = file->data.size();
      if (mode == CrashMode::kLoseUnsynced) keep = file->synced;
      if (mode == CrashMode::kTorn) {
        const size_t unsynced = file->data.size() - file->synced;
        keep = file->synced + static_cast<size_t>(rng() % (unsynced + 1));
      }

      auto copy = std::make_shared<SimFile>();
      copy->data = file->data.substr(0, keep);
      if (mode == CrashMode::kTorn && keep > file->synced && (rng() & 1U) != 0) {
        // A partially written sector: the end of what survived is garbage.
        const size_t garbage = std::min<size_t>(keep - file->synced, 1 + (rng() % 64));
        for (size_t i = keep - garbage; i < keep; ++i) copy->data[i] = static_cast<char>(rng());
      }
      copy->synced = copy->data.size();
      out_dir.current[name] = copy;
      out_dir.durable[name] = std::move(copy);
    }
  }
  return image;
}

std::unique_ptr<SimFs> SimFs::crash_image(CrashMode mode, uint64_t seed) const {
  const std::scoped_lock lock(mutex_);
  return build_image_locked(mode, seed);
}

void SimFs::capture_crash_image_after(uint64_t ops, CrashMode mode, uint64_t seed,
                                      std::function<void()> on_capture) {
  BATON_CHECK(ops > 0);
  const std::scoped_lock lock(mutex_);
  capture_ =
      Capture{.at_op = ops_ + ops, .mode = mode, .seed = seed, .on_capture = std::move(on_capture)};
}

std::unique_ptr<SimFs> SimFs::take_captured_image() {
  const std::scoped_lock lock(mutex_);
  return std::move(captured_);
}

uint64_t SimFs::mutating_ops() const {
  const std::scoped_lock lock(mutex_);
  return ops_;
}

// --- fault injection ----------------------------------------------------------

void SimFs::fail_sync_after(uint64_t n) {
  BATON_CHECK(n > 0);
  const std::scoped_lock lock(mutex_);
  sync_failure_countdown_ = n;
}

void SimFs::fail_append_after(uint64_t n) {
  BATON_CHECK(n > 0);
  const std::scoped_lock lock(mutex_);
  append_failure_countdown_ = n;
}

void SimFs::set_capacity(std::optional<uint64_t> bytes) {
  const std::scoped_lock lock(mutex_);
  capacity_ = bytes;
}

// --- test helpers ---------------------------------------------------------------

void SimFs::write_file(const std::string& path, std::string contents) {
  const std::scoped_lock lock(mutex_);
  const auto [dir, name] = split(path);
  auto file = std::make_shared<SimFile>();
  file->data = std::move(contents);
  file->synced = file->data.size();
  SimDir& d = dirs_[dir];
  d.current[name] = file;
  d.durable[name] = std::move(file);
}

void SimFs::flip_bit(const std::string& path, uint64_t byte_offset, unsigned bit) {
  const std::scoped_lock lock(mutex_);
  const auto file = find_locked(path);
  BATON_CHECK(file != nullptr, "flip_bit: no such file '{}'", path);
  BATON_CHECK(byte_offset < file->data.size());
  BATON_CHECK(bit < 8);
  file->data[static_cast<size_t>(byte_offset)] = static_cast<char>(
      static_cast<unsigned char>(file->data[static_cast<size_t>(byte_offset)]) ^ (1U << bit));
}

bool SimFs::exists(const std::string& path) const {
  const std::scoped_lock lock(mutex_);
  return find_locked(path) != nullptr;
}

}  // namespace baton
