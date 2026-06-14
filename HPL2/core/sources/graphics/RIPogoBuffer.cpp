#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"

#if ( DEVICE_IMPL_VULKAN )
#include <vk_mem_alloc.h>
#endif

void RI_PogoBufferInit( struct RIDevice *device, struct RI_PogoBuffer *pogo, uint32_t width, uint32_t height, enum RI_Format_e format )
{
	pogo->attachmentIndex = 0;

	// STORAGE lets the SurfelGI composite (a compute pass) write the pogo attach
	// via an RWTexture2D; still color-attachment + sampled for the raster passes
	// and post-effect chain. TRANSFER_SRC/DST back the guard-band crop blit
	// (overscan render-pogo center → authored pogo) at the end of Draw.
	RITextureDesc texDesc = {};
	texDesc.type = RI_TEXTURE_2D;
	texDesc.format = format;
	texDesc.width = width;
	texDesc.height = height;
	texDesc.depth = 1;
	texDesc.mipNum = 1;
	texDesc.layerNum = 1;
	texDesc.usage = RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
	                RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_TRANSFER_SRC |
	                RI_USAGE_TRANSFER_DST;
	texDesc.flags = RI_TEXTURE_FLAG_MUTABLE_FORMAT;

	RITextureViewDesc viewDesc = {};
	viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
	viewDesc.format = format;
	viewDesc.mipNum = 1;
	viewDesc.layerNum = 1;

	for( size_t p = 0; p < 2; p++ ) {
		pogo->textures[p] = RITexture::create( device, texDesc );
		pogo->views[p] = RITextureView::create( device, &pogo->textures[p], viewDesc );

		pogo->pogoAttachment[p] = RIDescriptor::sampledImage( device, &pogo->views[p], hash_random() );
		pogo->pogoAttachment[p].texture = &pogo->textures[p];
	}
}

void RI_PogoBufferDestroy( struct RIDevice *device, struct RI_PogoBuffer *pogo )
{
	for( size_t p = 0; p < 2; p++ ) {
		pogo->views[p].dispose( device );
		pogo->textures[p].dispose( device );
		pogo->pogoAttachment[p] = RIDescriptor{};
		pogo->textures[p] = RITexture{};
		pogo->views[p] = RITextureView{};
	}
}

void RI_PogoBufferToggle( struct RIDevice *device, struct RI_PogoBuffer *pogo, struct RICmd *handle )
{
	struct RITextureBarrier barriers[2] = {};
	barriers[0] = RI_PogoShaderBarrier( &pogo->textures[pogo->attachmentIndex], false );
	pogo->attachmentIndex = ( ( pogo->attachmentIndex + 1 ) % 2 );
	barriers[1] = RI_PogoAttachmentBarrier( &pogo->textures[pogo->attachmentIndex], false );
	handle->vk_d3d12_textureBarriers<2>( 2, barriers );
}
