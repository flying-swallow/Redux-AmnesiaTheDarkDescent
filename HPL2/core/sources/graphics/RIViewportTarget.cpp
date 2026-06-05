/**
 * Copyright 2026 Michael Pollind
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics/RIViewportTarget.h"

#include "graphics/HPLTexture.h"
#include "graphics/Image.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"

#include "system/LowLevelSystem.h"

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

namespace hpl {

void cViewportTarget::Resize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  if (mpTexture && mlWidth == width && mlHeight == height) {
    return;
  }

#if (DEVICE_IMPL_VULKAN)
  // Same deleter as every manager-created texture: GPU frees are deferred on
  // the frame freelist, so dropping the previous texture mid-flight is safe.
  std::shared_ptr<HPLTexture> texture(new HPLTexture{},
                                      HPLTexture::HPLTexture_Delete);

  VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = RIBootstrap::PogoColorFormatVk;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_DST: receives the pogo read-half blit each frame.
  // SAMPLED: the GUI (GuiSet) displays it like any other Image.
  // COLOR_ATTACHMENT: the DebugDraw overlay pass renders straight into it.
  imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  imageInfo.pQueueFamilyIndices = queueFamilies;
  VK_ConfigureImageQueueFamilies(&imageInfo, RI.device.queues, RI_QUEUE_LEN,
                                 queueFamilies, RI_QUEUE_LEN);

  VmaAllocationCreateInfo allocInfo = {};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (!VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imageInfo,
                                    &allocInfo, &texture->handle.vk.image,
                                    &texture->vk.vmaAlloc, NULL))) {
    Error("cViewportTarget: failed to create %ux%u color image\n", width,
          height);
    return;
  }

  VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = texture->handle.vk.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = imageInfo.format;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  texture->binding.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
  texture->binding.texture = &texture->handle;
  texture->binding.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  texture->binding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (!VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                       &texture->binding.vk.image.imageView))) {
    Error("cViewportTarget: failed to create image view\n");
    vmaDestroyImage(RI.device.vk.vmaAllocator, texture->handle.vk.image,
                    texture->vk.vmaAlloc);
    texture->handle.vk.image = VK_NULL_HANDLE;
    texture->vk.vmaAlloc = NULL;
    return;
  }
  RIFinalizeDescriptor(&RI.device, &texture->binding);

  texture->width = (uint16_t)width;
  texture->height = (uint16_t)height;
  texture->depth = 1;
  texture->mipNum = 1;
  texture->format = RIBootstrap::PogoColorFormat;

  mpTexture = texture;
  mlWidth = width;
  mlHeight = height;

  // Keep the Image wrapper stable across resizes — GUI elements hold the
  // Image*; GuiSet re-resolves the texture from it every draw.
  Image::SingleImage singleImage = {};
  singleImage.image = mpTexture;
  if (mpImage) {
    mpImage->SetImage(std::move(singleImage));
  } else {
    mpImage = std::make_shared<Image>(std::move(singleImage));
  }
#endif
}

} // namespace hpl
