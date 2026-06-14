#include "graphics/RITypes.h"
#include <cassert>

void RIBuffer::setDebugObjectName(struct RIDevice *device,
                                    const char *name) {
  assert(vk.buffer);
  if (vkSetDebugUtilsObjectNameEXT && vk.buffer) {
    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
        VK_OBJECT_TYPE_BUFFER, (uint64_t)vk.buffer, name};
    VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
  }
}


void RIBuffer::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
	if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
		vkDestroyBuffer(device->vk.device, vk.buffer, NULL);
		vmaFreeMemory(device->vk.vmaAllocator, vk.allocation);
	}
#endif
}

struct RIBuffer
RIBuffer::VK_createFromVMA(struct RIDevice *device,
                             VkBufferCreateInfo *vk_info,
                             VmaAllocationCreateInfo *vma_info) {


  RIBuffer buf = {};
  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, vk_info, vma_info,
                                &buf.vk.buffer, &buf.vk.allocation, nullptr));
  return buf;
}

uint64_t RIBuffer::GetDeviceHandle(struct RIDevice *device) {
  if (vk.buffer == NULL)
    return 0;
  VkBufferDeviceAddressInfo info = {
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = vk.buffer;
  return vkGetBufferDeviceAddress(device->vk.device, &info);
}
