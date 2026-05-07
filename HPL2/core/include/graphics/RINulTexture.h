#ifndef RI_NUL_TEXTURE_H
#define RI_NUL_TEXTURE_H

#include "graphics/RITypes.h"

struct RINulTexture
{
	struct RITexture_s tex;
	struct RIDescriptor_s binding;

	union
	{
#if ( DEVICE_IMPL_VULKAN )
		struct
		{
			// Final GPU image allocation
			struct VmaAllocation_T* vmaAlloc;

			// Temporary upload resources for the 1x1 white texture
			// These must survive until the upload command buffer has finished executing
			VkBuffer uploadStagingBuffer;
			struct VmaAllocation_T* uploadStagingAlloc;
		} vk;
#endif
	};

	static bool Create2DNulWhite(struct RICmd_s* cmd, struct RIDevice_s* device, struct RINulTexture* tex);

#if ( DEVICE_IMPL_VULKAN )
	static void Free2DNulWhiteUploadScratch(struct RIDevice_s* device, struct RINulTexture* tex);
#endif
};

#endif
