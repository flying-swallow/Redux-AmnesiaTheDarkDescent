#include "graphics/BindlessPool.h"
#include <cassert>
#include <cstring>

#define RESERVED_POOL_SLOTS 1024

namespace hpl {

LRUCache::LRUCache(uint32_t numElements, uint32_t frameInFlight)
    : pool(numElements), frameInFlight(frameInFlight), poolSlotPool(RESERVED_POOL_SLOTS), hashSlots() {}

void LRUCache::reset(uint32_t numElements) {
  for(size_t i = 0; i < hashSlots.size(); ++i) {
    hashSlots[i] = NULL;
  }
  queueBegin = NULL;
  queueEnd = NULL;
  pool.resetToReserved(numElements);
  poolSlotPool.reset();
}

void LRUCache::free(hash_t cookie) {
  const size_t hashIndex = cookie % hashSlots.size();
  for (LRUCache::BindlessPoolSlot *c = hashSlots[hashIndex]; c;
       c = c->hNext) {
    if (c->cookie == cookie) {
      // Unlink before the memory goes back to the pool: detachSlot reads the
      // slot's queue/hash links, and the hash walk above would otherwise step
      // through a freed node's hNext.
      detachSlot(c);
      pool.returnId(c->id);
      poolSlotPool.dealloc(c);
      return;
    }
  }
}

void LRUCache::detachSlot(struct BindlessPoolSlot *slot) {
  assert(slot);
  // Queue. The head/tail updates are independent tests, not an if/else chain:
  // a single-element queue has the slot as BOTH queueBegin and queueEnd, and
  // an else-if would clear only one of them, leaving the other dangling at a
  // slot that attachSlot then links to itself.
  {
    if (queueBegin == slot) {
      queueBegin = slot->quNext;
    }
    if (queueEnd == slot) {
      queueEnd = slot->quPrev;
    }
    if (slot->quPrev) {
      slot->quPrev->quNext = slot->quNext;
    }
    if (slot->quNext) {
      slot->quNext->quPrev = slot->quPrev;
    }
    slot->quNext = NULL;
    slot->quPrev = NULL;
  }
  // Hash table.
  {
    const size_t hashIndex = slot->cookie % hashSlots.size();
    if (hashSlots[hashIndex] == slot) {
      hashSlots[hashIndex] = slot->hNext;
    }
    if (slot->hPrev) {
      slot->hPrev->hNext = slot->hNext;
    }
    if (slot->hNext) {
      slot->hNext->hPrev = slot->hPrev;
    }
    slot->hNext = NULL;
    slot->hPrev = NULL;
  }
}
void LRUCache::attachSlot(struct BindlessPoolSlot *slot) {
  assert(slot);
  {
    slot->quNext = NULL;
    slot->quPrev = queueEnd;
    if (queueEnd) {
      queueEnd->quNext = slot;
    }
    queueEnd = slot;
    if (!queueBegin) {
      queueBegin = slot;
    }
  }
  {
    const size_t hashIndex = slot->cookie % hashSlots.size();
    slot->hPrev = NULL;
    slot->hNext = NULL;
    if (hashSlots[hashIndex]) {
      hashSlots[hashIndex]->hPrev = slot;
      slot->hNext = hashSlots[hashIndex];
    }
    hashSlots[hashIndex] = slot;
  }
}

LRUCache::BindlessPoolReq  LRUCache::request(hash_t cookie, uint32_t frameIndex) {
  const size_t hashIndex = cookie % hashSlots.size();
  for (LRUCache::BindlessPoolSlot *c = hashSlots[hashIndex]; c;
       c = c->hNext) {
    if (c->cookie == cookie) {
      // Refresh last-use so the eviction guard below measures frames since
      // this access, not since first allocation. Without this a texture used
      // every frame keeps a stale frameIndex; once it falls to queueBegin the
      // guard passes and its slot is recycled (descriptor overwritten) while
      // frames still in flight reference it — the wrong texture flashes.
      c->frameIndex = frameIndex;
      // Already the most-recently-used tail: nothing to move. This has to
      // return here rather than fall into the relink below — relinking the
      // tail to itself makes quNext == quPrev == c, and once queueBegin walks
      // onto that self-loop it can never advance again, so eviction stops for
      // the life of the pool and every later request reports exhausted.
      if (queueEnd == c) {
        return LRUCache::BindlessPoolReq{ c->id, true, false};
      }
      if (queueBegin == c) {
        queueBegin = c->quNext;
        if (c->quNext) {
          c->quNext->quPrev = NULL;
        }
      } else {
        if (c->quPrev)
          c->quPrev->quNext = c->quNext;
        if (c->quNext)
          c->quNext->quPrev = c->quPrev;
      }
      c->quNext = NULL;
      c->quPrev = queueEnd;
      if (queueEnd) {
        queueEnd->quNext = c;
      }
      queueEnd = c;
      // found a slot with the same cookie
      return LRUCache::BindlessPoolReq{ c->id, true, false};
    }
  }

  // Prefer a never-used id over recycling a live entry: evicting while the id
  // space still has room throws away a cached upload for nothing.
  const uint32_t id = pool.requestId();
  if (id != UINT32_MAX) {
    LRUCache::BindlessPoolSlot* slot =  poolSlotPool.allocate();
    if (slot == NULL) {
      // Out of host memory for the bookkeeping node; give the id back rather
      // than stranding it, and let the caller skip this frame.
      pool.returnId(id);
      return LRUCache::BindlessPoolReq{ UINT32_MAX, false, true };
    }
    memset(slot, 0, sizeof(LRUCache::BindlessPoolSlot));
    slot->cookie = cookie;
    slot->frameIndex = frameIndex;
    slot->id = id;
    attachSlot(slot);
    return LRUCache::BindlessPoolReq{ slot->id, false, false};
  }

  // Pool is full — recycle the least-recently-used slot, but only once it is
  // old enough that no frame in flight can still reference it.
  if (queueBegin && frameIndex > queueBegin->frameIndex + frameInFlight) {
    LRUCache::BindlessPoolSlot *slot = queueBegin;
    detachSlot(slot);
    slot->frameIndex = frameIndex;
    slot->cookie = cookie;
    attachSlot(slot);
    return LRUCache::BindlessPoolReq{ slot->id, false, false};
  }

  // pool is full and no slot is old enough to evict — caller must skip.
  return LRUCache::BindlessPoolReq{ UINT32_MAX, false, true };
}
} // namespace hpl
