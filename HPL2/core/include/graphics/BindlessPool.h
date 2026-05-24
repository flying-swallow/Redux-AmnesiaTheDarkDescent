#pragma once

#include "graphics/IndexPool.h"
#include "graphics/ObjectPool.h"
#include "system/Hasher.h"
#include <cstdint>
#include <array>
#include <cassert>
#include <new>
namespace hpl {

struct LRUCache {
public:
  struct BindlessPoolSlot {
    uint32_t frameIndex; // frame index when this slot was requested
    hash_t cookie;
    uint32_t id;

	  // queue
	  struct BindlessPoolSlot *quNext;
	  struct BindlessPoolSlot *quPrev;

	  // hash
	  struct BindlessPoolSlot *hNext;
	  struct BindlessPoolSlot *hPrev;
  };

  struct BindlessPoolReq {
    uint32_t id;
    bool found;
    bool exhausted; // pool full, no eviction available — id is invalid
  };

  LRUCache(uint32_t numElements, uint32_t frameInFlight);
  LRUCache(const LRUCache&) = delete;
  LRUCache& operator=(const LRUCache&) = delete;

  void reset(uint32_t numElements);
  void free(hash_t cookie);
  LRUCache::BindlessPoolReq request(hash_t cookie, uint32_t frameIndex);
private:

  void detachSlot(struct BindlessPoolSlot *slot );
  void attachSlot(struct BindlessPoolSlot *slot );

  uint32_t frameInFlight;
  IndexPool pool;
  ObjectPool<BindlessPoolSlot> poolSlotPool;
  std::array<LRUCache::BindlessPoolSlot *, 1024> hashSlots;
  struct LRUCache::BindlessPoolSlot *queueBegin = nullptr;
  struct LRUCache::BindlessPoolSlot *queueEnd = nullptr;
};

// Templated sibling of LRUCache that stores a per-entry `T` in each slot. The
// LRU / hash / queue mechanics mirror LRUCache (see BindlessPool.cpp); the only
// addition is `T`'s lifetime. ObjectPool hands out raw memory and never runs
// T's ctor/dtor, so this class owns that: T is placement-new constructed when a
// slot is first allocated, reset (move-assigned) when a slot is evicted and
// reused for a new key, and destroyed when a slot is freed or the cache is
// reset/destroyed. `request()` returns a pointer to the entry's `T` so the
// caller can (re)initialize it on a miss (`found == false`).
//
// Used to park an hpl::EventHandler<> per bindless object slot: a vertex
// buffer's destruction fires the handler, which retires that slot for surfel
// cleanup. The handler's lifetime is bounded by this cache (a renderer member),
// so it can never fire after the renderer is gone. T must be default-
// constructible and move-assignable.
template <typename T>
class LRUCacheState {
public:
  struct Slot {
    uint32_t frameIndex = 0;
    hash_t cookie = 0;
    uint32_t id = 0;
    Slot *quNext = nullptr;
    Slot *quPrev = nullptr;
    Slot *hNext = nullptr;
    Slot *hPrev = nullptr;
    T state{};
  };

  struct Req {
    uint32_t id;
    bool found;
    bool exhausted; // pool full, no eviction available — id/state invalid
    T *state;       // entry state; nullptr only when exhausted
  };

  LRUCacheState(uint32_t numElements, uint32_t frameInFlight)
      : frameInFlight(frameInFlight), pool(numElements),
        poolSlotPool(kReservedSlots), hashSlots() {}
  LRUCacheState(const LRUCacheState &) = delete;
  LRUCacheState &operator=(const LRUCacheState &) = delete;
  ~LRUCacheState() { destroyLiveStates(); }

  void reset(uint32_t numElements) {
    destroyLiveStates();
    for (size_t i = 0; i < hashSlots.size(); ++i)
      hashSlots[i] = nullptr;
    queueBegin = nullptr;
    queueEnd = nullptr;
    pool.resetToReserved(numElements);
    poolSlotPool.reset();
  }

  void free(hash_t cookie) {
    const size_t hashIndex = cookie % hashSlots.size();
    for (Slot *c = hashSlots[hashIndex]; c; c = c->hNext) {
      if (c->cookie == cookie) {
        detachSlot(c);
        pool.returnId(c->id);
        c->~Slot(); // run T's destructor before the memory is recycled
        poolSlotPool.dealloc(c);
        return;
      }
    }
  }

  Req request(hash_t cookie, uint32_t frameIndex) {
    const size_t hashIndex = cookie % hashSlots.size();
    for (Slot *c = hashSlots[hashIndex]; c; c = c->hNext) {
      if (c->cookie == cookie) {
        // Cache hit — move to tail (most-recently-used); state stays intact.
        if (queueEnd == c) {
        } else if (queueBegin == c) {
          queueBegin = c->quNext;
          if (c->quNext)
            c->quNext->quPrev = nullptr;
        } else {
          if (c->quPrev)
            c->quPrev->quNext = c->quNext;
          if (c->quNext)
            c->quNext->quPrev = c->quPrev;
        }
        c->quNext = nullptr;
        c->quPrev = queueEnd;
        if (queueEnd)
          queueEnd->quNext = c;
        queueEnd = c;
        return Req{c->id, true, false, &c->state};
      }
    }

    // Evict the oldest entry if it is old enough; reuse the slot in place.
    if (queueBegin && frameIndex > queueBegin->frameIndex + frameInFlight) {
      Slot *slot = queueBegin;
      detachSlot(slot);
      slot->state = T{}; // drop the evicted owner's state (e.g. disconnect its handler)
      slot->frameIndex = frameIndex;
      slot->cookie = cookie;
      attachSlot(slot);
      return Req{slot->id, false, false, &slot->state};
    }

    // Fresh slot.
    const uint32_t id = pool.requestId();
    if (id == UINT32_MAX)
      return Req{UINT32_MAX, false, true, nullptr};
    Slot *slot = poolSlotPool.allocate();
    assert(slot);
    new (slot) Slot(); // construct T + zero the trivial fields (raw pool memory)
    slot->cookie = cookie;
    slot->frameIndex = frameIndex;
    slot->id = id;
    attachSlot(slot);
    return Req{slot->id, false, false, &slot->state};
  }

private:
  static constexpr size_t kReservedSlots = 1024;

  // Run T's destructor on every live (queued) slot. Freed slots were already
  // destroyed in free(); never-allocated slots were never constructed.
  void destroyLiveStates() {
    for (Slot *c = queueBegin; c;) {
      Slot *next = c->quNext;
      c->~Slot();
      c = next;
    }
  }

  void detachSlot(Slot *slot) {
    assert(slot);
    if (queueBegin == slot) {
      queueBegin = slot->quNext;
      if (slot->quNext)
        slot->quNext->quPrev = nullptr;
    } else if (queueEnd == slot) {
      queueEnd = slot->quPrev;
      if (slot->quPrev)
        slot->quPrev->quNext = nullptr;
    } else {
      if (slot->quPrev)
        slot->quPrev->quNext = slot->quNext;
      if (slot->quNext)
        slot->quNext->quPrev = slot->quPrev;
    }
    const size_t hashIndex = slot->cookie % hashSlots.size();
    if (hashSlots[hashIndex] == slot) {
      hashSlots[hashIndex] = slot->hNext;
      if (slot->hNext)
        slot->hNext->hPrev = nullptr;
    } else {
      if (slot->hPrev)
        slot->hPrev->hNext = slot->hNext;
      if (slot->hNext)
        slot->hNext->hPrev = slot->hPrev;
    }
  }

  void attachSlot(Slot *slot) {
    assert(slot);
    slot->quNext = nullptr;
    slot->quPrev = queueEnd;
    if (queueEnd)
      queueEnd->quNext = slot;
    queueEnd = slot;
    if (!queueBegin)
      queueBegin = slot;
    const size_t hashIndex = slot->cookie % hashSlots.size();
    slot->hPrev = nullptr;
    slot->hNext = nullptr;
    if (hashSlots[hashIndex]) {
      hashSlots[hashIndex]->hPrev = slot;
      slot->hNext = hashSlots[hashIndex];
    }
    hashSlots[hashIndex] = slot;
  }

  uint32_t frameInFlight;
  IndexPool pool;
  ObjectPool<Slot> poolSlotPool;
  std::array<Slot *, 1024> hashSlots;
  Slot *queueBegin = nullptr;
  Slot *queueEnd = nullptr;
};

};
