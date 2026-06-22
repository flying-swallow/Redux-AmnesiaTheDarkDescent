#include "graphics/IndexPool.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iterator>

#include "system/stb_ds.h"

namespace hpl {

    IndexPool::IndexPool(uint32_t reserve) {
        m_avaliable.push_back({0, reserve - 1});
        m_capacity = reserve;
    }

    // Top-down: hand out the HIGHEST free id (back range's high end). O(1) — the
    // tail range shrinks / pops. Default; values are opaque to most callers.
    uint32_t IndexPool::requestId() {
        if(m_avaliable.size() > 0) {
            auto& entry = m_avaliable[m_avaliable.size() - 1];
            if(entry.m_end == entry.m_start) {
                const uint32_t res = entry.m_start;
                m_avaliable.pop_back();
                return res;
            }
            const uint32_t res = entry.m_end;
            entry.m_end--;
            return res;
        }
        return UINT32_MAX;
    }

    // Bottom-up: hand out the LOWEST free id (front range's low end) so allocations
    // stay packed from 0. Use this when buffer size / iteration tracks the
    // high-water slot — e.g. the world light-slot pools feeding the per-cell GPU
    // light grid, where a sparse high slot would inflate the per-cell loop. Also
    // makes "burn the first id to reserve slot 0" actually reserve 0. m_avaliable
    // is kept sorted ascending by returnId, so front() is the lowest range.
    uint32_t IndexPool::requestIdLow() {
        if(m_avaliable.empty())
            return UINT32_MAX;
        IdRange& entry = m_avaliable.front();
        const uint32_t res = entry.m_start;
        if(entry.m_start == entry.m_end) {
            m_avaliable.erase(m_avaliable.begin());   // range fully consumed
        } else {
            entry.m_start++;
        }
        return res;
    }

    void IndexPool::reset() {
      m_avaliable.clear();
      if (m_capacity > 0)
        m_avaliable.push_back({0, m_capacity - 1});
    }

    void IndexPool::resetToReserved(uint32_t reserve) {
      m_avaliable.clear();
      m_avaliable.push_back({0, reserve - 1});
      m_capacity = reserve;
    }

    void IndexPool::returnId(uint32_t index) { returnRange(index, index); }

    void IndexPool::returnRange(uint32_t start, uint32_t end) {
        if (start > end)
            return;
        // Insert keeping m_avaliable sorted ascending by m_start, then coalesce
        // with the neighbour ranges on either side (asserts catch a double-free /
        // overlap with an already-free span).
        auto it = std::lower_bound(
            m_avaliable.begin(), m_avaliable.end(), start,
            [](const IdRange &r, uint32_t v) { return r.m_start < v; });
        it = m_avaliable.insert(it, {start, end});

        if (it != m_avaliable.begin()) {
            auto prev = std::prev(it);
            assert(prev->m_end < start && "returnRange overlaps a free range");
            if (prev->m_end + 1 == start) {
                prev->m_end = it->m_end;
                it = std::prev(m_avaliable.erase(it));
            }
        }
        auto nxt = std::next(it);
        if (nxt != m_avaliable.end()) {
            assert(it->m_end < nxt->m_start && "returnRange overlaps a free range");
            if (it->m_end + 1 == nxt->m_start) {
                it->m_end = nxt->m_end;
                m_avaliable.erase(nxt);
            }
        }
    }

    uint32_t IndexPool::grow(uint32_t additional) {
        if (additional == 0)
            return m_capacity;
        // The fresh ids sit above every existing range, so this coalesces onto the
        // tail (or appends) in O(1) amortized.
        returnRange(m_capacity, m_capacity + additional - 1);
        m_capacity += additional;
        return m_capacity;
    }

}
