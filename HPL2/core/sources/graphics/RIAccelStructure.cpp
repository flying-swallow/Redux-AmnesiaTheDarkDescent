#include "graphics/RITypes.h"
#include <cassert>


void RIAccelStructure_s::dispose(struct RIDevice_s *device) {
  vkDestroyAccelerationStructureKHR(device->vk.device, vk.handle, NULL);
}


