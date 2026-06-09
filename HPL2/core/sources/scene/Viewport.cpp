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

#include "scene/Viewport.h"

#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/Renderer.h"

#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/World.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cViewport::cViewport(cScene *apScene)
	{
		mpScene = apScene;

		mbActive = true;
		mbVisible = true;

		mpRenderSettings = std::make_unique<cRenderSettings>();

		mpWorld = NULL;
		mpCamera = NULL;
		mpRenderer = NULL;
		mpPostEffectComposite = NULL;

		mbIsListener = false;
	}

	//-----------------------------------------------------------------------

	cViewport::~cViewport()
	{
		// GPU teardown of the viewport-owned targets: everything goes to the
		// frame freelist — freed once the in-flight pipeline is done with it
		// (or in RIBootstrap::Dispose at shutdown). No stall.
		RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
		std::visit(
			[cntx](auto &&arg) {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (!std::is_same_v<T, std::monostate>) {
					arg.Dispose(cntx);
				}
			},
			m_state);
		ReleasePogoBuffer(cntx);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// TARGET / STATE VISITORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cVector2l cViewport::GetTargetSize() const
	{
		return std::visit(
			[](auto &&arg) -> cVector2l {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, TargetSwapchain>) {
					return cVector2l((int)RI.swapchain.width,
									 (int)RI.swapchain.height);
				} else {
					return cVector2l((int)arg.width, (int)arg.height);
				}
			},
			mTarget);
	}

	struct RITextureView_s* cViewport::GetDepthView()
	{
		return std::visit(
			[](auto &&arg) -> struct RITextureView_s * {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return nullptr;
				} else {
					return arg.width != 0 ? &arg.depthView[RI.swapchainIndex]
										  : nullptr;
				}
			},
			m_state);
	}

	struct RITexture_s* cViewport::GetDepthTexture()
	{
		return std::visit(
			[](auto &&arg) -> struct RITexture_s * {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return nullptr;
				} else {
					return arg.width != 0 ? &arg.depthTextures[RI.swapchainIndex]
										  : nullptr;
				}
			},
			m_state);
	}

	cViewport::BackBuffer cViewport::GetBackBuffer()
	{
		return std::visit(
			[](auto &&arg) -> BackBuffer {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return BackBuffer{}; // zeroed — check renderTarget.vk.image
				} else {
					return arg.width != 0 ? arg.GetBackBuffer() : BackBuffer{};
				}
			},
			m_state);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// VIEWPORT TEXTURE HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

bool CreateViewportColorTexture(struct RIDevice_s *device, uint32_t width,
								uint32_t height, enum RI_Format_e format,
								VkImageUsageFlags usage,
								struct RITexture_s *tex,
								struct RIDescriptor_s *desc, const char *what) {
	uint32_t queueFamilies[RI_QUEUE_LEN] = {0};

	VmaAllocationCreateInfo memReqs = {};
	memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
				 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.extent = {width, height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.pQueueFamilyIndices = queueFamilies;
	VK_ConfigureImageQueueFamilies(&info, device->queues, RI_QUEUE_LEN,
								   queueFamilies, RI_QUEUE_LEN);
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.usage = usage;
	info.format = RIFormatToVK(format);

	if (!VK_WrapResult(vmaCreateImage(device->vk.vmaAllocator, &info, &memReqs,
									  &tex->vk.image, &tex->vk.allocation,
									  NULL))) {
		Error("%s: failed to create %ux%u color image\n", what, width, height);
		return false;
	}

	VkImageViewUsageCreateInfo usageInfo = {
		VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
	usageInfo.usage =
		usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_STORAGE_BIT);

	VkImageViewCreateInfo createInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	createInfo.pNext = &usageInfo;
	createInfo.subresourceRange =
		VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	createInfo.image = tex->vk.image;
	createInfo.format = RIFormatToVK(format);
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

	desc->flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
	desc->texture = tex;
	desc->vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	desc->vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (!VK_WrapResult(vkCreateImageView(device->vk.device, &createInfo, NULL,
										 &desc->vk.image.imageView))) {
		Error("%s: failed to create image view\n", what);
		vmaDestroyImage(device->vk.vmaAllocator, tex->vk.image,
						tex->vk.allocation);
		*tex = RITexture_s{};
		*desc = RIDescriptor_s{};
		return false;
	}
	desc->finalize(device);
	return true;
}

void ReleaseViewportColorTexture(std::vector<RIFreeHandle> &freelist,
								 struct RITexture_s *tex,
								 struct RIDescriptor_s *desc) {
	if (desc->vk.image.imageView) {
		RITextureView_s view = {};
		view.vk.image = desc->vk.image.imageView;
		freelist.push_back(view);
	}
	if (tex->vk.image) {
		freelist.push_back(*tex);
	}
	*desc = RIDescriptor_s{};
	*tex = RITexture_s{};
}

// Depth / visibility creation — ported verbatim from the retired swapchain-init
// block in Graphics.cpp, parameterized by extent.
bool CreateViewportAttachmentTexture(struct RIDevice_s *device, uint32_t width,
									 uint32_t height, enum RI_Format_e format,
									 VkImageUsageFlags usage,
									 VkImageAspectFlags aspect,
									 struct RITexture_s *tex,
									 struct RITextureView_s *view,
									 const char *what) {
	uint32_t queueFamilies[RI_QUEUE_LEN] = {0};

	VmaAllocationCreateInfo memReqs = {};
	memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
				 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.extent = {width, height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.pQueueFamilyIndices = queueFamilies;
	VK_ConfigureImageQueueFamilies(&info, device->queues, RI_QUEUE_LEN,
								   queueFamilies, RI_QUEUE_LEN);
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.format = RIFormatToVK(format);
	info.usage = usage;
	if (!VK_WrapResult(vmaCreateImage(device->vk.vmaAllocator, &info, &memReqs,
									  &tex->vk.image, &tex->vk.allocation,
									  NULL))) {
		Error("%s: failed to create %ux%u image\n", what, width, height);
		return false;
	}

	VkImageViewCreateInfo createInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	createInfo.format = RIFormatToVK(format);
	createInfo.subresourceRange = VkImageSubresourceRange{aspect, 0, 1, 0, 1};
	createInfo.image = tex->vk.image;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	if (!VK_WrapResult(vkCreateImageView(device->vk.device, &createInfo, NULL,
										 &view->vk.image))) {
		Error("%s: failed to create image view\n", what);
		vmaDestroyImage(device->vk.vmaAllocator, tex->vk.image,
						tex->vk.allocation);
		*tex = RITexture_s{};
		return false;
	}
	return true;
}

void ReleaseViewportAttachmentTexture(std::vector<RIFreeHandle> &freelist,
									  struct RITexture_s *tex,
									  struct RITextureView_s *view) {
	if (view->vk.image) {
		freelist.push_back(*view);
	}
	if (tex->vk.image) {
		freelist.push_back(*tex);
	}
	*view = RITextureView_s{};
	*tex = RITexture_s{};
}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// POGO BUFFER
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	RI_PogoBuffer* cViewport::PreparePogoBuffer(RIBootstrap::FrameContext *cntx)
	{
		const cVector2l vSize = GetTargetSize();
		const uint32_t alWidth = (uint32_t)vSize.x;
		const uint32_t alHeight = (uint32_t)vSize.y;
		if(alWidth == 0 || alHeight == 0)
		{
			return PogoBuffer();
		}
		if(mlPogoWidth == alWidth && mlPogoHeight == alHeight)
		{
			return &mPogoBuffer;
		}

		// Resize: hand the old halves to the frame freelist (the in-flight
		// pipeline still reads them) and recreate at the new extent — no
		// stall.
		ReleasePogoBuffer(cntx);

		RI_PogoBufferInit(&RI.device, &mPogoBuffer, alWidth, alHeight,
		                  RIBootstrap::PogoColorFormat);
		mlPogoWidth = alWidth;
		mlPogoHeight = alHeight;
		return &mPogoBuffer;
	}

	void cViewport::ReleasePogoBuffer(RIBootstrap::FrameContext *cntx)
	{
		if(mlPogoWidth == 0) return;

		for(size_t p = 0; p < 2; p++)
		{
			if(mPogoBuffer.pogoAttachment[p].vk.image.imageView)
			{
				RITextureView_s view = {};
				view.vk.image = mPogoBuffer.pogoAttachment[p].vk.image.imageView;
				cntx->freelist.push_back(view);
			}
			if(mPogoBuffer.textures[p].vk.image)
			{
				cntx->freelist.push_back(mPogoBuffer.textures[p]);
			}
			mPogoBuffer.pogoAttachment[p] = RIDescriptor_s{};
			mPogoBuffer.textures[p] = RITexture_s{};
		}
		mPogoBuffer.attachmentIndex = 0;
		mlPogoWidth = 0;
		mlPogoHeight = 0;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// EVALUATE (world draw -> feed -> post -> delivery)
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	namespace {

	// Single-subresource color barrier with the texture and transition filled
	// in — the feed/delivery path always touches exactly mip 0 / layer 0.
	RITextureBarrier_s ColorBarrier(RITexture_s *apTexture,
									enum RIResourceState_e aBefore, uint32_t aBeforeStages,
									enum RIResourceState_e aAfter, uint32_t aAfterStages)
	{
		RITextureBarrier_s barrier = {};
		barrier.texture = apTexture;
		barrier.before = aBefore;
		barrier.beforeStages = aBeforeStages;
		barrier.after = aAfter;
		barrier.afterStages = aAfterStages;
		barrier.mipCount = 1;
		barrier.layerCount = 1;
		return barrier;
	}

	// Fullscreen draw of the pogo READ half into a color-attachable view —
	// the single delivery primitive (TargetView panes and the swapchain tail
	// only differ in view/extent/format/pipeline-cache salt). The caller owns
	// the view's layout (must be COLOR_ATTACHMENT_OPTIMAL around the draw).
	void DrawPogoToTarget(RI_PogoBuffer *apPogo, VkImageView aView,
						  uint32_t alWidth, uint32_t alHeight, VkFormat aFormat,
						  uint32_t alHashSalt, const char *asLabel)
	{
		VkRenderingAttachmentInfo colorAttach = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		colorAttach.imageView   = aView;
		colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		renderInfo.renderArea = { { 0, 0 }, { alWidth, alHeight } };
		renderInfo.layerCount = 1;
		renderInfo.colorAttachmentCount = 1;
		renderInfo.pColorAttachments    = &colorAttach;

		vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

		VkViewport vp = { 0.0f, 0.0f, (float)alWidth, (float)alHeight, 0.0f, 1.0f };
		vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
		VkRect2D scr = { { 0, 0 }, { alWidth, alHeight } };
		vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &scr);

		PostEffectPipelineState blitState{};
		InitPostEffectPipelineState(blitState, aFormat, false);
		// Key on the salt AND the attachment format — bindPipeline's cache only
		// hashes kHash (the createInfo is consumed on first creation), so two
		// targets sharing a salt but differing in format must not collide.
		const hash_t kHash =
			hash_u32(hash_u32(HASH_INITIAL_VALUE, alHashSalt), (uint32_t)aFormat);
		RI.postEffectBlit.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
		                               asLabel, &blitState.createInfo);

		auto samplerDesc = RI.resolve_filter_descriptor(
		    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *samplerDesc;
		bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = *RI_PogoBufferShaderResource(apPogo);
		bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
		RI.postEffectBlit.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, bindings, 2);

		vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3, 1, 0, 0);
		vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
	}

	} // namespace

	//-----------------------------------------------------------------------

	bool cViewport::Evaluate(RIBootstrap::FrameContext *cntx, float afFrameTime, tFlag alFlags)
	{
		// Once per frame: the renderers' initial UNDEFINED backbuffer barriers
		// have an empty before-scope, so a second draw of the same per-
		// swapchain-image backbuffer would race this evaluation's feed blit.
		if(mlLastEvaluatedFrame == RI.frameIndex)
		{
			assert(false && "cViewport::Evaluate called twice in one frame");
			return false;
		}
		mlLastEvaluatedFrame = RI.frameIndex;

		cFrustum* pFrustum = mpCamera ? mpCamera->GetFrustum() : NULL;

		const bool worldRendered =
				(alFlags & tSceneRenderFlag_World) &&
				mpRenderer && mpWorld && pFrustum;

		if(alFlags & tSceneRenderFlag_World)
		{
			const cVector2l vPreSize = GetTargetSize();
			WorldDrawCtx preCtx{};
			preCtx.viewport   = this;
			preCtx.cmd        = &RI.primary.cmds[0];
			preCtx.device     = &RI.device;
			preCtx.frame      = cntx;
			preCtx.frustum    = pFrustum;
			preCtx.width      = (uint32_t)vPreSize.x;
			preCtx.height     = (uint32_t)vPreSize.y;
			preCtx.frameIndex = RI.frameIndex;
			preCtx.frameTime  = afFrameTime;
			m_onPreWorldDraw.Signal(preCtx);
			if (worldRendered) {
				mpRenderer->Draw(
						cntx,
						this,
						afFrameTime,
						pFrustum,
						mpWorld,
						GetRenderSettings(),
						false);

				// PRE-FEED HDR hook: the renderer left the BackBuffer holding
				// the linear-HDR scene (SHADER_READ) and depth in
				// DEPTH_ATTACHMENT_OPTIMAL. Handlers draw additive geometry
				// here (pickup flash / enemy glow) so the still-to-run feed
				// blit + post chain (bloom + tonemap) process it — the pogo
				// does not exist yet. Handlers must return the BackBuffer to
				// SHADER_READ for the feed blit below.
				PostTranslucenceDrawCtx transCtx{};
				transCtx.viewport   = this;
				transCtx.cmd        = &RI.primary.cmds[0];
				transCtx.device     = &RI.device;
				transCtx.frame      = cntx;
				transCtx.frustum    = pFrustum;
				transCtx.width      = (uint32_t)preCtx.width;
				transCtx.height     = (uint32_t)preCtx.height;
				transCtx.frameIndex = RI.frameIndex;
				transCtx.frameTime  = afFrameTime;
				transCtx.depthView  = GetDepthView();   // pogo not created yet
				m_onPostTranslucenceDraw.Signal(transCtx);
			}
		}

		// FEED + POST: once the world draw fully evaluated the viewport,
		// blit the backend's BackBuffer window (the crop is baked in —
		// the hybrid backend overdraws by its guard band) into the
		// viewport pogo READ half, prep the ATTACH half, and run the
		// post-effect chain on the pogo (each effect samples the read
		// half, renders the attach half, and toggles). Delivery happens
		// after, per the viewport's Target.
		BackBuffer backBuffer = GetBackBuffer();
		RI_PogoBuffer *pPogo = nullptr;
		if(worldRendered && backBuffer.renderTarget.vk.image != VK_NULL_HANDLE)
		{
			const cVector2l vTargetSize = GetTargetSize();
			pPogo = PreparePogoBuffer(cntx);

			const uint32_t readIdx = (pPogo->attachmentIndex + 1u) % 2u;
			VkImage srcImage = backBuffer.renderTarget.vk.image;
			VkImage dstImage = pPogo->textures[readIdx].vk.image;

			// Pre-blit: BackBuffer (left SHADER_READ by the renderer) ->
			// TRANSFER_SRC, pogo read half -> TRANSFER_DST (UNDEFINED
			// discard — fully overwritten, the fragment-stage hint orders
			// the overwrite after the prior frame's reads), pogo attach
			// half UNDEFINED -> COLOR so the post chain (which renders
			// into the attach half with no barrier of its own) finds it
			// ready.
			const RITextureBarrier_s pre[3] = {
				ColorBarrier(&backBuffer.renderTarget,
							 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
							 RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_BLIT),
				ColorBarrier(&pPogo->textures[readIdx],
							 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
							 RI_RESOURCE_STATE_COPY_DST, RI_STAGE_BLIT),
				RI_PogoAttachmentBarrier(
							 &pPogo->textures[pPogo->attachmentIndex], /*initial=*/true),
			};
			RI.primary.cmds[0].textureBarriers<3>(3, pre);

			VkImageBlit region = {};
			region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.srcOffsets[0]  = { (int32_t)backBuffer.x, (int32_t)backBuffer.y, 0 };
			region.srcOffsets[1]  = { (int32_t)(backBuffer.x + backBuffer.width),
			                          (int32_t)(backBuffer.y + backBuffer.height), 1 };
			region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.dstOffsets[0]  = { 0, 0, 0 };
			region.dstOffsets[1]  = { (int32_t)backBuffer.width, (int32_t)backBuffer.height, 1 };
			vkCmdBlitImage(RI.primary.cmds[0].vk.cmd, srcImage,
			               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
			               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
			               VK_FILTER_NEAREST);

			// Post-blit: BackBuffer back to SHADER_READ (the layout the
			// backend re-acquires it from next frame), pogo read half ->
			// SHADER_READ for the post chain / delivery.
			const RITextureBarrier_s post[2] = {
				ColorBarrier(&backBuffer.renderTarget,
							 RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_BLIT,
							 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT),
				ColorBarrier(&pPogo->textures[readIdx],
							 RI_RESOURCE_STATE_COPY_DST, RI_STAGE_BLIT,
							 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT),
			};
			RI.primary.cmds[0].textureBarriers<2>(2, post);

			cPostEffectComposite *pComposite = GetPostEffectComposite();
			if(pComposite && (alFlags & tSceneRenderFlag_PostEffects) &&
			   pComposite->HasActiveEffects())
			{
				pComposite->Render(afFrameTime, &RI.primary.cmds[0], pPogo,
				                   (uint32_t)vTargetSize.x, (uint32_t)vTargetSize.y,
				                   RI.frameIndex);
			}

			// The pogo read half holds the final (post-processed) image.
			PostWorldDrawCtx postCtx{};
			postCtx.viewport   = this;
			postCtx.cmd        = &RI.primary.cmds[0];
			postCtx.device     = &RI.device;
			postCtx.frame      = cntx;
			postCtx.frustum    = pFrustum;
			postCtx.width      = (uint32_t)vTargetSize.x;
			postCtx.height     = (uint32_t)vTargetSize.y;
			postCtx.frameIndex = RI.frameIndex;
			postCtx.frameTime  = afFrameTime;
			postCtx.pogo       = pPogo;
			postCtx.depthView  = GetDepthView();
			m_onPostWorldDraw.Signal(postCtx);
		}

		// Delivery — symmetric branch on the viewport's own Target
		// variant. Shared post-delivery context (pPogo is valid in both
		// branches' signal guard; the delivered target itself is reached
		// through ctx.viewport). TargetView (standalone, e.g. editor panes /
		// headless thumbnails): a fullscreen draw of the pogo read half into
		// the caller-provided view when one was set (contract: color-
		// attachable, matching the TargetView's `format`; fully rewritten
		// every frame and left SHADER_READ_ONLY for the consumer — the
		// editor's pane Image samples it). No swapchain composite or GUI for
		// these.
		const cVector2l vDeliverSize = GetTargetSize();
		WorldDrawCtx deliverCtx{};
		deliverCtx.viewport   = this;
		deliverCtx.cmd        = &RI.primary.cmds[0];
		deliverCtx.device     = &RI.device;
		deliverCtx.frame      = cntx;
		deliverCtx.frustum    = pFrustum;
		deliverCtx.width      = (uint32_t)vDeliverSize.x;
		deliverCtx.height     = (uint32_t)vDeliverSize.y;
		deliverCtx.frameIndex = RI.frameIndex;
		deliverCtx.frameTime  = afFrameTime;
		if(const auto *pView = std::get_if<TargetView>(&mTarget))
		{
			if(pView->view.vk.image != VK_NULL_HANDLE &&
			   worldRendered && pPogo != nullptr)
			{
				// Caller texture: discard previous contents (fully
				// rewritten; also covers its first use) -> COLOR for the
				// delivery draw, then -> SHADER_READ for the consumer.
				RITexture_s *pViewTexture = const_cast<RITexture_s *>(&pView->texture);
				RI.primary.cmds[0].textureBarrier(ColorBarrier(pViewTexture,
								 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE));

				DrawPogoToTarget(pPogo, pView->view.vk.image, pView->width, pView->height,
								 pView->format, 2u, "PostEffect.targetViewBlit");

				RI.primary.cmds[0].textureBarrier(ColorBarrier(pViewTexture,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
								 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT));

				// The target is in a known layout (SHADER_READ_ONLY) —
				// handlers may record against it (e.g. a readback copy; the
				// thumbnail builder transitions to COPY_SRC and back).
				m_onPostDelivery.Signal(deliverCtx);
			}
		}
		// TargetSwapchain: composite to the swapchain — tail draw of the
		// (post-processed) pogo read half BEFORE the GUI overlays (the GUI
		// block runs in cScene::Render, which owns the gui sets).
		else if(std::holds_alternative<TargetSwapchain>(mTarget))
		{
			if(worldRendered && pPogo != nullptr)
			{
				// Swapchain images are raw VkImage handles — bridge through
				// a stack RITexture_s for the barrier.
				RITexture_s swapchainTexture = {};
				swapchainTexture.vk.image = RI.swapchain.vk.images[RI.swapchainIndex];
				RI.primary.cmds[0].textureBarrier(ColorBarrier(&swapchainTexture,
								 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE));

				DrawPogoToTarget(pPogo, RI.swapchainView[RI.swapchainIndex].vk.image,
								 RI.swapchain.width, RI.swapchain.height,
								 RIFormatToVK((RI_Format_e)RI.swapchain.format), 1u,
								 "PostEffect.tailBlit");

				m_onPostDelivery.Signal(deliverCtx);
			}
		}

		return worldRendered;
	}


	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	void cViewport::SetWorld(cWorld *apWorld)
	{ 
		if(mpWorld != NULL && mpScene->WorldExists(mpWorld))
		{
			mpWorld->SetIsSoundEmitter(false);
		}

		mpWorld = apWorld;
		if(mpWorld) mpWorld->SetIsSoundEmitter(true);

		mpRenderSettings->ResetVariables();
	}
	

	//-----------------------------------------------------------------------

	void cViewport::AddGuiSet(cGuiSet *apSet)
	{
		m_guiSets.push_back(apSet);
	}
	void cViewport::RemoveGuiSet(cGuiSet *apSet)
	{
		STLFindAndRemove(m_guiSets, apSet);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	

	//-----------------------------------------------------------------------

}
