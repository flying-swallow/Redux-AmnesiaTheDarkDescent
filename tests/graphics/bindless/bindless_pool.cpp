// Headless coverage for the bindless LRU slot pools (LRUCache and its
// templated sibling LRUCacheState). No GPU, no engine: BindlessPool only needs
// IndexPool + ObjectPool + the header-only hasher.
//
// The regression these pin: the cache-hit path used to relink an entry that
// was already the most-recently-used tail, which pointed the entry's quPrev at
// itself. A later hit on that entry then failed to unlink it, orphaning its
// neighbour from the queue for good. Each orphan permanently loses one id, so
// the pool drained and every later request reported `exhausted` — which the
// renderer surfaces as "Material Slot exhausted" and a dropped draw.

// ObjectPool pulls in stb_ds; its implementation lives in this project's
// stb_ds_impl.cpp rather than the engine's System.cpp, which would drag in the
// whole engine.
#include "graphics/BindlessPool.h"

#include <cstdint>
#include <cstdio>
#include <set>

namespace {

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- LRUCache

// Touching an entry while it is already the queue tail used to relink it to
// itself, and the next hit on that entry then failed to unlink it — which cut
// its neighbour out of the queue for good. The orphaned slot keeps its id but
// can never be reached from the LRU head again, so the pool quietly loses
// capacity until nothing can be evicted and every request reports exhausted.
// Drive that exact access pattern, then check the pool can still hold a full
// capacity's worth of live cookies.
bool CheckTailHitDoesNotOrphanSlots() {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCache cache(kCapacity, /*frameInFlight*/ 0);

  // All within one frame, so nothing is old enough to evict and each miss has
  // to take a fresh id.
  cache.request(/*cookie*/ 1, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 2, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit on the tail
  cache.request(/*cookie*/ 4, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit from the middle

  // Every slot is now stale, so a fresh set of cookies must be able to recycle
  // all of them. One unreachable slot and the last of these reports exhausted.
  std::set<uint32_t> ids;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    auto req = cache.request(/*cookie*/ 100 + i, /*frameIndex*/ 10);
    if (req.exhausted) {
      std::printf("  exhausted after %u of %u slots were recycled\n", i,
                  kCapacity);
      return Check(false, "no slot is orphaned by a hit on the queue tail");
    }
    ids.insert(req.id);
  }
  return Check(ids.size() == kCapacity,
               "recycling the whole pool hands back every distinct id");
}


// A working set that fits inside the pool must never exhaust it, no matter how
// the accesses are ordered. The ordering here is the one that used to orphan
// slots: touch an entry twice in a row (so it is hit while it is the tail),
// push another entry behind it, then touch it again from the middle.
bool CheckWorkingSetNeverExhausts() {
  constexpr uint32_t kCapacity = 8;
  hpl::LRUCache cache(kCapacity, /*frameInFlight*/ 0);

  for (uint32_t frame = 0; frame < 500; ++frame) {
    const hash_t a = 1000 + (frame % 4);
    const hash_t b = 2000 + (frame % 4);
    const hash_t cookies[] = {a, a, b, a, b};
    for (hash_t cookie : cookies) {
      auto req = cache.request(cookie, frame);
      if (req.exhausted) {
        std::printf("  exhausted at frame %u on cookie %llu\n", frame,
                    (unsigned long long)cookie);
        return Check(false,
                     "8-slot pool holding an 8-cookie working set never exhausts");
      }
      if (req.id >= kCapacity)
        return Check(false, "returned id stays inside the pool capacity");
    }
  }
  return true;
}

// Cycling through far more cookies than the pool holds must keep working: the
// least-recently-used entry is recycled once it is older than frameInFlight.
bool CheckEvictionKeepsRecycling() {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCache cache(kCapacity, /*frameInFlight*/ 1);

  std::set<uint32_t> seenIds;
  for (uint32_t frame = 0; frame < 200; ++frame) {
    auto req = cache.request(500 + frame, frame);
    if (req.exhausted)
      return Check(false, "a 4-slot pool keeps recycling across 200 cookies");
    seenIds.insert(req.id);
  }
  return Check(seenIds.size() <= kCapacity,
               "recycling reuses the same ids rather than growing the pool");
}

// A one-element queue makes the entry both head and tail; detaching it has to
// clear both, or the next attach links the entry to itself.
bool CheckSingleSlotQueue() {
  hpl::LRUCache cache(/*numElements*/ 1, /*frameInFlight*/ 0);

  auto first = cache.request(/*cookie*/ 11, /*frameIndex*/ 0);
  if (!Check(!first.exhausted && !first.found, "first cookie takes the only slot"))
    return false;

  auto second = cache.request(/*cookie*/ 22, /*frameIndex*/ 5);
  if (!Check(!second.exhausted && !second.found && second.id == first.id,
             "the second cookie evicts the first and reuses its id"))
    return false;

  auto again = cache.request(/*cookie*/ 22, /*frameIndex*/ 5);
  if (!Check(again.found && again.id == second.id,
             "the surviving cookie is still a cache hit"))
    return false;

  auto evicted = cache.request(/*cookie*/ 11, /*frameIndex*/ 9);
  return Check(!evicted.found, "the evicted cookie is gone, not resurrected");
}

// With free ids left, a new cookie takes one instead of recycling a live entry
// (recycling would throw away that entry's already-uploaded payload).
bool CheckFreeIdsBeatEviction() {
  hpl::LRUCache cache(/*numElements*/ 4, /*frameInFlight*/ 0);

  auto a = cache.request(/*cookie*/ 1, /*frameIndex*/ 0);
  auto b = cache.request(/*cookie*/ 2, /*frameIndex*/ 10);
  if (!Check(!a.exhausted && !b.exhausted && a.id != b.id,
             "a fresh id is handed out while the pool has room"))
    return false;

  auto aAgain = cache.request(/*cookie*/ 1, /*frameIndex*/ 10);
  return Check(aAgain.found && aAgain.id == a.id,
               "the older entry survives because nothing was evicted");
}

// free() must unlink before the slot memory goes back to the pool, and must
// hand the id back.
bool CheckFreeReleasesSlot() {
  hpl::LRUCache cache(/*numElements*/ 2, /*frameInFlight*/ 0);

  auto a = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  auto b = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  if (!Check(!a.exhausted && !b.exhausted, "both cookies fit"))
    return false;

  cache.free(/*cookie*/ 7);

  auto reborn = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  if (!Check(!reborn.exhausted && !reborn.found,
             "the freed cookie comes back as a miss on a reclaimed id"))
    return false;

  auto stillThere = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  return Check(stillThere.found && stillThere.id == b.id,
               "freeing one cookie leaves the other's hash bucket intact");
}

// ----------------------------------------------------- LRUCacheState<T>

// The templated copy carries its own duplicate of the queue mechanics, so it
// needs the same orphan check (see CheckTailHitDoesNotOrphanSlots).
bool CheckStateTailHitDoesNotOrphanSlots() {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCacheState<int> cache(kCapacity, /*frameInFlight*/ 0);

  cache.request(/*cookie*/ 1, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 2, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit on the tail
  cache.request(/*cookie*/ 4, /*frameIndex*/ 0);
  cache.request(/*cookie*/ 3, /*frameIndex*/ 0); // hit from the middle

  std::set<uint32_t> ids;
  for (uint32_t i = 0; i < kCapacity; ++i) {
    auto req = cache.request(/*cookie*/ 100 + i, /*frameIndex*/ 10);
    if (req.exhausted)
      return Check(false,
                   "the templated pool orphans no slot on a queue-tail hit");
    ids.insert(req.id);
  }
  return Check(ids.size() == kCapacity,
               "recycling the whole templated pool hands back every id");
}


// Same contracts against the templated copy, which carries its own duplicate
// of the queue mechanics.
bool CheckStateWorkingSetNeverExhausts() {
  constexpr uint32_t kCapacity = 8;
  hpl::LRUCacheState<int> cache(kCapacity, /*frameInFlight*/ 0);

  for (uint32_t frame = 0; frame < 500; ++frame) {
    const hash_t a = 1000 + (frame % 4);
    const hash_t b = 2000 + (frame % 4);
    const hash_t cookies[] = {a, a, b, a, b};
    for (hash_t cookie : cookies) {
      auto req = cache.request(cookie, frame);
      if (req.exhausted)
        return Check(false,
                     "templated pool holding a fitting working set never exhausts");
      if (req.state == nullptr)
        return Check(false, "a non-exhausted request always yields state");
    }
  }
  return true;
}

// The per-entry state is reset when a slot is recycled for a new cookie, and
// preserved across cache hits.
bool CheckStateLifetime() {
  hpl::LRUCacheState<int> cache(/*numElements*/ 1, /*frameInFlight*/ 0);

  auto first = cache.request(/*cookie*/ 11, /*frameIndex*/ 0);
  if (!Check(!first.exhausted && *first.state == 0, "fresh state starts default"))
    return false;
  *first.state = 42;

  auto hit = cache.request(/*cookie*/ 11, /*frameIndex*/ 0);
  if (!Check(hit.found && *hit.state == 42, "a cache hit keeps the state"))
    return false;

  auto recycled = cache.request(/*cookie*/ 22, /*frameIndex*/ 5);
  return Check(!recycled.exhausted && !recycled.found && *recycled.state == 0,
               "an evicted slot drops the previous owner's state");
}

bool CheckStateEvictionKeepsRecycling() {
  constexpr uint32_t kCapacity = 4;
  hpl::LRUCacheState<int> cache(kCapacity, /*frameInFlight*/ 1);

  std::set<uint32_t> seenIds;
  for (uint32_t frame = 0; frame < 200; ++frame) {
    auto req = cache.request(500 + frame, frame);
    if (req.exhausted)
      return Check(false, "the templated pool keeps recycling across 200 cookies");
    seenIds.insert(req.id);
  }
  return Check(seenIds.size() <= kCapacity,
               "templated recycling reuses ids rather than growing the pool");
}

bool CheckStateFreeReleasesSlot() {
  hpl::LRUCacheState<int> cache(/*numElements*/ 2, /*frameInFlight*/ 0);

  auto a = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  auto b = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  if (!Check(!a.exhausted && !b.exhausted, "both cookies fit"))
    return false;
  *b.state = 5;

  cache.free(/*cookie*/ 7);

  auto reborn = cache.request(/*cookie*/ 7, /*frameIndex*/ 0);
  if (!Check(!reborn.exhausted && !reborn.found,
             "the freed cookie comes back as a miss on a reclaimed id"))
    return false;

  auto stillThere = cache.request(/*cookie*/ 8, /*frameIndex*/ 0);
  return Check(stillThere.found && *stillThere.state == 5,
               "freeing one cookie leaves the other entry untouched");
}

} // namespace

int main() {
  const bool ok = CheckTailHitDoesNotOrphanSlots() &&
                  CheckWorkingSetNeverExhausts() &&
                  CheckEvictionKeepsRecycling() && CheckSingleSlotQueue() &&
                  CheckFreeIdsBeatEviction() && CheckFreeReleasesSlot() &&
                  CheckStateTailHitDoesNotOrphanSlots() &&
                  CheckStateWorkingSetNeverExhausts() && CheckStateLifetime() &&
                  CheckStateEvictionKeepsRecycling() &&
                  CheckStateFreeReleasesSlot();
  if (!ok)
    return 1;
  std::printf("bindless pool tests passed\n");
  return 0;
}
