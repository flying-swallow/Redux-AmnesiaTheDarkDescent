#pragma once

#include <cstdint>
#include <vector>

namespace hpl {
class IndexPool {
public:
  IndexPool(uint32_t reserve);

  uint32_t requestId();
  void returnId(uint32_t);
  void reset();
  void resetToReserved(uint32_t reserve);
private:
  struct IdRange {
    uint32_t m_start;
    uint32_t m_end;
  };
  std::vector<IdRange> m_avaliable;
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
