#include "sched/timing_wheel.h"

#include <algorithm>
#include <bit>

#include "common/check.h"

namespace baton {
namespace {

constexpr unsigned kTopLevel = TimingWheel::kLevels - 1;
constexpr uint64_t kSlotMask = TimingWheel::kSlotsPerLevel - 1;

constexpr unsigned shift_of(unsigned level) { return level * TimingWheel::kSlotBits; }
constexpr uint64_t slot_width(unsigned level) { return uint64_t{1} << shift_of(level); }

}  // namespace

TimingWheel::TimingWheel(uint64_t now) : now_(now) { heads_.fill(kNil); }

// --- slab and lists ---------------------------------------------------------------

uint32_t TimingWheel::allocate() {
  if (free_head_ != kNil) {
    const uint32_t index = free_head_;
    free_head_ = nodes_[index].next;
    return index;
  }
  BATON_CHECK(nodes_.size() < kNil, "timing wheel slab is full");
  nodes_.emplace_back();
  return static_cast<uint32_t>(nodes_.size() - 1);
}

void TimingWheel::release(uint32_t index) {
  Node& node = nodes_[index];
  ++node.generation;  // invalidates every handle to this slab entry
  node.list = kFree;
  node.prev = kNil;
  node.next = free_head_;
  free_head_ = index;
}

void TimingWheel::link(uint32_t index, uint16_t list) {
  Node& node = nodes_[index];
  node.list = list;
  node.prev = kNil;
  node.next = heads_[list];
  if (node.next != kNil) nodes_[node.next].prev = index;
  heads_[list] = index;
  if (list < kDueList) occupied_[list >> kSlotBits] |= uint64_t{1} << (list & kSlotMask);
}

void TimingWheel::unlink(uint32_t index) {
  const Node& node = nodes_[index];
  if (node.prev != kNil) {
    nodes_[node.prev].next = node.next;
  } else {
    heads_[node.list] = node.next;
  }
  if (node.next != kNil) nodes_[node.next].prev = node.prev;
  if (heads_[node.list] == kNil && node.list < kDueList) {
    occupied_[node.list >> kSlotBits] &= ~(uint64_t{1} << (node.list & kSlotMask));
  }
}

// Chooses the list for a node from its deadline and the current time.
//
// The level is the position of the highest bit in which the deadline differs
// from now: if they differ only in the low 6 bits the timer is less than 64
// ticks away and goes on level 0, in the slot named by those bits; if the
// highest difference is in bits 6..11 it goes on level 1; and so on. Because all
// higher bits agree, the chosen slot is always ahead of the current position on
// its level, so lower levels never wrap.
//
// The top level is the exception. It also takes every deadline whose
// difference lies above the wheel's span, which can put a timer in a slot at or
// behind the current position: such a timer belongs to the *next* rotation.
// next_expiration() accounts for that. A deadline a full span or more away is
// parked one slot behind the current position - the last slot to come up -
// and re-examined there.
void TimingWheel::place(uint32_t index) {
  const uint64_t deadline = nodes_[index].deadline;
  if (deadline <= now_) {
    link(index, kDueList);
    return;
  }
  uint64_t target = deadline;
  if (deadline - now_ >= kSpan) target = now_ + kSpan - slot_width(kTopLevel);

  const uint64_t differing = (now_ ^ target) | kSlotMask;
  const unsigned highest_bit = 63U - static_cast<unsigned>(std::countl_zero(differing));
  const unsigned level = std::min(highest_bit / kSlotBits, kTopLevel);
  const auto slot = static_cast<unsigned>((target >> shift_of(level)) & kSlotMask);
  link(index, static_cast<uint16_t>((level << kSlotBits) | slot));
}

// --- public API ---------------------------------------------------------------------

TimerHandle TimingWheel::schedule(uint64_t deadline, uint8_t kind, uint64_t id) {
  const uint32_t index = allocate();
  Node& node = nodes_[index];
  node.deadline = deadline;
  node.id = id;
  node.kind = kind;
  place(index);
  ++size_;
  return TimerHandle{.index = index, .generation = node.generation};
}

bool TimingWheel::cancel(TimerHandle handle) {
  if (handle.index >= nodes_.size()) return false;
  const Node& node = nodes_[handle.index];
  if (node.list == kFree || node.generation != handle.generation) return false;
  unlink(handle.index);
  release(handle.index);
  --size_;
  return true;
}

void TimingWheel::fire(uint32_t index, std::vector<TimerEvent>& fired) {
  const Node& node = nodes_[index];
  fired.push_back(TimerEvent{.handle = TimerHandle{.index = index, .generation = node.generation},
                             .deadline = node.deadline,
                             .id = node.id,
                             .kind = node.kind});
  release(index);
  --size_;
}

std::optional<TimingWheel::Expiration> TimingWheel::next_expiration() const {
  std::optional<Expiration> best;
  for (unsigned level = 0; level < kLevels; ++level) {
    const uint64_t occupied = occupied_[level];
    if (occupied == 0) continue;

    // Scan the slots in the order they come up: the one after the current
    // position first, the current position last (it is a full rotation away).
    const auto now_slot = static_cast<unsigned>((now_ >> shift_of(level)) & kSlotMask);
    const unsigned first = (now_slot + 1) & kSlotMask;
    const auto distance =
        static_cast<unsigned>(std::countr_zero(std::rotr(occupied, static_cast<int>(first))));
    const unsigned slot = (first + distance) & kSlotMask;

    const uint64_t rotation = slot_width(level) << kSlotBits;
    const uint64_t rotation_start = now_ & ~(rotation - 1);
    uint64_t deadline = rotation_start + (slot * slot_width(level));
    if (deadline <= now_) deadline += rotation;  // the slot belongs to the next rotation

    if (!best || deadline < best->deadline) {
      best = Expiration{.level = level, .slot = slot, .deadline = deadline};
    }
  }
  return best;
}

void TimingWheel::advance(uint64_t now, std::vector<TimerEvent>& fired) {
  while (heads_[kDueList] != kNil) {
    const uint32_t index = heads_[kDueList];
    unlink(index);
    fire(index, fired);
  }
  if (now <= now_) return;

  while (const std::optional<Expiration> next = next_expiration()) {
    if (next->deadline > now) break;
    now_ = next->deadline;

    // Detach the whole slot first, then fire or re-place each timer. Re-placing
    // relative to the new now_ sends a timer to a lower level (or fires it if
    // this tick is its deadline), never back into the slot being emptied.
    const auto list = static_cast<uint16_t>((next->level << kSlotBits) | next->slot);
    uint32_t index = heads_[list];
    heads_[list] = kNil;
    occupied_[next->level] &= ~(uint64_t{1} << next->slot);
    while (index != kNil) {
      const uint32_t following = nodes_[index].next;
      if (nodes_[index].deadline <= now_) {
        fire(index, fired);
      } else {
        place(index);
      }
      index = following;
    }
  }
  now_ = now;
}

std::optional<uint64_t> TimingWheel::next_wakeup() const {
  if (heads_[kDueList] != kNil) return now_;
  if (const std::optional<Expiration> next = next_expiration()) return next->deadline;
  return std::nullopt;
}

}  // namespace baton
