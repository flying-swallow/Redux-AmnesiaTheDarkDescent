#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/MaterialResource.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/Renderable.h"
#include "math/Frustum.h"
#include "math/Math.h"

#include "scene/RenderableContainer.h"
#include "scene/World.h"

#include <cstring>
#include <functional>

namespace hpl {
cHybridRenderer::cHybridRenderer(cGraphics *apGraphics,
                                 cResources *apResources) 
		: iRenderer("Deferred",apGraphics, apResources, 0)

{
		uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
		VkBufferCreateInfo vertexBufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		VK_ConfigureBufferQueueFamilies( &vertexBufferCreateInfo, RI.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );
		vertexBufferCreateInfo.pNext = NULL;
		vertexBufferCreateInfo.flags = 0;
		vertexBufferCreateInfo.size = UBO_BUFFER_SIZE;
		vertexBufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		VmaAllocationInfo allocationInfo = { 0 };
		VmaAllocationCreateInfo allocInfo = { 0 };
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		//VK_WrapResult( vmaCreateBuffer( RI.device.vk.vmaAllocator, &vertexBufferCreateInfo, &allocInfo, &sceneUBO.vk.buffer, &sceneUBO.vk.alloc, &allocationInfo ) );
		//if( vkSetDebugUtilsObjectNameEXT ) {
		//	VkDebugUtilsObjectNameInfoEXT debugName = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, VK_OBJECT_TYPE_BUFFER, (uint64_t)sceneUBO.vk.buffer, "VBO_VERTEX_BUFFER" };
		//	VK_WrapResult( vkSetDebugUtilsObjectNameEXT( RI.device.vk.device, &debugName ) );
		//}

    //VmaVirtualBlockCreateInfo allocCreateInfo = {};
    //allocCreateInfo.size = UBO_BUFFER_SIZE;
    //vmaCreateVirtualBlock(&allocCreateInfo, &sceneUBOVirtualAlloc);
}

void cHybridRenderer::Draw(
    		RIBootstrap::FrameContext* cntx,
        cViewport* viewport,
        float afFrameTime,
        cFrustum* apFrustum,
        cWorld* apWorld,
        cRenderSettings* apSettings,
        bool abSendFrameBufferToPostEffects) {


}

cHybridRenderer::~cHybridRenderer() {}

} // namespace hpl
