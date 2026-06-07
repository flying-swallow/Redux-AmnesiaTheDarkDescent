#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RISwapchain.h"
#include "graphics/RIVK.h"

namespace hpl {

RIBootstrap RI = RIBootstrap{};

void RIBootstrap::IncrementFrame() { frameIndex++; }

void RIBootstrap::Dispose() {
  device.queues[RI_QUEUE_GRAPHICS].waitIdle(&device);

  for (auto &desc : cachedFilters) {
    if (desc.cookie) {
      desc.dispose(&device);
    }
  }

  for (auto &set : frameSets) {
    set.resourceLink.clear();
    for (auto &entry : set.freelist) {
      std::visit([&](auto &res) { res.dispose(&device); }, entry);
    }
    set.freelist.clear();
    FreeRIScratchAlloc(&device, &set.uboScratchAlloc);
    FreeRIScratchAlloc(&device, &set.accelScratchAlloc);
  }

  graphicsCmdRing.dispose(&device);
}

void RIBootstrap::CloseAndSubmitActiveSet() {
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
  struct RIQueue_s *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];
  {
    // Swapchain image: COLOR -> PRESENT for the queue present. The swapchain
    // images are raw VkImage handles, so bridge through a stack RITexture_s.
    RITexture_s swapchainTexture = {};
    swapchainTexture.vk.image = RI.swapchain.vk.images[RI.swapchainIndex];

    RITextureBarrier_s toPresent = {};
    toPresent.texture = &swapchainTexture;
    toPresent.before = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    toPresent.after = RI_RESOURCE_STATE_PRESENT;
    RI.primary.cmds[0].textureBarrier(toPresent);
  }
  RI.primary.cmds[0].end(&RI.device);
  RI.blasSubmit.cmds[0].end(&RI.device);

  // Flush pending resource uploads (the vertex/index data the BLAS builds read)
  // once, up front, so both the BLAS submit and the primary chain off it.
  RIResourceUploaderVKResult_s uploadResult =
      RI_VKFlushResourceUpdate(&RI.device, &RI.uploader, 0, NULL);

  // Submit the dedicated BLAS-build command buffer ahead of the primary. It waits
  // on the uploader (so the builds see uploaded geometry) and signals its own
  // semaphore; the primary submit below waits on that, so every BLAS is fully
  // built before the primary's TLAS build runs — no inline accel-build barrier
  // needed. Submitted even when it recorded no builds so the semaphore signals.
  // The upload→blas→primary chain also makes uploads visible to the primary, so
  // the (binary) uploader semaphore is waited by exactly one consumer (here).
  {
    VkCommandBufferSubmitInfo blasCmd = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    blasCmd.commandBuffer = RI.blasSubmit.cmds[0].vk.cmd;

    VkSemaphoreSubmitInfo blasWait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    blasWait.semaphore = uploadResult.vk.semaphore;
    blasWait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphoreSubmitInfo blasSignal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
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

    VkSemaphoreSubmitInfo signalFinish = {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalFinish.semaphore =
        RI.swapchain.vk.finishSem[RI.swapchain.vk.frameIndex];
    signalFinish.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalFinish;

    VK_WrapResult(vkResetFences(RI.device.vk.device, 1, &RI.primary.vk.fence));
    VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &submitInfo,
                                 RI.primary.vk.fence));
    RISwapchainPresent(&RI.device, &RI.swapchain);
  }
  RI.IncrementFrame();
}
void RIBootstrap::BeginActiveSet() {
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();

  RI.graphicsCmdRing.advance();
  RI.primary = RI.graphicsCmdRing.acquire(&RI.device, 1);
  // Second element from the same pool for the dedicated BLAS-build submit.
  RI.blasSubmit = RI.graphicsCmdRing.acquire(&RI.device, 1);
  RI.primary.wait(&RI.device);
  RI.blasSubmit.wait(&RI.device);
  // One pool backs both elements; resetting it once frees both command buffers.
  RI.primary.pool->reset(&RI.device);

  for (auto &entry : cntx->freelist) {
    std::visit([&](auto &res) { res.dispose(&RI.device); }, entry);
  }
  cntx->freelist.clear();

  RI.swapchainIndex = RISwapchainAcquireNextTexture(&RI.device, &RI.swapchain);

  // cleanup
  RIResetScratchAlloc(&RI.device, &cntx->uboScratchAlloc);
  RIResetScratchAlloc(&RI.device, &cntx->accelScratchAlloc);
  // cntx->colorAttachment = RI.colorAttachment[RI.swapchainIndex];
  // cntx->colorAttachment.finalize(&RI.device);
  // Drop the keep-alive refs parked last time this slot was used. BLAS
  // handles ride here too, deferring their release by frames-in-flight
  // alongside the storage/vertex/index buffers — the Draw path re-parks
  // every BLAS-backed geometry each frame, so a BLAS stays resident as long
  // as it (or a TLAS that references it) can be in flight.
  cntx->resourceLink.clear();

  RI.blasSubmit.cmds[0].begin(&RI.device);
  RI.primary.cmds[0].begin(&RI.device);
  {
    // Swapchain image: UNDEFINED -> COLOR for the frame. (Depth is
    // per-viewport now — each renderer's Draw emits its own first-use
    // UNDEFINED transition on its viewport's depth target.)
    RITexture_s swapchainTexture = {};
    swapchainTexture.vk.image = RI.swapchain.vk.images[RI.swapchainIndex];

    RITextureBarrier_s toColor = {};
    toColor.texture = &swapchainTexture;
    toColor.before = RI_RESOURCE_STATE_UNDEFINED;
    toColor.after = RI_RESOURCE_STATE_RENDER_TARGET_READ;
    RI.primary.cmds[0].textureBarrier(toColor);
  }
}

void RIBootstrap::UpdateFrameUBO(RIDescriptor_s *descriptor, void *data,
                                 size_t size) {
  auto *activeSet = GetActiveSet();
  const hash_t hash =
      hash_data_hsieh(HASH_INITIAL_VALUE + frameIndex, data, size);
  if (descriptor->cookie != hash) {
    descriptor->cookie = hash;
    struct RIBufferScratchAllocReq_s scratchReq = RIAllocBufferFromScratchAlloc(
        &device, &activeSet->uboScratchAlloc, size);
    descriptor->vk.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor->vk.buffer.buffer = scratchReq.block.buffer.vk.buffer;
    descriptor->vk.buffer.offset = scratchReq.bufferOffset;
    descriptor->vk.buffer.range = size;
    memcpy((uint8_t *)scratchReq.pMappedAddress + scratchReq.bufferOffset, data,
           size);
    RIFinishScrachReq(&device, &scratchReq);
  }
}

RIDescriptor_s *RIBootstrap::resolve_filter_descriptor(eTextureWrap wrapS,
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
    const hash_t hash =
        hash_data(HASH_INITIAL_VALUE, &info, sizeof(VkSamplerCreateInfo));
    const size_t startIndex = (hash % cachedFilters.size());
    size_t index = startIndex;
    do {
      if (cachedFilters[index].cookie == hash) {
        return &cachedFilters[index];
      } else if (cachedFilters[index].isEmpty()) {
        cachedFilters[index].vk.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        cachedFilters[index].vk.image.imageView = VK_NULL_HANDLE;
        cachedFilters[index].vk.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        cachedFilters[index].flags = RI_VK_DESC_OWN_SAMPLER;
        VK_WrapResult(vkCreateSampler(device.vk.device, &info, NULL,
                                      &cachedFilters[index].vk.image.sampler));
        cachedFilters[index].finalize(&device);
        cachedFilters[index].cookie = hash;
        return &cachedFilters[index];
      }
      index = (index + 1) % cachedFilters.size();
    } while (index != startIndex);
  }
#endif
  return NULL;
}

} // namespace hpl
