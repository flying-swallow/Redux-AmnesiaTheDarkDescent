/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "LuxScreenCapture.h"

#include "LuxBase.h"
#include "LuxMapHandler.h"

#include "engine/Engine.h"
#include "gui/Gui.h"
#include "scene/Viewport.h"
#include "graphics/RIPogoBuffer.h"

using namespace hpl;

//-----------------------------------------------------------------------

// Allocate `outTex` as a screen-sized color attachment that is also sampleable
// as a 2D texture, and wire up the descriptor binding used by
// cGui::CreateGfxTexture / RIProgram::bindDescriptors. The cTexture_Delete
// deleter expects `handle.vk.image` (+ its VMA allocation) and
// `binding.vk.image.imageView` to be set — see cTexture.cpp:19 — so we fill
// all three.
bool LuxCreateScreenRenderTarget(
		std::shared_ptr<hpl::cTexture> &outTex,
		uint32_t width, uint32_t height, uint32_t format, const char *debugName)
{
	auto tex = std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);
	tex->width  = static_cast<uint16_t>(width);
	tex->height = static_cast<uint16_t>(height);
	tex->depth  = 1;
	tex->mipNum = 1;

	RITextureDesc texDesc = {};
	texDesc.type = RI_TEXTURE_2D;
	texDesc.format = format;
	texDesc.width = width;
	texDesc.height = height;
	texDesc.depth = 1;
	texDesc.mipNum = 1;
	texDesc.layerNum = 1;
	texDesc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_COLOR_ATTACHMENT |
	                RI_USAGE_TRANSFER_DST;
	tex->handle = RITexture::create(&RI.device, texDesc);
	if (tex->handle.isEmpty(&RI.device))
		return false;

	RITextureViewDesc viewDesc = {};
	viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
	viewDesc.format = format;
	viewDesc.mipNum = 1;
	viewDesc.layerNum = 1;
	tex->view = RITextureView::create(&RI.device, &tex->handle, viewDesc);
	tex->binding = RIDescriptor::sampledImage(&RI.device, &tex->view, hash_random());
//tex->binding = {};
//	tex->binding.texture = &tex->handle;
//	tex->binding.view = &tex->view;
//	tex->binding.type = RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
//#if (DEVICE_IMPL_VULKAN)
//	tex->binding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
//#endif
//	tex->binding.finalize(&RI.device);
	tex->setDebugName(debugName);

	outTex = std::move(tex);
	return true;
}

//-----------------------------------------------------------------------

// Lay down a single whole-image color barrier on the given command buffer.
static void ImageBarrier(RICmd *cmd, RITexture *texture,
		uint32_t before, uint32_t beforeStages,
		uint32_t after, uint32_t afterStages)
{
	RITextureBarrier barrier(texture, before, after, beforeStages, afterStages);
	barrier.mipCount   = 1;
	barrier.layerCount = 1;
	cmd->vk_d3d12_textureBarrier(barrier);
}

//-----------------------------------------------------------------------

cLuxScreenCapture::cLuxScreenCapture() : iLuxUpdateable("ScreenCapture")
{
}

cLuxScreenCapture::~cLuxScreenCapture()
{
	Destroy();
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::EnsureTextures()
{
	if (m_primaryColor) {
		return;
	}

	if (mpGui == nullptr) {
		mpGui = gpBase->mpEngine->GetGui();
	}

	mlWidth  = RI.swapchain.width;
	mlHeight = RI.swapchain.height;

	// Same format as the pogo so the capture is a straight vkCmdCopyImage.
	LuxCreateScreenRenderTarget(m_primaryColor, mlWidth, mlHeight,
	                            RIBootstrap::PogoColorFormat, "screen.primaryCapture");

	m_primaryImage = std::make_shared<Image>(Image::SingleImage{m_primaryColor});
	mpScreenGfx    = mpGui->CreateGfxTexture(m_primaryImage.get(), false, eGuiMaterial_Diffuse);
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::RequestCapture()
{
	EnsureTextures();
	mbPending = true;
	// Invalidate the previous capture: consumers (the per-state effect, the
	// sharp quad) must wait for the NEW copy to land instead of re-using the
	// stale frame still resident in m_primaryColor from a prior menu open.
	mbCaptured = false;
}

//-----------------------------------------------------------------------

RIDescriptor* cLuxScreenCapture::PrimaryDescriptor()
{
	return m_primaryColor ? &m_primaryColor->binding : nullptr;
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::OnPostRender(float afFrameTime)
{
	if (!mbPending) {
		return;
	}
	if (!m_primaryColor) {
		return;
	}

	if (!RI.primary.cmds || !RI.primary.cmds[0].isOpen(&RI.renderer)) {
		return; // No active frame to ride on; try again next post-render.
	}

	// The clean game scene (world + post-effects, NO GUI) lives in the gameplay
	// viewport's RETAINED pogo "read" half. The gameplay viewport is hidden while
	// the menu is up, but its pogo is retained (ReleasePogoBuffer only runs on
	// teardown/resize), so it still holds the last gameplay frame, left
	// SHADER_RESOURCE by the renderer's tail hand-off.
	cViewport *pViewport = gpBase->mpMapHandler ? gpBase->mpMapHandler->GetViewport() : nullptr;
	RI_PogoBuffer *pPogo = pViewport ? pViewport->PogoBuffer() : nullptr;
	if (pPogo == nullptr) {
		return; // No gameplay frame rendered yet; try again next post-render.
	}
	RITexture *pSource = &pPogo->textures[(pPogo->attachmentIndex + 1) % 2];

	// Record the copy on the primary command buffer. As a global module our
	// OnPostRender runs before the active state's cLuxScreenEffect::OnPostRender,
	// so this copy is sequenced before the effect samples m_primaryColor.
	ImageBarrier(&RI.primary.cmds[0], pSource,
			RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
			RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_COPY);
	ImageBarrier(&RI.primary.cmds[0], &m_primaryColor->handle,
			mbPrimaryEverWritten ? RI_RESOURCE_STATE_SHADER_RESOURCE : RI_RESOURCE_STATE_UNDEFINED,
			mbPrimaryEverWritten ? RI_STAGE_FRAGMENT : RI_STAGE_NONE,
			RI_RESOURCE_STATE_COPY_DST, RI_STAGE_COPY);

	RIImageCopyDesc region = {};
	region.width  = (uint32_t)mlWidth;
	region.height = (uint32_t)mlHeight;
	region.depth  = 1;
	RI.primary.cmds[0].copyImage(&RI.renderer, pSource, &m_primaryColor->handle, region);

	// Restore the pogo half for the renderer's reuse; leave the capture
	// sampleable for the GUI and the per-state effect pass.
	ImageBarrier(&RI.primary.cmds[0], pSource,
			RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_COPY,
			RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
	ImageBarrier(&RI.primary.cmds[0], &m_primaryColor->handle,
			RI_RESOURCE_STATE_COPY_DST, RI_STAGE_COPY,
			RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

	mbPending            = false;
	mbCaptured           = true;
	mbPrimaryEverWritten = true;
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::Destroy()
{
	if (mpScreenGfx && mpGui) {
		mpGui->DestroyGfx(mpScreenGfx);
	}
	mpScreenGfx = nullptr;

	// The cTexture deleter parks the GPU handles on the active frame slot's
	// freelist; they're released once that slot's fence has signaled.
	m_primaryImage.reset();
	m_primaryColor.reset();

	mbPending            = false;
	mbCaptured           = false;
	mbPrimaryEverWritten = false;
}

//-----------------------------------------------------------------------
