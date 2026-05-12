#include "graphics/BindlessPool.h"
#include <cassert>

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
      pool.returnId(c->id);
      poolSlotPool.dealloc(c);
      detachSlot(c);
    }
  }
}

void LRUCache::detachSlot(struct BindlessPoolSlot *slot) {
  assert(slot);
  // remove from queue
  {
    if (queueBegin == slot) {
      queueBegin = slot->quNext;
      if (slot->quNext) {
        slot->quNext->quPrev = NULL;
      }
    } else if (queueEnd == slot) {
      queueEnd = slot->quPrev;
      if (slot->quPrev) {
        slot->quPrev->quNext = NULL;
      }
    } else {
      if (slot->quPrev) {
        slot->quPrev->quNext = slot->quNext;
      }
      if (slot->quNext) {
        slot->quNext->quPrev = slot->quPrev;
      }
    }
  }
  // free from hashTable
  {
    const size_t hashIndex = slot->cookie % hashSlots.size();
    if (hashSlots[hashIndex] == slot) {
      hashSlots[hashIndex] = slot->hNext;
      if (slot->hNext) {
        slot->hNext->hPrev = NULL;
      }
    } else {
      if (slot->hPrev) {
        slot->hPrev->hNext = slot->hNext;
      }
      if (slot->hNext) {
        slot->hNext->hPrev = slot->hPrev;
      }
    }
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
      if (queueEnd == c) {
      } else if (queueBegin == c) {
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

  if (queueBegin && frameIndex > queueBegin->frameIndex + frameInFlight) {
    LRUCache::BindlessPoolSlot *slot = queueBegin;
    detachSlot(slot);
    slot->frameIndex = frameIndex;
    slot->cookie = cookie;
    attachSlot(slot);
    return LRUCache::BindlessPoolReq{ slot->id, false, false};
  }

  const uint32_t id = pool.requestId();
  if (id == UINT32_MAX) {
    // pool is full and no slot is old enough to evict — caller must skip.
    return LRUCache::BindlessPoolReq{ UINT32_MAX, false, true };
  }
  LRUCache::BindlessPoolSlot* slot =  poolSlotPool.allocate();
  memset(slot, 0, sizeof(LRUCache::BindlessPoolSlot));
  assert(slot);
  slot->cookie = cookie;
  slot->frameIndex = frameIndex;
  slot->id = id;
  attachSlot(slot);
  return LRUCache::BindlessPoolReq{ slot->id, false, false};
}
} // namespace hpl
