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

// Hand-off transition: pogo attachment -> fragment-sampled resource.
// initial=true treats the contents as discarded (UNDEFINED source).
static inline struct RITextureBarrier_s RI_PogoShaderBarrier( struct RITexture_s *texture, bool initial )
{
	struct RITextureBarrier_s barrier = {};
	barrier.texture = texture;
	barrier.before = initial ? RI_RESOURCE_STATE_UNDEFINED : RI_RESOURCE_STATE_RENDER_TARGET;
	barrier.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
	barrier.afterStages = RI_STAGE_FRAGMENT;
	return barrier;
}

// Reuse transition: fragment-sampled resource -> pogo attachment.
// initial=true treats the contents as discarded (UNDEFINED source).
static inline struct RITextureBarrier_s RI_PogoAttachmentBarrier( struct RITexture_s *texture, bool initial )
{
	struct RITextureBarrier_s barrier = {};
	barrier.texture = texture;
	barrier.before = initial ? RI_RESOURCE_STATE_UNDEFINED : RI_RESOURCE_STATE_SHADER_RESOURCE;
	barrier.beforeStages = initial ? RI_STAGE_NONE : RI_STAGE_FRAGMENT;
	barrier.after = RI_RESOURCE_STATE_RENDER_TARGET;
	return barrier;
}

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


