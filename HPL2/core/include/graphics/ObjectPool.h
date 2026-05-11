#pragma once

#include <cstddef>

#include "system/stb_ds.h"

template <typename T> class ObjectPool {
public:
  ObjectPool(size_t chunkSize)
      : start(NULL), end(NULL), poolIndex(0), chunkSize(chunkSize), pool(NULL) {}

  struct PoolSlot {
    struct PoolSlot *next;
    T inner;
  };

  T *allocate() {
    if (start) {
      PoolSlot *slot = start;
      start = start->next;
      if (start == NULL)
        end = NULL; // If we just took the last slot
      return &slot->inner;
    }
    if (pool == NULL || poolIndex == chunkSize) {
      T *newChunk = (T *)calloc(chunkSize, sizeof(PoolSlot));
      if (newChunk == NULL)
        return NULL;
      arrput(pool, newChunk);
      poolIndex = 0;
      return newChunk + (poolIndex++);
    }
    return pool[arrlen(pool) - 1] + (poolIndex++);
  }

  void reset() {
    end = NULL;
    start = NULL;
    for (size_t i = 0; i < arrlen(pool); ++i) {
      PoolSlot *slot = (PoolSlot *)pool[i];
      for (size_t j = 0; j < chunkSize; ++j) {
        if (end)
          end->next = &slot[j];
        else
          start = &slot[j];
        end = slot;
      }
    }
    end = start;
    poolIndex = chunkSize;
  }

  void dealloc(T *ptr) {
    PoolSlot *slot = (PoolSlot*)(((char *)ptr) - offsetof(PoolSlot, inner));
    if (end)
      end->next = slot;
    else
      start = slot;
    end = slot;
  }

  ~ObjectPool() {
    for (size_t i = 0; i < arrlen(pool); ++i) {
      free(pool[i]);
    }
    arrfree(pool);
  }

private:
  PoolSlot *start;
  PoolSlot *end;
  size_t poolIndex;
  size_t chunkSize;
  T **pool;
};
