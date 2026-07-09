#ifndef RI_SHARED_POINTER_H
#define RI_SHARED_POINTER_H

// RIPreamble sets up the backend-selection macros (+ Vulkan headers) so this
// header's layout is deterministic regardless of include order. Keep it first.
#include "graphics/RIPreamble.h"

#include <atomic>

// RISharedPointer only ever uses RIDevice as a pointer, so a forward declaration
// is enough — no need to pull in the full RITypes.h definition here.
struct RIDevice;

// Intrusive, atomically reference-counted shared owner of a raw RI handle.
// `T` must expose `void dispose(RIDevice*)` (RITexture / RITextureView /
// RIBuffer / RISampler / RIAccelStructure). The last reference to drop disposes
// the handle IMMEDIATELY — callers are responsible for GPU-safety (e.g. parking
// the owner in FrameDeferral) where the resource may still be in flight.
// `renderer` + `device` are carried explicitly (flat, no RIDevice::renderer
// back-pointer traversal).
template<typename T>
class RISharedPointer {
public:
  struct RIInternal {
    RIDevice* device;
    std::atomic<unsigned int> references;
    T value;
  };

  RISharedPointer() = default;

  // Adopts `value`; starts the reference count at 1.
  RISharedPointer(RIDevice* device, T value)
      : m_internal(new RIInternal{device, 1, value}) {}

  RISharedPointer(const RISharedPointer& other)
      : m_internal(other.m_internal) {
    if (m_internal)
      m_internal->references.fetch_add(1, std::memory_order_relaxed);
  }
  RISharedPointer& operator=(const RISharedPointer& other) {
    if (m_internal == other.m_internal) // same internal (incl. self) — alias safe
      return *this;
    reset();
    m_internal = other.m_internal;
    if (m_internal)
      m_internal->references.fetch_add(1, std::memory_order_relaxed);
    return *this;
  }

  RISharedPointer(RISharedPointer&& other) noexcept
      : m_internal(other.m_internal) {
    other.m_internal = nullptr;
  }
  RISharedPointer& operator=(RISharedPointer&& other) noexcept {
    if (this == &other)
      return *this;
    reset();
    m_internal = other.m_internal;
    other.m_internal = nullptr;
    return *this;
  }

  ~RISharedPointer() { reset(); }

  // Detach without disposing: hands ownership of one reference back to the
  // caller. Like std::unique_ptr::release.
  T* Release() {
    T* h = m_internal ? &m_internal->value : nullptr;
    m_internal = nullptr;
    return h;
  }

  T* Get() const { return m_internal ? &m_internal->value : nullptr; }
  T* operator->() const { return &m_internal->value; }
  T& operator*() const { return m_internal->value; }
  explicit operator bool() const { return m_internal != nullptr; }
  bool IsValid() const { return m_internal != nullptr; }
  // True when there is no owned handle, or the owned handle is an uncreated
  // (empty) RI resource. Lets a deferral queue skip parking null handles.
  bool isEmpty() const {
    return !m_internal || m_internal->value.isEmpty();
  }

private:
  // Drop one reference; dispose the handle and free the control block once the
  // count hits zero.
  void reset() {
    if (m_internal &&
        m_internal->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      m_internal->value.dispose(m_internal->device);
      delete m_internal;
    }
    m_internal = nullptr;
  }

  RIInternal* m_internal = nullptr;
};

#endif // RI_SHARED_POINTER_H
