#pragma once

#include "graphics/IndexPool.h"
#include "graphics/ObjectPool.h"
#include "system/Hasher.h"
#include <cstdint>
#include <array>
namespace hpl {

struct BindlessPool {
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

  BindlessPool(uint32_t numElements, uint32_t frameInFlight);
  BindlessPool(const BindlessPool&) = delete;
  BindlessPool& operator=(const BindlessPool&) = delete;

  void reset(uint32_t numElements);
  void free(hash_t cookie);
  BindlessPool::BindlessPoolReq request(hash_t cookie, uint32_t frameIndex);
private:

  void detachSlot(struct BindlessPoolSlot *slot );
  void attachSlot(struct BindlessPoolSlot *slot );

  uint32_t frameInFlight;
  IndexPool pool;
  ObjectPool<BindlessPoolSlot> poolSlotPool;
  std::array<BindlessPool::BindlessPoolSlot *, 1024> hashSlots;
  struct BindlessPool::BindlessPoolSlot *queueBegin = nullptr;
  struct BindlessPool::BindlessPoolSlot *queueEnd = nullptr;
};

};
