#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RISwapchain.h"
#include "graphics/RIVK.h"

namespace hpl {

RIBootstrap RI = RIBootstrap{};

void RIBootstrap::IncrementFrame() {
  frameIndex++;
}

void RIBootstrap::CloseAndSubmitActiveSet() {
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
  struct RIQueue_s *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];
  {
    VkImageMemoryBarrier2 imageBarriers[1] = {};
    imageBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageBarriers[0].srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    imageBarriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageBarriers[0].dstAccessMask = VK_ACCESS_2_NONE;
    imageBarriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imageBarriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarriers[0].image = cntx->colorAttachment.texture->vk.image;
    imageBarriers[0].subresourceRange = (VkImageSubresourceRange){
        VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
        VK_REMAINING_ARRAY_LAYERS,
    };
    VkDependencyInfo dependencyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = imageBarriers;
    vkCmdPipelineBarrier2(cntx->cmd.vk.cmd, &dependencyInfo);
  }
  {
    // close cmd buffer and submit
    vkEndCommandBuffer(cntx->cmd.vk.cmd);
    {
      VkCommandBufferSubmitInfo cmdSubmitInfo = {
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
      cmdSubmitInfo.commandBuffer = cntx->cmd.vk.cmd;

      VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
      submitInfo.pCommandBufferInfos = &cmdSubmitInfo;
      submitInfo.commandBufferInfoCount = 1;

      RIResourceUploaderVKResult_s uploadResult =
          RI_VKFlushResourceUpdate(&RI.device, &RI.uploader, 0, NULL);
      VkSemaphoreSubmitInfo waitUpload = {
          VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
      if (uploadResult.signaled) {
        waitUpload.semaphore = uploadResult.vk.semaphore;
        waitUpload.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitUpload;
      }
      VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &submitInfo,
                                   VK_NULL_HANDLE));
      RISwapchainPresent(&RI.device, &RI.swapchain);
    }
    {
      VkSemaphoreSubmitInfo signalSem = {
          VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
      signalSem.stageMask = VK_PIPELINE_STAGE_2_NONE;
      signalSem.value = RI.frameIndex + 1;
      signalSem.semaphore = RI.vk.frameSemaphore;
      VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
      submitInfo.pSignalSemaphoreInfos = &signalSem;
      submitInfo.signalSemaphoreInfoCount = 1;

      VK_WrapResult(vkQueueSubmit2(graphicsQueue->vk.queue, 1, &submitInfo,
                                   VK_NULL_HANDLE));
    }
  }
  RI.IncrementFrame();
}
void RIBootstrap::BeginActiveSet() {
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
  struct RIQueue_s *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];

  if (RI.frameIndex >= RI_NUMBER_FRAMES_FLIGHT) {
    const uint64_t waitValue = 1 + RI.frameIndex - RI_NUMBER_FRAMES_FLIGHT;
    VkSemaphoreWaitInfo semaphoreWaitInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    semaphoreWaitInfo.semaphoreCount = 1;
    semaphoreWaitInfo.pSemaphores = &RI.vk.frameSemaphore;
    semaphoreWaitInfo.pValues = &waitValue;
    VK_WrapResult(vkWaitSemaphores(RI.device.vk.device, &semaphoreWaitInfo,
                                   5000 * 1000000ull));
    VK_WrapResult(vkResetCommandPool(RI.device.vk.device, cntx->vk.pool, 0));
  }
  RI.swapchainIndex = RISwapchainAcquireNextTexture(&RI.device, &RI.swapchain);

  // cleanup
  RIResetScratchAlloc(&RI.device, &cntx->uboScratchAlloc);
  cntx->colorAttachment = RI.colorAttachment[RI.swapchainIndex];
  cntx->depthAttachment = RI.depthAttachment[RI.swapchainIndex];
  cntx->textureLink.clear();
  {
    VkCommandBufferBeginInfo info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cntx->cmd.vk.cmd, &info);
  }
  {
    VkImageMemoryBarrier2 imageBarriers[2] = {};
    imageBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageBarriers[0].srcAccessMask = VK_ACCESS_2_NONE;
    imageBarriers[0].dstStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    imageBarriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imageBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarriers[0].image = cntx->colorAttachment.texture->vk.image;
    imageBarriers[0].subresourceRange = (VkImageSubresourceRange){
        VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
        VK_REMAINING_ARRAY_LAYERS,
    };

    imageBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    imageBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageBarriers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    imageBarriers[1].srcAccessMask = VK_ACCESS_2_NONE;
    imageBarriers[1].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    imageBarriers[1].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    imageBarriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    imageBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarriers[1].image = cntx->depthAttachment.texture->vk.image;
    imageBarriers[1].subresourceRange = (VkImageSubresourceRange){
        VK_IMAGE_ASPECT_DEPTH_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
        VK_REMAINING_ARRAY_LAYERS,
    };
    VkDependencyInfo dependencyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.imageMemoryBarrierCount = 2;
    dependencyInfo.pImageMemoryBarriers = imageBarriers;
    vkCmdPipelineBarrier2(cntx->cmd.vk.cmd, &dependencyInfo);
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
    descriptor->vk.buffer.buffer = scratchReq.block.vk.buffer;
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
    info.addressModeV = RI_VK_TextureWrap(wrapR);
    info.addressModeW = RI_VK_TextureWrap(wrapT);
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
    const hash_t hash =
        hash_data(HASH_INITIAL_VALUE, &info, sizeof(VkSamplerCreateInfo));
    const size_t startIndex = (hash % cachedFilters.size());
    size_t index = startIndex;
    do {
      if (cachedFilters[index].cookie == hash) {
        return &cachedFilters[index];
      } else if (RI_IsEmptyDescriptor(&cachedFilters[index])) {
        cachedFilters[index].vk.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        cachedFilters[index].vk.image.imageView = VK_NULL_HANDLE;
        cachedFilters[index].vk.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        cachedFilters[index].flags = RI_VK_DESC_OWN_SAMPLER;
        VK_WrapResult(vkCreateSampler(device.vk.device, &info, NULL,
                                      &cachedFilters[index].vk.image.sampler));
        RIFinalizeDescriptor(&device, &cachedFilters[index]);
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
