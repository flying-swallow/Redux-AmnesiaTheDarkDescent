#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RISwapchain.h"
#include "graphics/RIVK.h"

#include <algorithm>
#include <optional>

namespace hpl {

RIBootstrap RI = RIBootstrap{};

void RIBootstrap::IncrementFrame() { frameIndex++; }

RIDescriptor RIBootstrap::whiteTexture2DDescriptor() {
  return RIDescriptor::sampledImage(&device, &whiteTexture2DView);
}

namespace {
// Shared grow-and-recreate for the per-frame scratch buffers (mirrors the
// GuiSet gui*Alloc blocks / DebugDraw::requestSegments): try to claim a
// segment; on miss, grow the allocator ×1.5 until the request fits, recreate
// the host-mapped buffer (old one parked on the active set's freelist so
// in-flight frames keep their data) and claim from the fresh allocator.
bool __RequestScratchSegment(RIBootstrap::FrameContext *cntx,
                             RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> &alloc,
                             RISharedPointer<RIBuffer> &buffer, uint16_t elementStride,
                             uint32_t usage, size_t numElements,
                             struct RISegmentReq *req) {
  if (!buffer.isEmpty() &&
      alloc.request(RI.frameIndex, numElements, req)) {
    return true;
  }

  struct RISegmentAllocDesc segmentAllocDesc = {0};
  segmentAllocDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
  segmentAllocDesc.elementStride = elementStride;
  segmentAllocDesc.maxElements = static_cast<uint32_t>(std::max<size_t>(alloc.maxElements, 4096));
  do {
    segmentAllocDesc.maxElements =
        segmentAllocDesc.maxElements + (segmentAllocDesc.maxElements >> 1);
  } while (segmentAllocDesc.maxElements < numElements);
  alloc = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&segmentAllocDesc);
  if (!alloc.request(RI.frameIndex, numElements, req)) {
    assert(false);
    return false;
  }

  if (!buffer.isEmpty()) {
    RI.graphicsDefer.push(buffer);
  }
  buffer = RISharedPointer<RIBuffer>(
      &RI.device,
      RIBuffer::create(
          &RI.device, {(uint64_t)segmentAllocDesc.maxElements *
                           segmentAllocDesc.elementStride,
                       usage, RI_MEMORY_HOST_UPLOAD, 0}));
  return true;
}
} // namespace

bool RIBootstrap::RequestTranslucentVtx(FrameContext *cntx, size_t numFloats,
                                        struct RISegmentReq *req) {
  // SHADER_DEVICE_ADDRESS: the hybrid ParticlePass pulls these streams via BDA
  // (segment base address + byte offset fanned into the bindless slot mirrors).
  return __RequestScratchSegment(cntx, translucentVtxAlloc,
                                 translucentVtxBuffer, sizeof(float),
                                 RI_BUFFER_USAGE_VERTEX_BUFFER |
                                     RI_BUFFER_USAGE_DEVICE_ADDRESS,
                                 numFloats, req);
}

bool RIBootstrap::RequestTranslucentIdx(FrameContext *cntx, size_t numIndices,
                                        struct RISegmentReq *req) {
  return __RequestScratchSegment(cntx, translucentIdxAlloc,
                                 translucentIdxBuffer, sizeof(uint32_t),
                                 RI_BUFFER_USAGE_INDEX_BUFFER |
                                     RI_BUFFER_USAGE_DEVICE_ADDRESS,
                                 numIndices, req);
}

void RIBootstrap::Dispose() {
  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);

  translucentVtxBuffer = {};
  translucentIdxBuffer = {};

  // Free the owned samplers (cookie != 0 marks an occupied slot).
  for (size_t i = 0; i < cachedSamplers.size(); i++) {
    if (cachedSamplers[i].cookie) {
      cachedSamplers[i].dispose(&device);
      cachedSamplers[i] = RISampler{};
    }
  }

  // The GPU is idle (waitIdle above), so every sealed frame has completed and
  // anything still pending was deferred for a frame that never submitted
  // (teardown mid-frame). Release all of it, then drop the timeline itself.
  graphicsDefer.drainAll();
  profiler.dispose(&device);
  graphicsTimeline.dispose(&device);

  for (auto &set : frameSets) {
    FreeRIScratchAlloc(&device, &set.uboScratchAlloc);
    FreeRIScratchAlloc(&device, &set.accelScratchAlloc);
  }

  graphicsCmdRing.dispose(&device);
}

void RIBootstrap::CloseAndSubmitActiveSet() {
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
  struct RIQueue *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];
  {
    // Swapchain image: COLOR -> PRESENT for the queue present. The swapchain
    // images are raw VkImage handles, so bridge through a stack RITexture.
    RITexture swapchainTexture = {};
    swapchainTexture.vk.image = RI.swapchain.vk.images[RI.swapchainIndex];

    RITextureBarrier toPresent = {};
    toPresent.texture = &swapchainTexture;
    toPresent.before = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    toPresent.after = RI_RESOURCE_STATE_PRESENT;
    RI.primary.cmds[0].vk_d3d12_textureBarrier(toPresent);
  }
  RI.primary.cmds[0].end(&RI.device);
  RI.blasSubmit.cmds[0].end(&RI.device);

  // Flush pending resource uploads (the vertex/index data the BLAS builds read)
  // once, up front, so both the BLAS submit and the primary chain off it.
  RIResourceUploaderVKResult uploadResult =
      RI_VKFlushResourceUpdate(&RI.device, &RI.uploader, 0, NULL);

  // Submit the dedicated BLAS-build command buffer ahead of the primary. It
  // waits on the uploader (so the builds see uploaded geometry) and signals its
  // own semaphore; the primary submit below waits on that, so every BLAS is
  // fully built before the primary's TLAS build runs — no inline accel-build
  // barrier needed. Submitted even when it recorded no builds so the semaphore
  // signals. The upload→blas→primary chain also makes uploads visible to the
  // primary, so the (binary) uploader semaphore is waited by exactly one
  // consumer (here).
  {
    VkCommandBufferSubmitInfo blasCmd = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    blasCmd.commandBuffer = RI.blasSubmit.cmds[0].vk.cmd;

    VkSemaphoreSubmitInfo blasWait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    blasWait.semaphore = uploadResult.vk.semaphore;
    blasWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphoreSubmitInfo blasSignal = {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    blasSignal.semaphore = RI.blasSubmit.vk.semaphore;
    blasSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 blasSubmitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    blasSubmitInfo.commandBufferInfoCount = 1;
    blasSubmitInfo.pCommandBufferInfos = &blasCmd;
    blasSubmitInfo.waitSemaphoreInfoCount = uploadResult.signaled ? 1u : 0u;
    blasSubmitInfo.pWaitSemaphoreInfos = &blasWait;
    blasSubmitInfo.signalSemaphoreInfoCount = 1;
    blasSubmitInfo.pSignalSemaphoreInfos = &blasSignal;

    VK_WrapResult(
        vkResetFences(RI.device.vk.device, 1, &RI.blasSubmit.vk.fence));
    VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &blasSubmitInfo,
                                 RI.blasSubmit.vk.fence));
  }
  {
    VkCommandBufferSubmitInfo cmdSubmitInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdSubmitInfo.commandBuffer = RI.primary.cmds[0].vk.cmd;

    VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
    submitInfo.commandBufferInfoCount = 1;

    // The graphics submit transitions the swapchain image's layout, so it must
    // wait on the acquire semaphore before touching the image. It also waits on
    // the BLAS submit's semaphore (which chained off the uploader), so the TLAS
    // build sees built BLAS and every pass sees uploaded geometry. Signals
    // finishSem so present waits on the rendering work.
    VkSemaphoreSubmitInfo waitInfos[2] = {};
    uint32_t waitCount = 0;
    waitInfos[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfos[waitCount].semaphore =
        RI.swapchain.vk.imageAcquireSem[RI.swapchain.vk.frameIndex];
    waitInfos[waitCount].stageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waitCount++;
    waitInfos[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfos[waitCount].semaphore = RI.blasSubmit.vk.semaphore;
    waitInfos[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    waitCount++;
    submitInfo.waitSemaphoreInfoCount = waitCount;
    submitInfo.pWaitSemaphoreInfos = waitInfos;

    // Reserve this frame's timeline value. The primary signals it on
    // completion; everything parked on the active set is latched to it below so
    // it's reclaimed exactly when the GPU finishes this frame.
    const uint64_t frameTimelineValue = RI.graphicsTimeline.next();

    VkSemaphoreSubmitInfo signalInfos[2] = {};
    // Binary finish semaphore for present.
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore =
        RI.swapchain.vk.finishSem[RI.swapchain.vk.frameIndex];
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // Monotonic timeline drives deferred resource reclamation.
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = RI.graphicsTimeline.vk.semaphore;
    signalInfos[1].value = frameTimelineValue;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    submitInfo.signalSemaphoreInfoCount = 2;
    submitInfo.pSignalSemaphoreInfos = signalInfos;

    VK_WrapResult(vkResetFences(RI.device.vk.device, 1, &RI.primary.vk.fence));
    VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &submitInfo,
                                 RI.primary.vk.fence));
    RISwapchainPresent(&RI.device, &RI.swapchain);

    // Seal this frame's deferred destroys + keep-alives against the timeline
    // value the primary signals; they release in BeginActiveSet once the GPU
    // passes it. BLAS work lives in blasSubmit, which the primary waits on, so
    // primary completion implies every parked resource is GPU-idle.
    RI.graphicsDefer.seal(frameTimelineValue);
  }
  RI.IncrementFrame();
}
void RIBootstrap::BeginActiveSet() {
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();

  RI.graphicsCmdRing.advance();
  RI.primary = RI.graphicsCmdRing.acquire(&RI.device, 1);
  RI.blasSubmit = RI.graphicsCmdRing.acquire(&RI.device, 1);
  RI.primary.wait(&RI.device);
  RI.blasSubmit.wait(&RI.device);
  RI.primary.pool->reset(&RI.device);

  const uint64_t completedTimeline = RI.graphicsTimeline.completed(&RI.device);
  RI.graphicsDefer.drain(completedTimeline);
  // Read back any GPU timing slot whose frame has finished (uses the same
  // completed value as the deferral drain), before this frame overwrites a slot.
  RI.profiler.resolve(&RI.device, completedTimeline);

  RI.swapchainIndex = RISwapchainAcquireNextTexture(&RI.device, &RI.swapchain);

  // cleanup
  RIResetScratchAlloc(&RI.device, &cntx->uboScratchAlloc);
  RIResetScratchAlloc(&RI.device, &cntx->accelScratchAlloc);

  RI.blasSubmit.cmds[0].begin(&RI.device);
  RI.primary.cmds[0].begin(&RI.device);
  {
    // Swapchain image: UNDEFINED -> COLOR for the frame. (Depth is
    // per-viewport now — each renderer's Draw emits its own first-use
    // UNDEFINED transition on its viewport's depth target.)
    RITexture swapchainTexture = {};
    swapchainTexture.vk.image = RI.swapchain.vk.images[RI.swapchainIndex];

    RITextureBarrier toColor = {};
    toColor.texture = &swapchainTexture;
    toColor.before = RI_RESOURCE_STATE_UNDEFINED;
    toColor.after = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    RI.primary.cmds[0].vk_d3d12_textureBarrier(toColor);
  }

  // Reset this frame's GPU-timing query pool on the primary CB (must be outside
  // any dynamic-rendering scope — true here, before any pass records). The value
  // passed is the timeline value CloseAndSubmitActiveSet will reserve via next()
  // (== pending() + 1), so resolve() can gate readback on it.
  RI.profiler.beginFrame(&RI.primary.cmds[0],
                         RI.frameIndex % RI_NUMBER_FRAMES_FLIGHT,
                         RI.graphicsTimeline.pending() + 1);
}

void RIBootstrap::UpdateFrameUBO(RIDescriptor *descriptor, void *data,
                                 size_t size) {
  auto *activeSet = GetActiveSet();
  struct RIBufferScratchAllocReq scratchReq = RIAllocBufferFromScratchAlloc(
      &device, &activeSet->uboScratchAlloc, size);
  memcpy((uint8_t *)scratchReq.pMappedAddress + scratchReq.bufferOffset, data,
         size);
  // The descriptor is a transient shim produced here: its cookie derives from
  // the scratch buffer identity folded with the sub-allocation offset/range, so
  // each slice is a distinct, stable descriptor-set cache key.
  *descriptor = RIDescriptor::uniformBuffer(
      &device, &scratchReq.block.buffer, scratchReq.bufferOffset, size);
  RIFinishScrachReq(&device, &scratchReq);
}

std::optional<RIDescriptor> RIBootstrap::resolve_filter_descriptor(eTextureWrap wrapS,
                                                       eTextureWrap wrapT,
                                                       eTextureWrap wrapR,
                                                       eTextureFilter filter) {
#if (DEVICE_IMPL_VULKAN)
  {
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.addressModeU = RI_VK_TextureWrap(wrapS);
    info.addressModeV = RI_VK_TextureWrap(wrapT);
    info.addressModeW = RI_VK_TextureWrap(wrapR);
    switch (filter) {
    case eTextureFilter_Nearest:
      info.minFilter = VK_FILTER_NEAREST;
      info.magFilter = VK_FILTER_NEAREST;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      break;
    case eTextureFilter_Bilinear:
      info.minFilter = VK_FILTER_LINEAR;
      info.magFilter = VK_FILTER_LINEAR;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      break;
    case eTextureFilter_Trilinear:
      info.minFilter = VK_FILTER_LINEAR;
      info.magFilter = VK_FILTER_LINEAR;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      break;
    case eTextureFilter_LastEnum:
      assert(false);
      break;
    }
    info.maxLod = 16;
    constexpr uint32_t kWrapCount = static_cast<uint32_t>(eTextureWrap_LastEnum);
    constexpr uint32_t kFilterCount = static_cast<uint32_t>(eTextureFilter_LastEnum);

    const uint32_t wrapSIndex = static_cast<uint32_t>(wrapS);
    const uint32_t wrapTIndex = static_cast<uint32_t>(wrapT);
    const uint32_t wrapRIndex = static_cast<uint32_t>(wrapR);
    const uint32_t filterIndex = static_cast<uint32_t>(filter);

    if (wrapSIndex >= kWrapCount || wrapTIndex >= kWrapCount || wrapRIndex >= kWrapCount || filterIndex >= kFilterCount) {
        assert(false && "Invalid sampler configuration");
        return std::nullopt;
    }

    // Collision-free mixed-radix key in the range [0, 191].
    const size_t cacheIndex = (((static_cast<size_t>(wrapSIndex) * kWrapCount + wrapTIndex) * kWrapCount + wrapRIndex) * kFilterCount + filterIndex);
    assert(cacheIndex < cachedSamplers.size());
    RISampler& sampler = cachedSamplers[cacheIndex];

    // cookie == 0 means the slot has not been created.
    // Add one because cacheIndex itself can be zero.
    const hash_t samplerCookie = static_cast<hash_t>(cacheIndex) + 1;

    if (sampler.cookie == 0) {
        VK_WrapResult(vkCreateSampler(device.vk.device, &info, nullptr, &sampler.vk.sampler));
        sampler.cookie = samplerCookie;
    }
    else {
        // Direct indexing guarantees this slot belongs to this configuration.
        assert(sampler.cookie == samplerCookie);
    }

    return RIDescriptor::sampler(&device, &sampler);
  }
#endif
  return std::nullopt;
}

} // namespace hpl
