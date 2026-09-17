#pragma once

// A hashed hierarchical timing wheel (Varghese & Lauck, 1987): O(1) schedule and
// cancel for millions of pending timers. Design notes: docs/design.md 5.7.
//
//  - 6 levels x 64 slots, 1-tick resolution. A slot on level k is 64^k ticks
//    wide, so the wheel spans 64^6 ticks (~2.2 years of milliseconds). Later
//    deadlines are parked on the top level and re-examined when they come up.
//  - The level of a deadline is the position of the highest bit in which it
//    differs from the current time. One occupancy bitmap per level turns "which
//    slot is next?" into a rotate and a count-trailing-zeros.
//  - When time reaches a slot on level k > 0, its timers are re-inserted and
//    land on a lower level (they "cascade"), so every timer fires on exactly
//    its own tick.
//  - Timers live in a slab and are linked by index: no allocation per timer and
//    no pointers. Handles carry a generation number, so a stale handle (already
//    fired or cancelled) can never affect the timer now using the same slab
//    entry.
//
// The wheel knows nothing about clocks or jobs: ticks are just uint64_t, and a
// timer carries an opaque (kind, id) for the owner to interpret.
// Single-threaded.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace baton {

struct TimerHandle {
  uint32_t index = UINT32_MAX;
  uint32_t generation = 0;

  bool valid() const { return index != UINT32_MAX; }
  friend bool operator==(TimerHandle, TimerHandle) = default;
};

struct TimerEvent {
  TimerHandle handle;  // the (now stale) handle this timer was scheduled under
  uint64_t deadline = 0;
  uint64_t id = 0;
  uint8_t kind = 0;
};

class TimingWheel {
 public:
  static constexpr unsigned kLevels = 6;
  static constexpr unsigned kSlotBits = 6;
  static constexpr unsigned kSlotsPerLevel = 1U << kSlotBits;
  static constexpr uint64_t kSpan = uint64_t{1} << (kLevels * kSlotBits);  // 64^6 ticks

  explicit TimingWheel(uint64_t now = 0);

  // Schedules a timer. A deadline that is not in the future fires on the next
  // advance(), whatever `now` that call is given.
  TimerHandle schedule(uint64_t deadline, uint8_t kind, uint64_t id);

  // Returns false if the handle is stale (the timer already fired or was
  // cancelled), which is always safe to ignore.
  bool cancel(TimerHandle handle);

  // Moves time forward to `now` and appends every timer with deadline <= now to
  // `fired`, in deadline order. Nothing fires re-entrantly: the caller handles
  // the events after this returns, and may schedule or cancel while doing so.
  // Time never moves backwards; an older `now` only fires what is already due.
  void advance(uint64_t now, std::vector<TimerEvent>& fired);

  // The earliest tick at which advance() has work to do (a timer to fire or a
  // slot to cascade), or nullopt if no timers are pending. Never later than the
  // earliest deadline, so sleeping until this tick cannot make a timer late.
  std::optional<uint64_t> next_wakeup() const;

  uint64_t now() const { return now_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Bytes of slab memory per pending timer, for memory accounting.
  static constexpr size_t kBytesPerTimer = 40;

 private:
  static constexpr uint32_t kNil = UINT32_MAX;
  static constexpr uint16_t kDueList = kLevels * kSlotsPerLevel;  // deadline <= now_
  static constexpr uint16_t kFree = kDueList + 1;

  struct Node {
    uint64_t deadline = 0;
    uint64_t id = 0;
    uint32_t prev = kNil;
    uint32_t next = kNil;
    uint32_t generation = 0;
    uint16_t list = kFree;  // which list the node is on: a slot, kDueList or kFree
    uint8_t kind = 0;
  };
  static_assert(sizeof(Node) <= kBytesPerTimer);

  struct Expiration {
    unsigned level = 0;
    unsigned slot = 0;
    uint64_t deadline = 0;  // the tick at which the slot must be processed
  };

  uint32_t allocate();
  void release(uint32_t index);
  void link(uint32_t index, uint16_t list);
  void unlink(uint32_t index);
  void place(uint32_t index);  // choose the list for nodes_[index].deadline relative to now_
  void fire(uint32_t index, std::vector<TimerEvent>& fired);
  std::optional<Expiration> next_expiration() const;

  uint64_t now_;
  size_t size_ = 0;
  std::vector<Node> nodes_;
  uint32_t free_head_ = kNil;
  std::array<uint32_t, kDueList + 1> heads_{};  // filled with kNil by the constructor
  std::array<uint64_t, kLevels> occupied_{};
};

}  // namespace baton
