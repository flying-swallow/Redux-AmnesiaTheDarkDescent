/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_RESOURCEBASE_H
#define HPL_RESOURCEBASE_H

#include "system/LowLevelSystem.h"
#include "system/SystemTypes.h"
#include "resources/ResourceManager.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <time.h>
#include <utility>

namespace hpl {
class iResourceManager;

class iResourceBase {
public:
  iResourceBase(const tString &asName, const tWString &asFullPath,
                unsigned long alPrio);

  virtual ~iResourceBase();

  /**
   * virtual bool Reload()=0;
   * \return true is reload was succesful, else false.
   */
  virtual bool Reload() = 0;

  /**
   * Free most the memory, save info to get started again.
   */
  virtual void Unload() = 0;

  /**
   * Free all memory.
   */
  virtual void Destroy() = 0;

  const tString &GetName() { return msName; }
  unsigned long GetHandle() { return mlHandle; }
  void SetHandle(unsigned long alHandle) { mlHandle = alHandle; }

  // Stable per-instance cookie (random, assigned in the constructor). Used
  // as a cache key in place of the resource pointer, which is unsafe across
  // free + realloc (a reused address would alias the old entry).
  uint64_t GetUniqueCookie() const { return mUniqueCookie; }

  void SetFullPath(const tWString &asPath);
  const tWString &GetFullPath() { return msFullPath; }

  unsigned long GetTime() { return mlTime; }
  unsigned long GetPrio() { return mlPrio; }

  void SetLogDestruction(bool abX) { mbLogDestruction = abX; }

  unsigned int GetReferenceCount() { return mlUserCount.load(); }
  void AddReference();
  void DropReference() {
    // Dropping a reference that was never held is a refcount bug (e.g. a double
    // release). Catch it in debug; the guard still prevents underflow in release.
    assert(mlUserCount.load() > 0 && "DropReference on a resource with zero references");
    // Single-threaded acquire/release assumed; the atomic guards against torn
    // reads, the guarded decrement is not a lock-free CAS.
    if (mlUserCount.load() > 0)
      mlUserCount--;
  }
  bool HasReferences() { return mlUserCount.load() > 0; }

  static bool GetLogCreateAndDelete() { return mbLogCreateAndDelete; }
  static void SetLogCreateAndDelete(bool abX) { mbLogCreateAndDelete = abX; }

protected:
  static bool mbLogCreateAndDelete;

  tString msName;

  unsigned int mlPrio;  // dunno if this will be of any use.
  unsigned long mlTime; // Time for creation.
  unsigned long mlSize; // for completion. Not used yet.

  std::atomic<unsigned int> mlUserCount;
  unsigned long mlHandle;
  uint64_t mUniqueCookie;
  bool mbLogDestruction;

private:
  tWString msFullPath;
};


// Shared, reference-counted handle to a managed resource. The count IS the
// resource's own iResourceBase reference count: copying adds a reference, and when the
// last reference is dropped the handle frees the resource through the owning manager.
// The handle stores a raw back-pointer to that manager and calls its virtual
// iResourceManager::FreeResource() at zero — no per-handle closure. The manager
// outlives every handle to its resources (handles are released before the manager is
// destroyed), so the back-pointer is always valid when used.
//
// There is no non-owning mode: a handle either owns exactly one reference or is
// empty. To share a pointer you only borrowed, take a real reference with
// RetainResource; to hand the reference off as a raw pointer, use Release().
// Single-threaded acquire/release is assumed.
template<class T>
class SharedResourceHandle {
public:
  SharedResourceHandle() = default;

  // Fundamental ctor: adopt one already-held reference (no increment); the owning
  // manager frees the resource when the last reference drops.
  SharedResourceHandle(iResourceManager* manager, T* handle)
      : m_handle(handle), m_manager(manager) {}

  SharedResourceHandle(const SharedResourceHandle& other)
      : m_handle(other.m_handle), m_manager(other.m_manager) {
    if (m_handle) m_handle->AddReference();
  }
  SharedResourceHandle(SharedResourceHandle&& other) noexcept
      : m_handle(other.m_handle), m_manager(other.m_manager) {
    other.m_handle = nullptr;
    other.m_manager = nullptr;
  }

  ~SharedResourceHandle() { reset(); }

  SharedResourceHandle& operator=(const SharedResourceHandle& other) {
    if (m_handle == other.m_handle) return *this; // same resource (incl. self) — alias safe
    reset();
    m_handle = other.m_handle;
    m_manager = other.m_manager;
    if (m_handle) m_handle->AddReference();
    return *this;
  }
  SharedResourceHandle& operator=(SharedResourceHandle&& other) noexcept {
    if (this == &other) return *this;
    reset();
    m_handle = other.m_handle;
    m_manager = other.m_manager;
    other.m_handle = nullptr;
    other.m_manager = nullptr;
    return *this;
  }

  // Detach without releasing: hands the held reference back to the caller as a raw
  // pointer (the caller becomes responsible for it). Like std::unique_ptr::release.
  T* Release() {
    T* h = m_handle;
    m_handle = nullptr;
    m_manager = nullptr;
    return h;
  }

  T* Get() const { return m_handle; }
  T* operator->() const { return m_handle; }
  T& operator*() const { return *m_handle; }
  explicit operator bool() const { return m_handle != nullptr; }
  bool IsValid() const { return m_handle != nullptr; }

private:
  // Drop this handle's reference; free through the manager once the count hits zero.
  void reset() {
    if (!m_handle) return;
    m_handle->DropReference();
    if (!m_handle->HasReferences() && m_manager)
      m_manager->FreeResource(m_handle);
    m_handle = nullptr;
    m_manager = nullptr;
  }

  T* m_handle = nullptr;
  iResourceManager* m_manager = nullptr;
};

// Mint a handle to a manager resource, taking the one reference this handle owns.
// This is the single bridge from a raw manager pointer to a shared handle: the
// manager's Create*/Get* does NOT touch the count, the handle does it here. Works
// for both a freshly loaded resource (count 0 -> 1) and a cache hit (n -> n+1).
template<class T>
inline SharedResourceHandle<T> AcquireResource(iResourceManager* manager, iResourceBase* base) {
  if (base) base->AddReference();
  return SharedResourceHandle<T>(manager, static_cast<T*>(base));
}

// Alias of AcquireResource for call sites that read better as "share this live
// resource I only have a borrowed pointer to".
template<class T>
inline SharedResourceHandle<T> RetainResource(iResourceManager* manager, T* handle) {
  return AcquireResource<T>(manager, handle);
}

}; // namespace hpl
#endif // HPL_RESOURCEBASE_H
