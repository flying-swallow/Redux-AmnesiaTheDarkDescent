#include "graphics/RITypes.h"
#include <cassert>

void RIBuffer_s::setDebugObjectName(struct RIDevice_s *device,
                                    const char *name) {
  assert(vk.buffer);
  if (vkSetDebugUtilsObjectNameEXT && vk.buffer) {
    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
        VK_OBJECT_TYPE_BUFFER, (uint64_t)vk.buffer, name};
    VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
  }
}


void RIBuffer_s::dispose(struct RIDevice_s *device) {
		vkDestroyBuffer(device->vk.device, vk.buffer, NULL);
		vmaFreeMemory(device->vk.vmaAllocator, vk.allocation);
}

struct RIBuffer_s
RIBuffer_s::VK_createFromVMA(struct RIDevice_s *device,
                             VkBufferCreateInfo *vk_info,
                             VmaAllocationCreateInfo *vma_info) {


  RIBuffer_s buf = {};
  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, vk_info, vma_info,
                                &buf.vk.buffer, &buf.vk.allocation, nullptr));
  return buf;
}

uint64_t RIBuffer_s::GetDeviceHandle(struct RIDevice_s *device) {
  if (vk.buffer == NULL)
    return 0;
  VkBufferDeviceAddressInfo info = {
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = vk.buffer;
  return vkGetBufferDeviceAddress(device->vk.device, &info);
}
