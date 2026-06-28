#include "graphics/RIGpuProfiler.h"

#include "graphics/RITypes.h"

#include <vector>

namespace hpl {

#if (DEVICE_IMPL_VULKAN)
static constexpr VkPipelineStageFlags2 kTimestampStage =
    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
#endif

void RIGpuProfiler::init(struct RIDevice *device) {
  m_enabled = false;
#if (DEVICE_IMPL_VULKAN)
  if (!RIIsTargetSelected(RI_DEVICE_API_VK))
    return;

  const uint64_t freq = device->physicalAdapter.timestampFrequencyHz;
  if (freq == 0)
    return; // device reports no timestamp clock

  // timestampValidBits is per queue family, not in RIPhysicalAdapter — query it
  // directly for the graphics family (the only queue the primary CB submits on).
  VkPhysicalDevice phys = device->physicalAdapter.vk.physicalDevice;
  const uint32_t qfIdx = device->queues[RI_QUEUE_GRAPHICS].vk.queueFamilyIdx;
  uint32_t qfCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfProps(qfCount);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfProps.data());
  const uint32_t validBits =
      (qfIdx < qfCount) ? qfProps[qfIdx].timestampValidBits : 0;
  if (validBits == 0)
    return; // graphics queue does not support timestamps

  m_validBitsMask = (validBits >= 64) ? ~0ull : ((1ull << validBits) - 1ull);
  m_ticksToMs = 1000.0 / (double)freq;

  for (auto &slot : m_slots) {
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = kMaxQueries;
    if (!VK_WrapResult(
            vkCreateQueryPool(device->vk.device, &ci, nullptr, &slot.pool))) {
      // Roll back any pools created so far and stay disabled.
      for (auto &s : m_slots) {
        if (s.pool) {
          vkDestroyQueryPool(device->vk.device, s.pool, nullptr);
          s.pool = VK_NULL_HANDLE;
        }
      }
      return;
    }
    slot.timelineValue = 0;
    slot.resolved = true;
    slot.queryCount = 0;
    slot.scopes.clear();
  }
  m_enabled = true;
#else
  (void)device;
#endif
}

void RIGpuProfiler::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  for (auto &slot : m_slots) {
    if (slot.pool) {
      vkDestroyQueryPool(device->vk.device, slot.pool, nullptr);
      slot.pool = VK_NULL_HANDLE;
    }
    slot.scopes.clear();
  }
#else
  (void)device;
#endif
  m_enabled = false;
  m_openStack.clear();
}

void RIGpuProfiler::beginFrame(struct RICmd *cmd, uint32_t slot,
                               uint64_t timelineValue) {
  m_openStack.clear();
  if (!m_enabled || slot >= m_slots.size())
    return;
  m_activeSlot = slot;
#if (DEVICE_IMPL_VULKAN)
  Slot &s = m_slots[slot];
  // The pool must be reset on the device timeline; vkCmdResetQueryPool records
  // into the primary CB before any timestamp write this frame.
  vkCmdResetQueryPool(cmd->vk.cmd, s.pool, 0, kMaxQueries);
  s.queryCount = 0;
  s.scopes.clear();
  s.timelineValue = timelineValue;
  s.resolved = false;
#else
  (void)cmd;
  (void)timelineValue;
#endif
}

void RIGpuProfiler::beginScope(struct RICmd *cmd, const char *name) {
#if (DEVICE_IMPL_VULKAN)
  // Debug label is emitted regardless of timestamp support so RGP/Nsight
  // captures stay named even if the queue can't time.
  if (vkCmdBeginDebugUtilsLabelEXT) {
    VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(cmd->vk.cmd, &label);
  }
#endif
  if (!m_enabled)
    return;

  Slot &s = m_slots[m_activeSlot];
  const uint32_t depth = (uint32_t)m_openStack.size();
  uint32_t beginIdx = UINT32_MAX;
#if (DEVICE_IMPL_VULKAN)
  if (s.queryCount < kMaxQueries) {
    beginIdx = s.queryCount++;
    vkCmdWriteTimestamp2(cmd->vk.cmd, kTimestampStage, s.pool, beginIdx);
  }
#endif
  s.scopes.push_back(Scope{name, depth, beginIdx, UINT32_MAX});
  m_openStack.push_back((uint32_t)(s.scopes.size() - 1));
}

void RIGpuProfiler::endScope(struct RICmd *cmd) {
  if (m_enabled && !m_openStack.empty()) {
    Slot &s = m_slots[m_activeSlot];
    const uint32_t idx = m_openStack.back();
    m_openStack.pop_back();
    Scope &sc = s.scopes[idx];
#if (DEVICE_IMPL_VULKAN)
    if (sc.beginIdx != UINT32_MAX && s.queryCount < kMaxQueries) {
      sc.endIdx = s.queryCount++;
      vkCmdWriteTimestamp2(cmd->vk.cmd, kTimestampStage, s.pool, sc.endIdx);
    }
#endif
  }
#if (DEVICE_IMPL_VULKAN)
  if (vkCmdEndDebugUtilsLabelEXT)
    vkCmdEndDebugUtilsLabelEXT(cmd->vk.cmd);
#else
  (void)cmd;
#endif
}

void RIGpuProfiler::resolve(struct RIDevice *device,
                            uint64_t completedTimeline) {
  if (!m_enabled)
    return;
#if (DEVICE_IMPL_VULKAN)
  // Resolve eligible slots in ascending timeline order so the most recent
  // completed frame's numbers are the ones left in m_lastResults.
  uint64_t bestValue = 0;
  for (auto &slot : m_slots) {
    if (slot.resolved || slot.timelineValue == 0)
      continue;
    if (slot.timelineValue > completedTimeline)
      continue; // GPU hasn't finished this frame yet — try again next call

    slot.resolved = true;

    if (slot.queryCount == 0)
      continue;

    uint64_t data[kMaxQueries] = {};
    const VkResult r = vkGetQueryPoolResults(
        device->vk.device, slot.pool, 0, slot.queryCount,
        sizeof(uint64_t) * slot.queryCount, data, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS)
      continue; // VK_NOT_READY shouldn't happen (timeline-gated); skip if it does

    if (slot.timelineValue < bestValue)
      continue; // an already-processed slot was newer; keep its results
    bestValue = slot.timelineValue;

    m_lastResults.clear();
    m_lastResults.reserve(slot.scopes.size());
    float total = 0.0f;
    for (const Scope &sc : slot.scopes) {
      float ms = 0.0f;
      if (sc.beginIdx != UINT32_MAX && sc.endIdx != UINT32_MAX) {
        const uint64_t b = data[sc.beginIdx] & m_validBitsMask;
        const uint64_t e = data[sc.endIdx] & m_validBitsMask;
        if (e >= b)
          ms = (float)((double)(e - b) * m_ticksToMs);
      }
      m_lastResults.push_back(GpuPassTiming{sc.name, ms, sc.depth});
      if (sc.depth == 0)
        total += ms;
    }
    m_lastTotalMs = total;
  }
#else
  (void)device;
  (void)completedTimeline;
#endif
}

} // namespace hpl
