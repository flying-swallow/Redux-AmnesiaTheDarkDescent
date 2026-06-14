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

	struct RITextureView* cViewport::GetDepthView()
	{
		return std::visit(
			[](auto &&arg) -> struct RITextureView * {
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

	struct RITexture* cViewport::GetDepthTexture()
	{
		return std::visit(
			[](auto &&arg) -> struct RITexture * {
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

bool CreateViewportColorTexture(struct RIDevice *device, uint32_t width,
								uint32_t height, enum RI_Format_e format,
								uint32_t usage,
								struct RITexture *tex,
								struct RITextureView *view,
								struct RIDescriptor *desc, const char *what) {
	RITextureDesc texDesc = {};
	texDesc.type = RI_TEXTURE_2D;
	texDesc.format = format;
	texDesc.width = width;
	texDesc.height = height;
	texDesc.depth = 1;
	texDesc.mipNum = 1;
	texDesc.layerNum = 1;
	texDesc.usage = usage;
	texDesc.flags = RI_TEXTURE_FLAG_MUTABLE_FORMAT;
	*tex = RITexture::create(device, texDesc);
	if (tex->isEmpty(device)) {
		Error("%s: failed to create %ux%u color image\n", what, width, height);
		return false;
	}

	RITextureViewDesc viewDesc = {};
	viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
	viewDesc.format = format;
	viewDesc.mipNum = 1;
	viewDesc.layerNum = 1;
	*view = RITextureView::create(device, tex, viewDesc);

	// The caller (renderer state) owns `view`; the descriptor references it.
	// Created once per (re)size, so hash_random() is a stable per-instance cookie.
	*desc = RIDescriptor::sampledImage(device, view, hash_random());
	return true;
}

void ReleaseViewportColorTexture(std::vector<RIFreeHandle> &freelist,
								 struct RITexture *tex,
								 struct RITextureView *view,
								 struct RIDescriptor *desc) {
	if (!desc->isEmpty())
		freelist.push_back(*view); // defer the owned color view's destruction
	freelist.push_back(*tex);
	*view = RITextureView{};
	*desc = RIDescriptor{};
	*tex = RITexture{};
}

// Depth / visibility creation, parameterized by extent. Aspect is derived from
// the format inside RITextureView::create.
bool CreateViewportAttachmentTexture(struct RIDevice *device, uint32_t width,
									 uint32_t height, enum RI_Format_e format,
									 uint32_t usage,
									 enum RITextureViewType_e viewType,
									 struct RITexture *tex,
									 struct RITextureView *view,
									 const char *what) {
	RITextureDesc texDesc = {};
	texDesc.type = RI_TEXTURE_2D;
	texDesc.format = format;
	texDesc.width = width;
	texDesc.height = height;
	texDesc.depth = 1;
	texDesc.mipNum = 1;
	texDesc.layerNum = 1;
	texDesc.usage = usage;
	texDesc.flags = RI_TEXTURE_FLAG_MUTABLE_FORMAT;
	*tex = RITexture::create(device, texDesc);
	if (tex->isEmpty(device)) {
		Error("%s: failed to create %ux%u image\n", what, width, height);
		return false;
	}

	RITextureViewDesc viewDesc = {};
	viewDesc.viewType = viewType;
	viewDesc.format = format;
	viewDesc.mipNum = 1;
	viewDesc.layerNum = 1;
	*view = RITextureView::create(device, tex, viewDesc);
	return true;
}

void ReleaseViewportAttachmentTexture(std::vector<RIFreeHandle> &freelist,
									  struct RITexture *tex,
									  struct RITextureView *view) {
	freelist.push_back(*view);
	freelist.push_back(*tex);
	*view = RITextureView{};
	*tex = RITexture{};
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
			// The pogo view + texture are created together; free both when the
			// texture is live (RITextureView has no isEmpty — gate on the texture).
			if(!mPogoBuffer.textures[p].isEmpty(&RI.device))
			{
				cntx->freelist.push_back(mPogoBuffer.views[p]);
				mPogoBuffer.views[p] = RITextureView{};
				cntx->freelist.push_back(mPogoBuffer.textures[p]);
			}
			mPogoBuffer.pogoAttachment[p] = RIDescriptor{};
			mPogoBuffer.textures[p] = RITexture{};
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
	RITextureBarrier ColorBarrier(RITexture *apTexture,
									enum RIResourceState_e aBefore, uint32_t aBeforeStages,
									enum RIResourceState_e aAfter, uint32_t aAfterStages)
	{
		RITextureBarrier barrier = {};
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
	void DrawPogoToTarget(RI_PogoBuffer *apPogo, RITextureView *aView,
						  uint32_t alWidth, uint32_t alHeight, enum RI_Format_e aFormat,
						  uint32_t alHashSalt, const char *asLabel)
	{
		RIRenderingAttachment colorAttach = {};
		colorAttach.view    = aView;
		colorAttach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		RIBeginRenderingDesc renderInfo = {};
		renderInfo.renderArea.width  = (int16_t)alWidth;
		renderInfo.renderArea.height = (int16_t)alHeight;
		renderInfo.colorCount = 1;
		renderInfo.colors     = &colorAttach;

		RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderInfo);

		RI.primary.cmds[0].mtl_encoderDraw(renderInfo);

		RIViewport vp = {};
		vp.width    = (float)alWidth;
		vp.height   = (float)alHeight;
		vp.depthMax = 1.0f;
		RI.primary.cmds[0].setViewport(&RI.renderer, vp);
		RIRect scr = {};
		scr.width  = (int16_t)alWidth;
		scr.height = (int16_t)alHeight;
		RI.primary.cmds[0].setScissor(&RI.renderer, scr);

		RIGraphicsPipelineDesc blitState{};
		blitState.colorCount = 1;
		blitState.colors[0].format = aFormat; // blend disabled (default)
		// Key on the salt AND the attachment format — bindPipeline's cache only
		// hashes kHash, so two targets sharing a salt but differing in format
		// must not collide.
		const hash_t kHash =
			hash_u32(hash_u32(HASH_INITIAL_VALUE, alHashSalt), (uint32_t)aFormat);
		RI.postEffectBlit.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
		                               asLabel, blitState);

		auto samplerDesc = RI.resolve_filter_descriptor(
		    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *samplerDesc;
		bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = *RI_PogoBufferShaderResource(apPogo);
		bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
		RI.postEffectBlit.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, bindings, 2);

		RI.primary.cmds[0].draw(&RI.renderer, 3, 1, 0, 0);
		RI.primary.cmds[0].mtl_encoderEnd();
		RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);
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
		if(worldRendered && !backBuffer.renderTarget.isEmpty(&RI.device))
		{
			const cVector2l vTargetSize = GetTargetSize();
			pPogo = PreparePogoBuffer(cntx);

			const uint32_t readIdx = (pPogo->attachmentIndex + 1u) % 2u;

			// Pre-blit: BackBuffer (left SHADER_READ by the renderer) ->
			// TRANSFER_SRC, pogo read half -> TRANSFER_DST (UNDEFINED
			// discard — fully overwritten, the fragment-stage hint orders
			// the overwrite after the prior frame's reads), pogo attach
			// half UNDEFINED -> COLOR so the post chain (which renders
			// into the attach half with no barrier of its own) finds it
			// ready.
			const RITextureBarrier pre[3] = {
				ColorBarrier(&backBuffer.renderTarget,
							 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
							 RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_BLIT),
				ColorBarrier(&pPogo->textures[readIdx],
							 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
							 RI_RESOURCE_STATE_COPY_DST, RI_STAGE_BLIT),
				RI_PogoAttachmentBarrier(
							 &pPogo->textures[pPogo->attachmentIndex], /*initial=*/true),
			};
			RI.primary.cmds[0].vk_d3d12_textureBarriers<3>(3, pre);

			// Source and destination are the same size, so the former
			// vkCmdBlitImage (NEAREST) is a 1:1 region copy: read the
			// backbuffer's sub-rect into the pogo read half at the origin.
			RIImageCopyDesc region = {};
			region.srcX   = (int32_t)backBuffer.x;
			region.srcY   = (int32_t)backBuffer.y;
			region.width  = (uint32_t)backBuffer.width;
			region.height = (uint32_t)backBuffer.height;
			region.depth  = 1;
			RI.primary.cmds[0].copyImage(&RI.renderer, &backBuffer.renderTarget,
			                             &pPogo->textures[readIdx], region);

			// Post-blit: BackBuffer back to SHADER_READ (the layout the
			// backend re-acquires it from next frame), pogo read half ->
			// SHADER_READ for the post chain / delivery.
			const RITextureBarrier post[2] = {
				ColorBarrier(&backBuffer.renderTarget,
							 RI_RESOURCE_STATE_COPY_SRC, RI_STAGE_BLIT,
							 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT),
				ColorBarrier(&pPogo->textures[readIdx],
							 RI_RESOURCE_STATE_COPY_DST, RI_STAGE_BLIT,
							 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT),
			};
			RI.primary.cmds[0].vk_d3d12_textureBarriers<2>(2, post);

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
			if(!pView->texture.isEmpty(&RI.device) &&
			   worldRendered && pPogo != nullptr)
			{
				// Caller texture: discard previous contents (fully
				// rewritten; also covers its first use) -> COLOR for the
				// delivery draw, then -> SHADER_READ for the consumer.
				RITexture *pViewTexture = const_cast<RITexture *>(&pView->texture);
				RI.primary.cmds[0].vk_d3d12_textureBarrier(ColorBarrier(pViewTexture,
								 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE));

				DrawPogoToTarget(pPogo, const_cast<RITextureView *>(&pView->view),
								 pView->width, pView->height,
								 (enum RI_Format_e)pView->format, 2u, "PostEffect.targetViewBlit");

				RI.primary.cmds[0].vk_d3d12_textureBarrier(ColorBarrier(pViewTexture,
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
				// The swapchain exposes a neutral per-image RITexture/RITextureView
				// (RISwapchain.cpp fills textures[]/swapchainView[] for both backends).
				RI.primary.cmds[0].vk_d3d12_textureBarrier(ColorBarrier(
								 &RI.swapchain.textures[RI.swapchainIndex],
								 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE));

				DrawPogoToTarget(pPogo, &RI.swapchainView[RI.swapchainIndex],
								 RI.swapchain.width, RI.swapchain.height,
								 (enum RI_Format_e)RI.swapchain.format, 1u,
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
