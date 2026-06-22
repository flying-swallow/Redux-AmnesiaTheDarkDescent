#pragma once

#include <cstdint>
#include <vector>

namespace hpl {
class IndexPool {
public:
  IndexPool(uint32_t reserve);

  uint32_t requestId();     // highest free id first (top-down)
  uint32_t requestIdLow();  // lowest free id first (bottom-up); packs ids from 0
  void returnId(uint32_t);
  // Return a contiguous [start, end] span to the free set (coalescing neighbours).
  // returnId is the single-id case. Also the primitive grow() builds on.
  void returnRange(uint32_t start, uint32_t end);
  // Extend the id space by `additional` ids (frees [capacity, capacity+additional)
  // and bumps the capacity). Lets a caller recover from an exhausted pool — e.g.
  // a new light past the reserve — and then re-request; the backing RIBuffer grows
  // to fit the new high slot on its next upload. Returns the new capacity.
  uint32_t grow(uint32_t additional);
  uint32_t capacity() const { return m_capacity; }
  void reset();
  void resetToReserved(uint32_t reserve);
private:
  struct IdRange {
    uint32_t m_start;
    uint32_t m_end;
  };
  std::vector<IdRange> m_avaliable;
  uint32_t m_capacity = 0;  // id space is [0, m_capacity); grows via grow()
};

class IndexPoolHandle {
public:
  inline IndexPoolHandle() : m_index(UINT32_MAX), m_pool(nullptr) {}
  explicit inline IndexPoolHandle(IndexPool* pool) : m_index(UINT32_MAX), m_pool(pool) {
    if (m_pool) {
      m_index = m_pool->requestId();
    }
  }
  inline ~IndexPoolHandle() {
    if (m_pool && m_index != UINT32_MAX) {
      m_pool->returnId(m_index);
    }
    m_index = UINT32_MAX;
  }

  inline IndexPoolHandle(const IndexPoolHandle&) = delete;
  inline IndexPoolHandle& operator=(const IndexPoolHandle&) = delete;

  inline IndexPoolHandle(IndexPoolHandle&& other) noexcept
      : m_index(other.m_index), m_pool(other.m_pool) {
    other.m_index = UINT32_MAX;
    other.m_pool = nullptr;
  }
  inline IndexPoolHandle& operator=(IndexPoolHandle&& other) noexcept {
    if (this != &other) {
      if (m_pool && m_index != UINT32_MAX) {
        m_pool->returnId(m_index);
      }
      m_index = other.m_index;
      m_pool = other.m_pool;
      other.m_index = UINT32_MAX;
      other.m_pool = nullptr;
    }
    return *this;
  }

  explicit operator uint32_t() const { return m_index; }
  inline uint32_t get() const { return m_index; }
  inline bool isValid() const { return m_index != UINT32_MAX; }
  inline IndexPool* pool() const { return m_pool; }

private:
  uint32_t m_index;
  IndexPool* m_pool;
};

} // namespace hpl
