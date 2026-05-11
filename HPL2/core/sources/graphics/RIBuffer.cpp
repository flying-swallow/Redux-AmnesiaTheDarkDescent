#include "graphics/RITypes.h"

uint64_t RIBuffer_s::GetDeviceHandle(struct RIDevice_s *device) {
  if(vk.buffer == NULL)
    return 0;
  VkBufferDeviceAddressInfo info = {
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = vk.buffer;
  return vkGetBufferDeviceAddress(device->vk.device, &info);
}
