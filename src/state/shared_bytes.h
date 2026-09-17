#pragma once

// Immutable, reference-counted bytes for job payloads.
//
// A payload never changes after ENQUEUE, so copies of a job can share it. That
// is what makes a consistent snapshot copy cheap (M5): copying a job bumps a
// reference count instead of copying the payload. The count is atomic
// (std::shared_ptr), so the snapshot thread may hold and drop references while
// the event loop does the same.
//
// One allocation holds the control block and the bytes.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

namespace baton {

// `char[]` below is the element type of a shared array (one allocation for the
// control block and the bytes), not a C array declaration.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
class SharedBytes {
 public:
  SharedBytes() = default;

  explicit SharedBytes(std::string_view data) : size_(data.size()) {
    if (data.empty()) return;
    std::shared_ptr<char[]> buffer = std::make_shared_for_overwrite<char[]>(data.size());
    std::memcpy(buffer.get(), data.data(), data.size());
    data_ = std::move(buffer);
  }

  std::string_view view() const { return {data_.get(), size_}; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Heap bytes attributable to this payload (shared across all copies).
  size_t allocated_bytes() const { return size_ == 0 ? 0 : size_ + kControlBlockBytes; }

 private:
  static constexpr size_t kControlBlockBytes = 32;

  std::shared_ptr<const char[]> data_;
  size_t size_ = 0;
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

}  // namespace baton
