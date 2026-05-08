#ifndef RI_POGO_BUFFER_H
#define RI_POGO_BUFFER_H

#include "RIFormat.h"
#include "RITypes.h"

struct FrameState_s;

struct RI_PogoBuffer {
	struct RITexture_s textures[2];
	struct RIDescriptor_s pogoAttachment[2];
	uint16_t attachmentIndex;
};

VkImageMemoryBarrier2 VK_RI_PogoShaderMemoryBarrier2( VkImage image, bool initial );
VkImageMemoryBarrier2 VK_RI_PogoAttachmentMemoryBarrier2( VkImage image, bool initial );

void RI_PogoBufferInit( struct RIDevice_s *device, struct RI_PogoBuffer *pogo, uint32_t width, uint32_t height, enum RI_Format_e format );
void RI_PogoBufferDestroy( struct RIDevice_s *device, struct RI_PogoBuffer *pogo );
void RI_PogoBufferToggle( struct RIDevice_s *device, struct RI_PogoBuffer *pogo, struct RICmd_s *handle );

static inline struct RIDescriptor_s *RI_PogoBufferAttachment( struct RI_PogoBuffer *pogo )
{
	return pogo->pogoAttachment + pogo->attachmentIndex;
}

static inline struct RIDescriptor_s *RI_PogoBufferShaderResource( struct RI_PogoBuffer *pogo )
{
	return pogo->pogoAttachment + ( ( pogo->attachmentIndex + 1 ) % 2 );
}

#endif


