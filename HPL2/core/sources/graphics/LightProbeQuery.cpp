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

#include "graphics/LightProbeQuery.h"

#include "graphics/RIBarrier.h"
#include "graphics/RICommand.h"
#include "graphics/RIRenderer.h" // RIDevice::vk.vmaAllocator (readback invalidate)
#include "scene/Light.h"

#include <cstring>

// The shared host/shader header; on a C++ translation unit it emits the
// SHARED_CONSTs as real constants, which is how the static_asserts below
// compare the CPU mirror against the shader's own values.
#include "Constants.h"

namespace hpl {

//------------------------------------------------------------------------

static_assert(cLightProbeQuery::kMaxProbes == (int)kMaxLightProbes,
              "kMaxProbes must match kMaxLightProbes in amnesia/slang/Constants.h");
static_assert(cLightProbeQuery::kMaxExcludes == (int)kMaxLightProbeExcludes,
              "kMaxExcludes must match kMaxLightProbeExcludes in amnesia/slang/Constants.h");

// Scalar layout: probeCount, excludeCount, then kMaxExcludes ids, all uint.
static_assert(sizeof(cLightProbeQuery::cPushConstants) ==
                  sizeof(uint32_t) * (2 + cLightProbeQuery::kMaxExcludes),
              "cPushConstants must match LightProbePC in LightProbePass.cs.slang");

// packLightId in Scene.slang: top 2 bits type, low 30 bits the light's stable
// per-type GPU slot. Mirrored here so the exclusion list the probe compares
// against is built from the same identity the light grid stores.
static const uint32_t kLightSlotBits = 30u;
static const uint32_t kLightSlotMask = (1u << kLightSlotBits) - 1u;

static uint32_t PackLightId(uint32_t alType, uint32_t alSlot) {
  return (alType << kLightSlotBits) | (alSlot & kLightSlotMask);
}

//------------------------------------------------------------------------

cLightProbeQuery::cLightProbeQuery() {}

cLightProbeQuery::~cLightProbeQuery() {
  // Buffers must have been released through Dispose while the device was still
  // alive; nothing to do here.
}

//------------------------------------------------------------------------

void cLightProbeQuery::Init(RIDevice *apDevice) {
  if (mbInitialized || apDevice == NULL)
    return;

  const uint64_t lRequestSize = sizeof(cGpuRequest) * (uint64_t)kMaxProbes;
  const uint64_t lResultSize = sizeof(cGpuResult) * (uint64_t)kMaxProbes;

  for (size_t i = 0; i < mvSlots.size(); ++i) {
    cFrameSlot &slot = mvSlots[i];

    RIBufferDesc reqDesc = {};
    reqDesc.size = lRequestSize;
    reqDesc.usage = RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE;
    reqDesc.location = RI_MEMORY_HOST_UPLOAD;
    slot.mRequests = RIBuffer::create(apDevice, reqDesc);

    RIBufferDesc resDesc = {};
    resDesc.size = lResultSize;
    resDesc.usage =
        RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_SRC;
    resDesc.location = RI_MEMORY_DEVICE;
    slot.mResults = RIBuffer::create(apDevice, resDesc);

    RIBufferDesc backDesc = {};
    backDesc.size = lResultSize;
    backDesc.usage = RI_BUFFER_USAGE_TRANSFER_DST;
    backDesc.location = RI_MEMORY_HOST_READBACK;
    slot.mReadback = RIBuffer::create(apDevice, backDesc);

    // A failed allocation leaves the whole service off rather than half-armed:
    // every entry point checks mbInitialized, and GetResult then reports "no
    // answer", which every caller already has to handle.
    if (slot.mRequests.isEmpty() || slot.mResults.isEmpty() ||
        slot.mReadback.isEmpty()) {
      Dispose(apDevice);
      return;
    }

    slot.mRequests.setDebugObjectName(apDevice, "LightProbe.Requests");
    slot.mResults.setDebugObjectName(apDevice, "LightProbe.Results");
    slot.mReadback.setDebugObjectName(apDevice, "LightProbe.Readback");
  }

  mbInitialized = true;
}

//------------------------------------------------------------------------

void cLightProbeQuery::Dispose(RIDevice *apDevice) {
  if (apDevice == NULL)
    return;

  for (size_t i = 0; i < mvSlots.size(); ++i) {
    cFrameSlot &slot = mvSlots[i];
    slot.mRequests.dispose(apDevice);
    slot.mResults.dispose(apDevice);
    slot.mReadback.dispose(apDevice);
    slot.mlStageValue = 0;
    slot.mlProbeCount = 0;
  }
  mbInitialized = false;
  Reset();
}

//------------------------------------------------------------------------

void cLightProbeQuery::SetProbes(const cVector3f *apPositions,
                                 int alCount) {
  if (apPositions == NULL)
    alCount = 0;
  if (alCount < 0)
    alCount = 0;
  if (alCount > kMaxProbes)
    alCount = kMaxProbes;

  for (int i = 0; i < alCount; ++i) {
    mvRequests[i].mvPos[0] = apPositions[i].x;
    mvRequests[i].mvPos[1] = apPositions[i].y;
    mvRequests[i].mvPos[2] = apPositions[i].z;
    mvRequests[i].mfPadding = 0.0f;
  }
  mlProbeCount = alCount;
}

//------------------------------------------------------------------------

void cLightProbeQuery::SetExcludedLights(iLight **apLights, int alCount) {
  mlExcludeCount = 0;
  if (apLights == NULL)
    return;

  for (int i = 0; i < alCount && mlExcludeCount < kMaxExcludes; ++i) {
    iLight *pLight = apLights[i];
    if (pLight == NULL)
      continue;

    // A light that has never been uploaded has no slot, so there is no id for
    // the probe to compare against — and nothing in the grid to exclude either.
    const uint32_t lSlot = pLight->GetGpuLightSlot();
    if (lSlot == UINT32_MAX)
      continue;

    uint32_t lType;
    switch (pLight->GetLightType()) {
    case eLightType_Point:
      lType = 0u;
      break;
    case eLightType_Spot:
      lType = 1u;
      break;
    case eLightType_Area:
      lType = 2u;
      break;
    default:
      continue; // box lights are never uploaded, so never binned
    }

    mvExcludeIds[mlExcludeCount++] = PackLightId(lType, lSlot);
  }
}

//------------------------------------------------------------------------

bool cLightProbeQuery::GetResult(int alIndex, cVector3f &avIrradiance) const {
  if (!mbHasResult || alIndex < 0 || alIndex >= mlResultCount)
    return false;

  const cGpuResult &res = mvResults[alIndex];
  avIrradiance = cVector3f(res.mvIrradiance[0], res.mvIrradiance[1],
                           res.mvIrradiance[2]);
  return true;
}

//------------------------------------------------------------------------

void cLightProbeQuery::Reset() {
  mbHasResult = false;
  mlResultCount = 0;
  for (size_t i = 0; i < mvSlots.size(); ++i) {
    // Drop the in-flight copies too. Their results describe the old world; the
    // buffers themselves stay and are simply overwritten by the next frame.
    mvSlots[i].mlStageValue = 0;
    mvSlots[i].mlProbeCount = 0;
  }
}

//------------------------------------------------------------------------

void cLightProbeQuery::Poll(RIDevice *apDevice,
                            uint64_t alCompletedTimelineValue) {
  if (!mbInitialized || apDevice == NULL)
    return;

  // Harvest every slot whose copy has executed, newest last so the freshest
  // answer is the one left in mvResults.
  uint64_t lNewest = 0;
  for (size_t i = 0; i < mvSlots.size(); ++i) {
    cFrameSlot &slot = mvSlots[i];
    if (slot.mlStageValue == 0 || slot.mlStageValue > alCompletedTimelineValue)
      continue;

    if (slot.mlStageValue >= lNewest && slot.mReadback.mappedAddress != NULL) {
      lNewest = slot.mlStageValue;
      mlResultCount = slot.mlProbeCount;
      if (mlResultCount > 0) {
#if (DEVICE_IMPL_VULKAN)
        // The readback allocation may have landed in HOST_CACHED memory, where
        // the mapped pointer alone would hand back stale cache lines.
        vmaInvalidateAllocation(apDevice->vk.vmaAllocator,
                                slot.mReadback.vk.allocation, 0, VK_WHOLE_SIZE);
#endif
        std::memcpy(mvResults.data(), slot.mReadback.mappedAddress,
                    sizeof(cGpuResult) * (size_t)mlResultCount);
        mbHasResult = true;
      }
    }
    slot.mlStageValue = 0;
  }
}

//------------------------------------------------------------------------

bool cLightProbeQuery::BeginFrame(uint32_t alFrameIndex) {
  if (!mbInitialized || mlProbeCount <= 0)
    return false;

  cFrameSlot &slot = mvSlots[alFrameIndex % RI_NUMBER_FRAMES_FLIGHT];
  if (slot.mRequests.mappedAddress == NULL)
    return false;

  // Overwriting this slot is safe: the frame that last used it is the one
  // RI_NUMBER_FRAMES_FLIGHT frames back, which the frame fence has already
  // retired before the engine handed us this frame index.
  std::memcpy(slot.mRequests.mappedAddress, mvRequests.data(),
              sizeof(cGpuRequest) * (size_t)mlProbeCount);
  slot.mlProbeCount = mlProbeCount;
  return true;
}

//------------------------------------------------------------------------

RIBuffer *cLightProbeQuery::GetRequestBuffer(uint32_t alFrameIndex) {
  if (!mbInitialized)
    return NULL;
  return &mvSlots[alFrameIndex % RI_NUMBER_FRAMES_FLIGHT].mRequests;
}

RIBuffer *cLightProbeQuery::GetResultBuffer(uint32_t alFrameIndex) {
  if (!mbInitialized)
    return NULL;
  return &mvSlots[alFrameIndex % RI_NUMBER_FRAMES_FLIGHT].mResults;
}

uint64_t cLightProbeQuery::GetRequestBufferRange() const {
  return sizeof(cGpuRequest) * (uint64_t)kMaxProbes;
}

uint64_t cLightProbeQuery::GetResultBufferRange() const {
  return sizeof(cGpuResult) * (uint64_t)kMaxProbes;
}

//------------------------------------------------------------------------

cLightProbeQuery::cPushConstants cLightProbeQuery::GetPushConstants() const {
  cPushConstants push = {};
  push.mlProbeCount = (uint32_t)mlProbeCount;
  push.mlExcludeCount = (uint32_t)mlExcludeCount;
  for (int i = 0; i < kMaxExcludes; ++i)
    push.mvExcludeIds[i] = (i < mlExcludeCount) ? mvExcludeIds[i] : 0u;
  return push;
}

//------------------------------------------------------------------------

void cLightProbeQuery::RecordReadback(RIDevice *apDevice, RICmd *apCmd,
                                      uint32_t alFrameIndex,
                                      uint64_t alPendingTimelineValue) {
  if (!mbInitialized || apDevice == NULL || apCmd == NULL)
    return;

  cFrameSlot &slot = mvSlots[alFrameIndex % RI_NUMBER_FRAMES_FLIGHT];
  if (slot.mlProbeCount <= 0)
    return;

  const uint64_t lSize = sizeof(cGpuResult) * (uint64_t)slot.mlProbeCount;

  apCmd->vk_d3d12_bufferBarrier(RIBufferBarrier(
      &slot.mResults, RI_RESOURCE_STATE_STORAGE_WRITE,
      RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_COMPUTE, RI_STAGE_COPY));

  apCmd->copyBuffer(apDevice, &slot.mResults, 0, &slot.mReadback, 0, lSize);

  // The copy is part of the frame currently being recorded, so it has executed
  // once the graphics timeline passes the value that frame's submit signals.
  slot.mlStageValue = alPendingTimelineValue;
}

//------------------------------------------------------------------------

} // namespace hpl
