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

#include "scene/Scene.h"

#include "scene/Viewport.h"
#include "scene/Camera.h"
#include "scene/World.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/Script.h"
#include "system/Platform.h"

#include "resources/Resources.h"
#include "resources/ScriptManager.h"
#include "resources/FileSearcher.h"
#include "resources/WorldLoaderHandler.h"

#include "graphics/DebugDraw.h"
#include "graphics/Graphics.h"
#include "graphics/Renderer.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIPogoBuffer.h"
#include "system/Hasher.h"
#include "graphics/LowLevelGraphics.h"
#include "graphics/RenderList2.h"

#include "sound/Sound.h"
#include "sound/LowLevelSound.h"
#include "sound/SoundHandler.h"

#include "gui/Gui.h"
#include "gui/GuiSet.h"

#include "physics/Physics.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cScene::cScene(cGraphics *apGraphics,cResources *apResources, cSound* apSound,cPhysics *apPhysics,
					cSystem *apSystem, cAI *apAI,cGui *apGui, cHaptic *apHaptic)
		: iUpdateable("HPL_Scene")
	{
		mpGraphics = apGraphics;
		mpResources = apResources;
		mpSound = apSound;
		mpPhysics = apPhysics;
		mpSystem = apSystem;
		mpAI = apAI;
		mpGui = apGui;
		mpHaptic = apHaptic;

		mpCurrentListener = NULL;
	}

	//-----------------------------------------------------------------------

	cScene::~cScene()
	{
		Log("Exiting Scene Module\n");
		Log("--------------------------------------------------------\n");

		STLDeleteAll(mlstViewports);
		STLDeleteAll(mlstWorlds);
		STLDeleteAll(mlstCameras);

		Log("--------------------------------------------------------\n\n");

	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	cViewport* cScene::CreateViewport(cCamera *apCamera, cWorld *apWorld, bool abPushFront)
	{
		cViewport *pViewport = hplNew ( cViewport, (this) );	

		pViewport->SetCamera(apCamera);
		pViewport->SetWorld(apWorld);
		pViewport->SetSize(-1);
		pViewport->SetRenderer(mpGraphics->GetRenderer(eRenderer_Main));

		if (abPushFront) {
			mlstViewports.push_front(pViewport);
		} else {
			mlstViewports.push_back(pViewport);
		}

		return pViewport;
	}
	
	//-----------------------------------------------------------------------

	void cScene::DestroyViewport(cViewport* apViewPort)
	{
		STLFindAndDelete(mlstViewports, apViewPort);
	}

	//-----------------------------------------------------------------------

	bool cScene::ViewportExists(cViewport* apViewPort)
	{
		for(tViewportListIt it = mlstViewports.begin(); it != mlstViewports.end(); ++it)
		{
			if(apViewPort == *it) return true;
		}

		return false;
	}

	//-----------------------------------------------------------------------

	cViewport* cScene::GetPrimaryViewport()
	{
		// The viewport whose backbuffer is composited to the swapchain: the
		// single visible viewport whose Target is TargetSwapchain (the
		// CreateViewport default). TargetView viewports (editor panes /
		// previews) are standalone — delivered/sampled per their target.
		cViewport *pPrimary = NULL;
		for(cViewport *pViewPort : mlstViewports)
		{
			if(pViewPort->IsVisible()==false) continue;
			if(std::holds_alternative<cViewport::TargetSwapchain>(pViewPort->GetTarget())==false) continue;

			assert(pPrimary == NULL &&
				   "multiple visible swapchain-compositing viewports");
			pPrimary = pViewPort;
#ifdef NDEBUG
			break;
#endif
		}
		return pPrimary;
	}

	//-----------------------------------------------------------------------

	void cScene::SetCurrentListener(cViewport* apViewPort)
	{
		//If there was a previous listener make sure that world is not a listener.
		if(mpCurrentListener != NULL && ViewportExists(mpCurrentListener))
		{
			mpCurrentListener->SetIsListener(false);
			cWorld *pWorld = mpCurrentListener->GetWorld();
			if(pWorld && WorldExists(pWorld)) pWorld->SetIsSoundEmitter(false);
		}
		
		mpCurrentListener = apViewPort;
		if(mpCurrentListener)
		{
			mpCurrentListener->SetIsListener(true);
			cWorld *pWorld = mpCurrentListener->GetWorld();
			if(pWorld) pWorld->SetIsSoundEmitter(true);
		}
	}

	//-----------------------------------------------------------------------

	cCamera* cScene::CreateCamera(eCameraMoveMode aMoveMode)
	{
		cCamera *pCamera = hplNew( cCamera, () );
		pCamera->SetAspect(mpGraphics->GetLowLevel()->GetScreenSizeFloat().x /
							mpGraphics->GetLowLevel()->GetScreenSizeFloat().y);

		//Add Camera to list
		mlstCameras.push_back(pCamera);

		return pCamera;
	}


	//-----------------------------------------------------------------------

	void cScene::DestroyCamera(cCamera* apCam)
	{
		STLFindAndDelete(mlstCameras, apCam);
	}

	//-----------------------------------------------------------------------

	void cScene::Render(float afFrameTime, tFlag alFlags)
	{
		//Increase the frame count (do this at top, so render count is valid until this Render is called again!)
		iRenderer::IncRenderFrameCount();
		{
			RIBootstrap::FrameContext* cntx = RI.GetActiveSet();
			tViewportListIt viewIt = mlstViewports.begin();

			for(; viewIt != mlstViewports.end(); ++viewIt)
			{
					cViewport *pViewPort = *viewIt;
					if(pViewPort->IsVisible()==false) continue;

					iRenderer* pRenderer = pViewPort->GetRenderer();
					cCamera* pCamera = pViewPort->GetCamera();
					cFrustum* pFrustum = pCamera ? pCamera->GetFrustum() : NULL;

					const bool worldRendered =
							(alFlags & tSceneRenderFlag_World) &&
							pRenderer && pViewPort->GetWorld() && pFrustum;

					if(alFlags & tSceneRenderFlag_World)
					{
						pViewPort->RunViewportCallbackMessage(eViewportMessage_OnPreWorldDraw);
						if (worldRendered) {
							pRenderer->Draw(
									cntx,
									pViewPort,
									afFrameTime,
									pFrustum,
									pViewPort->GetWorld(),
									pViewPort->GetRenderSettings(),
									false);
						}
						pViewPort->RunViewportCallbackMessage(eViewportMessage_OnPostWorldDraw);
					}

					// FEED + POST: once the world draw fully evaluated the viewport,
					// blit the backend's BackBuffer window (the crop is baked in —
					// the hybrid backend overdraws by its guard band) into the
					// viewport pogo READ half, prep the ATTACH half, and run the
					// post-effect chain on the pogo (each effect samples the read
					// half, renders the attach half, and toggles). Delivery happens
					// after, per the viewport's Target.
					cViewport::BackBuffer backBuffer = pViewPort->GetBackBuffer();
					RI_PogoBuffer *pPogo = nullptr;
					if(worldRendered && backBuffer.renderTarget.vk.image != VK_NULL_HANDLE)
					{
						const cVector2l vTargetSize = pViewPort->GetTargetSize();
						pPogo = pViewPort->PreparePogoBuffer(cntx, (uint32_t)vTargetSize.x,
						                                     (uint32_t)vTargetSize.y);

						const uint32_t readIdx = (pPogo->attachmentIndex + 1u) % 2u;
						VkImage srcImage = backBuffer.renderTarget.vk.image;
						VkImage dstImage = pPogo->textures[readIdx].vk.image;
						VkImage attachImage = pPogo->textures[pPogo->attachmentIndex].vk.image;

						// Pre-blit: BackBuffer (left SHADER_READ by the renderer) ->
						// TRANSFER_SRC, pogo read half -> TRANSFER_DST (UNDEFINED
						// discard — fully overwritten), pogo attach half UNDEFINED ->
						// COLOR so the post chain (which renders into the attach half
						// with no barrier of its own) finds it ready.
						{
							VkImageMemoryBarrier2 pre[3] = {};
							pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
							pre[0].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
							pre[0].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
							pre[0].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
							pre[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
							pre[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
							pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							pre[0].image = srcImage;
							pre[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

							pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
							pre[1].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
							pre[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
							pre[1].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
							pre[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
							pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
							pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
							pre[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							pre[1].image = dstImage;
							pre[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

							pre[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
							pre[2].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
							pre[2].srcAccessMask = VK_ACCESS_2_NONE;
							pre[2].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
							pre[2].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
							pre[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
							pre[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
							pre[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							pre[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							pre[2].image = attachImage;
							pre[2].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

							VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
							dep.imageMemoryBarrierCount = 3;
							dep.pImageMemoryBarriers = pre;
							vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
						}

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
						{
							VkImageMemoryBarrier2 post[2] = {};
							post[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
							post[0].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
							post[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
							post[0].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
							post[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
							post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
							post[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							post[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							post[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							post[0].image = srcImage;
							post[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

							post[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
							post[1].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
							post[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
							post[1].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
							post[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
							post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
							post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							post[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							post[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							post[1].image = dstImage;
							post[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

							VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
							dep.imageMemoryBarrierCount = 2;
							dep.pImageMemoryBarriers = post;
							vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
						}

						cPostEffectComposite *pComposite = pViewPort->GetPostEffectComposite();
						if(pComposite && (alFlags & tSceneRenderFlag_PostEffects) &&
						   pComposite->HasActiveEffects())
						{
							pComposite->Render(afFrameTime, &RI.primary.cmds[0], pPogo,
							                   (uint32_t)vTargetSize.x, (uint32_t)vTargetSize.y,
							                   RI.frameIndex);
						}
					}

					// Delivery — symmetric branch on the viewport's own Target
					// variant. TargetView (standalone, e.g. editor panes): a
					// fullscreen draw of the pogo read half into the
					// caller-provided view when one was set (contract:
					// color-attachable, RIBootstrap::PogoColorFormat; fully
					// rewritten every frame and left SHADER_READ_ONLY for the
					// consumer — the editor's pane Image samples it). No swapchain
					// composite or GUI for these.
					if(const auto *pView = std::get_if<cViewport::TargetView>(&pViewPort->GetTarget()))
					{
						if(pView->view.vk.image != VK_NULL_HANDLE &&
						   worldRendered && pPogo != nullptr)
						{
							// Caller texture: discard previous contents (fully
							// rewritten; also covers its first use) -> COLOR for
							// the delivery draw.
							{
								VkImageMemoryBarrier2 toColor = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
								toColor.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
								toColor.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
								toColor.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
								toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
								toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
								toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
								toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								toColor.image = pView->texture.vk.image;
								toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
								VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
								dep.imageMemoryBarrierCount = 1;
								dep.pImageMemoryBarriers = &toColor;
								vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
							}
							VkRenderingAttachmentInfo colorAttach = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
							colorAttach.imageView   = pView->view.vk.image;
							colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
							colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
							colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
							VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
							renderInfo.renderArea = { { 0, 0 }, { pView->width, pView->height } };
							renderInfo.layerCount = 1;
							renderInfo.colorAttachmentCount = 1;
							renderInfo.pColorAttachments    = &colorAttach;

							vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

							VkViewport vp = { 0.0f, 0.0f, (float)pView->width, (float)pView->height, 0.0f, 1.0f };
							vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
							VkRect2D scr = { { 0, 0 }, { pView->width, pView->height } };
							vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &scr);

							PostEffectPipelineState blitState{};
							InitPostEffectPipelineState(blitState, RIBootstrap::PogoColorFormatVk, false);
							const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, 2u);
							RI.postEffectBlit.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
							                               "PostEffect.targetViewBlit", &blitState.createInfo);

							RIDescriptor_s *samplerDesc = RI.resolve_filter_descriptor(
							    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
							    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
							RIProgram::DescriptorBinding bindings[2] = {};
							bindings[0].descriptor = *samplerDesc;
							bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
							bindings[1].descriptor = *RI_PogoBufferShaderResource(pPogo);
							bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
							RI.postEffectBlit.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, bindings, 2);

							vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3, 1, 0, 0);
							vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

							// Caller texture -> SHADER_READ_ONLY for the consumer
							// (the pane GUI samples it).
							{
								VkImageMemoryBarrier2 toRead = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
								toRead.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
								toRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
								toRead.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
								toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
								toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
								toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
								toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								toRead.image = pView->texture.vk.image;
								toRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
								VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
								dep.imageMemoryBarrierCount = 1;
								dep.pImageMemoryBarriers = &toRead;
								vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
							}
						}
					}
					// TargetSwapchain: composite to the swapchain — tail draw of
					// the (post-processed) pogo read half BEFORE the GUI overlays,
					// then the GUI block. At most one visible viewport may target
					// the swapchain; GetPrimaryViewport re-derives and asserts it.
					else if(std::holds_alternative<cViewport::TargetSwapchain>(pViewPort->GetTarget()))
					{
						assert(pViewPort == GetPrimaryViewport());
						if(worldRendered && pPogo != nullptr)
						{
							// Tail blit: pogo read half -> swapchain (UNDEFINED -> COLOR first).
							{
								VkImageMemoryBarrier2 swapBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
								swapBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
								swapBarrier.srcAccessMask = 0;
								swapBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
								swapBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
								swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
								swapBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
								swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
								swapBarrier.image = RI.swapchain.vk.images[RI.swapchainIndex];
								swapBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
								VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
								dep.imageMemoryBarrierCount = 1;
								dep.pImageMemoryBarriers    = &swapBarrier;
								vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
							}
							{
								VkRenderingAttachmentInfo colorAttach = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
								colorAttach.imageView   = RI.swapchainView[RI.swapchainIndex].vk.image;
								colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
								colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
								colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
								VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
								renderInfo.renderArea = { { 0, 0 }, { RI.swapchain.width, RI.swapchain.height } };
								renderInfo.layerCount = 1;
								renderInfo.colorAttachmentCount = 1;
								renderInfo.pColorAttachments    = &colorAttach;

								vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

								VkViewport vp = { 0.0f, 0.0f, (float)RI.swapchain.width, (float)RI.swapchain.height, 0.0f, 1.0f };
								vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
								VkRect2D scr = { { 0, 0 }, { RI.swapchain.width, RI.swapchain.height } };
								vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &scr);

								PostEffectPipelineState blitState{};
								InitPostEffectPipelineState(blitState, RIFormatToVK((RI_Format_e)RI.swapchain.format), false);
								const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, 1u);
								RI.postEffectBlit.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
								                               "PostEffect.tailBlit", &blitState.createInfo);

								RIDescriptor_s *samplerDesc = RI.resolve_filter_descriptor(
								    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
								    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
								RIProgram::DescriptorBinding bindings[2] = {};
								bindings[0].descriptor = *samplerDesc;
								bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
								bindings[1].descriptor = *RI_PogoBufferShaderResource(pPogo);
								bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
								RI.postEffectBlit.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, bindings, 2);

								vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3, 1, 0, 0);
								vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
							}
						}

						// GUI block: open a rendering instance with color (+ depth when the
						// viewport has one), render the GUIs, close it. When the world render
						// ran, Draw left the swapchain in COLOR_ATTACHMENT_OPTIMAL with the
						// composite inside — LOAD so the GUI overlays on top. Otherwise CLEAR
						// (also establishes UNDEFINED → COLOR transition). Depth LOADs when
						// present (the viewport's renderer populated it earlier); GUI-only
						// frames (menus) have no viewport depth and the sets render the
						// no-depth pipeline variant (they derive it from the viewport
						// passed into cGuiSet::Render).
						struct RITextureView_s *pGuiDepthView = pViewPort->GetDepthView();

						VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
						RI_VK_FillColorAttachmentView( &colorAttachment, &RI.swapchainView[RI.swapchainIndex] , !worldRendered );
						VkRenderingAttachmentInfo depthStencil = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
						if(pGuiDepthView)
						{
							RI_VK_FillDepthAttachment( &depthStencil, pGuiDepthView, false );
						}
						VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
						renderingInfo.renderArea = VkRect2D{ { 0, 0 }, { RI.swapchain.width, RI.swapchain.height } };
						renderingInfo.layerCount = 1;
						renderingInfo.colorAttachmentCount = 1;
						renderingInfo.pColorAttachments = &colorAttachment;
						renderingInfo.pDepthAttachment = pGuiDepthView ? &depthStencil : NULL;

						// GuiSet builds its pipelines for RI.swapchain.format / RIBootstrap::DepthFormat
						// (see GuiSet.cpp). If the attachments here ever change, update GuiSet to match.
						assert(colorAttachment.imageView == RI.swapchainView[RI.swapchainIndex].vk.image);

						vkCmdBeginRendering( RI.primary.cmds[0].vk.cmd , &renderingInfo );

						if(alFlags & tSceneRenderFlag_World)
						{
							START_TIMING(Render3DGui)
							Render3DGui(pViewPort, pFrustum, afFrameTime);
							STOP_TIMING(Render3DGui)
						}
						if(alFlags & tSceneRenderFlag_Gui)
						{
							START_TIMING(RenderGUI)
							RenderScreenGui(pViewPort, afFrameTime);
							STOP_TIMING(RenderGUI)
						}

						vkCmdEndRendering( RI.primary.cmds[0].vk.cmd );
					}
			}
		}


		/////////////////////////////////////////////
		//// Iterate all viewports and render
		//tViewportListIt viewIt = mlstViewports.begin();
		//for(; viewIt != mlstViewports.end(); ++viewIt)
		//{
		//	cViewport *pViewPort = *viewIt;
		//	if(pViewPort->IsVisible()==false) continue;

		//	//////////////////////////////////////////////
		//	//Init vars
		//	cPostEffectComposite *pPostEffectComposite = pViewPort->GetPostEffectComposite();
		//	bool bPostEffects = false;
		//	iRenderer *pRenderer = pViewPort->GetRenderer();
		//	cCamera *pCamera = pViewPort->GetCamera();
		//	cFrustum *pFrustum = pCamera ? pCamera->GetFrustum() : NULL;

		//	//////////////////////////////////////////////
		//	//Render world and call callbacks
		//	if(alFlags & tSceneRenderFlag_World)
		//	{
		//		pViewPort->RunViewportCallbackMessage(eViewportMessage_OnPreWorldDraw);
		//		
		//		if(pPostEffectComposite && (alFlags & tSceneRenderFlag_PostEffects)) 
		//		{
		//			bPostEffects = pPostEffectComposite->HasActiveEffects();
		//		}
		//		
		//		if(pRenderer && pViewPort->GetWorld() && pFrustum)
		//		{
		//			START_TIMING(RenderWorld)
		//			pRenderer->Render(	afFrameTime,pFrustum,
		//								pViewPort->GetWorld(),pViewPort->GetRenderSettings(), 
		//								pViewPort->GetRenderTarget(),
		//								bPostEffects,
		//								pViewPort->GetRendererCallbackList());
		//			STOP_TIMING(RenderWorld)
		//		}
		//		else
		//		{
		//			//If no renderer sets up viewport do that by our selves.
		//			cRenderTarget* pRenderTarget = pViewPort->GetRenderTarget();
		//			mpGraphics->GetLowLevel()->SetCurrentFrameBuffer(	pRenderTarget->mpFrameBuffer,
		//																pRenderTarget->mvPos,
		//																pRenderTarget->mvSize);
		//		}
		//		pViewPort->RunViewportCallbackMessage(eViewportMessage_OnPostWorldDraw);

		//		//////////////////////////////////////////////
		//		//Render 3D GuiSets
		//		// Should this really be here? Or perhaps send in a frame buffer depending on the renderer.
		//		START_TIMING(Render3DGui)
		//		Render3DGui(pViewPort,pFrustum, afFrameTime);
		//		STOP_TIMING(Render3DGui)
		//	}

		//	//////////////////////////////////////////////
		//	//Render Post effects
		//	if(bPostEffects)
		//	{
		//		//TODO: If renderer is null get texture from frame buffer and if frame buffer is NULL, then copy to a texture.
		//		//		Or this is solved?
		//		iTexture *pInputTexture = pRenderer->GetPostEffectTexture();

		//		START_TIMING(RenderPostEffects)
		//		pPostEffectComposite->Render(afFrameTime, pFrustum, pInputTexture,pViewPort->GetRenderTarget());
		//		STOP_TIMING(RenderPostEffects)
		//	}
		//	
		//	//////////////////////////////////////////////
		//	//Render Screen GUI
		//	if(alFlags & tSceneRenderFlag_Gui)
		//	{
		//		START_TIMING(RenderGUI)
		//		RenderScreenGui(pViewPort, afFrameTime);
		//		STOP_TIMING(RenderGUI)
		//	}
		//}

	}

	//-----------------------------------------------------------------------

	void cScene::PostUpdate(float afTimeStep)
	{
		//////////////////////////////////////
		//Update worlds
		tWorldListIt it = mlstWorlds.begin();
		for(; it != mlstWorlds.end(); ++it)
		{
			cWorld *pWorld = *it;
            if(pWorld->IsActive()) pWorld->Update(afTimeStep);
		}


		//////////////////////////////////////
		//Update listener position with current listener, if there is one.
		if(mpCurrentListener && mpCurrentListener->GetCamera())
		{
			cCamera* pCamera3D = mpCurrentListener->GetCamera();
			mpSound->GetLowLevel()->SetListenerAttributes(	pCamera3D->GetPosition(), cVector3f(0,0,0),
															pCamera3D->GetForward()*-1.0f, pCamera3D->GetUp());
		}
	}

	//-----------------------------------------------------------------------

	void cScene::Reset()
	{
	}

	//-----------------------------------------------------------------------

	cWorld* cScene::LoadWorld(const tString& asFile, tWorldLoadFlag aFlags)
	{
		cHybridRenderer* pHybridRenderer = static_cast<cHybridRenderer*>(mpGraphics->GetRenderer(eRenderer_Main));
		cRenderList2* pRenderList = pHybridRenderer->GetRenderList();
		pRenderList->ClearPersistentObjectsForMapChange();

		///////////////////////////////////
		// Load the map file
		tWString asPath = mpResources->GetFileSearcher()->GetFilePath(asFile);
		if(asPath == _W(""))
		{
			if(cResources::GetCreateAndLoadCompressedMaps())
				asPath = mpResources->GetFileSearcher()->GetFilePath(cString::SetFileExt(asFile,"cmap"));
			
			if(asPath == _W(""))
			{
				Error("World '%s' doesn't exist\n",asFile.c_str());
				return NULL;
			}
		}

		cWorld* pWorld = mpResources->GetWorldLoaderHandler()->LoadWorld(asPath, aFlags);
		if(pWorld==NULL){
			Error("Couldn't load world from '%s'\n",cString::To8Char(asPath).c_str());
			return NULL;
		}

		return pWorld;
	}

	//-----------------------------------------------------------------------

	cWorld* cScene::CreateWorld(const tString& asName)
	{
		cWorld* pWorld = hplNew( cWorld, (asName,mpGraphics,mpResources,mpSound,mpPhysics,this,
										mpSystem,mpAI,mpHaptic) );

		mlstWorlds.push_back(pWorld);

		return pWorld;
	}

	//-----------------------------------------------------------------------

	void cScene::DestroyWorld(cWorld* apWorld)
	{
		STLFindAndDelete(mlstWorlds,apWorld);
	}

	//-----------------------------------------------------------------------

	bool cScene::WorldExists(cWorld* apWorld)
	{
		for(tWorldListIt it = mlstWorlds.begin(); it != mlstWorlds.end(); ++it)
		{
			if(apWorld == *it) return true;
		}

		return false;
	}
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cScene::Render3DGui(cViewport *apViewPort,cFrustum *apFrustum,float afTimeStep)
	{
		if(apViewPort->GetCamera()==NULL) return;

		cGuiSetListIterator it = apViewPort->GetGuiSetIterator();	
		while(it.HasNext())
		{
			cGuiSet *pSet = it.Next();
			if(pSet->Is3D())
			{
				pSet->Render(apFrustum, apViewPort);
			}
		}
	}
	
	void cScene::RenderScreenGui(cViewport *apViewPort,float afTimeStep)
	{
		///////////////////////////////////////
		//Put all of the non 3D sets in to a sorted map
		typedef std::multimap<int, cGuiSet*> tPrioMap;
		tPrioMap mapSortedSets;

    cGuiSetListIterator it = apViewPort->GetGuiSetIterator();	
		while(it.HasNext())
		{
			cGuiSet *pSet = it.Next();
			
			if(pSet->Is3D()==false)
				mapSortedSets.insert(tPrioMap::value_type(pSet->GetDrawPriority(),pSet));
		}

		///////////////////////////////////////
		//Iterate and render all sets
		if(mapSortedSets.empty()) return;
		tPrioMap::iterator SortIt = mapSortedSets.begin();
		for(; SortIt != mapSortedSets.end(); ++SortIt)
		{
			cGuiSet *pSet = SortIt->second;
			
			//Log("Rendering gui '%s'\n", pSet->GetName().c_str());

			pSet->Render(NULL, apViewPort);
		}
	}

	//-----------------------------------------------------------------------
}
