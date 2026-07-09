#ifndef HPL_RI_TIMELINE_H
#define HPL_RI_TIMELINE_H

#include "graphics/RITypes.h"

#include <cstdint>

namespace hpl {

// A monotonic GPU timeline semaphore. This is the sole source of truth for "the
// GPU is done with frame N's work" — not a per-frame-slot fence. The CPU hands
// out increasing values with next(), the submit that does the work signals that
// value, and completed() reports how far the GPU has actually progressed.
//
// Deferred resource reclamation is layered on top by cGraphics::FrameDeferral,
// which parks per-frame resources against a value and frees them once
// completed() passes it. This struct deliberately knows nothing about that
// bookkeeping.
//
// Usage each frame:
//   uint64_t v = timeline.next();          // value this submit will signal
//   ...submit work that signals (timeline, v)...
//   ...later: if (timeline.completed(device) >= v) /* frame v is done */
//
// Header-only, like RICommandRingBuffer / RISegmentAlloc: the Vulkan entry
// points come through RITypes.h (volk).
struct RITimeline {
  void init(struct RIDevice *device) {
    signalValue = 0;
#if (DEVICE_IMPL_VULKAN)
    vk.semaphore = VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo timelineInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;

    VkSemaphoreCreateInfo createInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &timelineInfo;
    VK_WrapResult(
        vkCreateSemaphore(device->vk.device, &createInfo, NULL, &vk.semaphore));
#endif
  }

  void dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
    if (vk.semaphore) {
      vkDestroySemaphore(device->vk.device, vk.semaphore, NULL);
      vk.semaphore = VK_NULL_HANDLE;
    }
#endif
  }

  // Reserve the next value to signal. Call immediately before the submit that
  // signals this timeline, and signal that submit with the returned value.
  uint64_t next() { return ++signalValue; }
  // Highest value handed out by next() (i.e. the latest in-flight submit).
  uint64_t pending() const { return signalValue; }
  // Value the GPU has actually reached (queries the semaphore counter).
  uint64_t completed(struct RIDevice *device) const {
#if (DEVICE_IMPL_VULKAN)
    uint64_t value = 0;
    VK_WrapResult(
        vkGetSemaphoreCounterValue(device->vk.device, vk.semaphore, &value));
    return value;
#else
    return signalValue;
#endif
  }

  // Block until the GPU reaches `value`. Used at shutdown / when tearing the
  // device down; the normal frame loop polls completed() instead.
  void wait(struct RIDevice *device, uint64_t value) const {
#if (DEVICE_IMPL_VULKAN)
    VkSemaphoreWaitInfo waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &vk.semaphore;
    waitInfo.pValues = &value;
    VK_WrapResult(vkWaitSemaphores(device->vk.device, &waitInfo, UINT64_MAX));
#else
    (void)device;
    (void)value;
#endif
  }

  uint64_t signalValue = 0;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSemaphore semaphore;
    } vk;
#endif
  };
};

} // namespace hpl

#endif // HPL_RI_TIMELINE_H
