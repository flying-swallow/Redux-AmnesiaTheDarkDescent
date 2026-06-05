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

#include "graphics/RendererWireFrame.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "system/Hasher.h"

#include "graphics/Graphics.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Renderable.h"

#include "graphics/DebugDraw.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/RIViewportTarget.h"
#include "graphics/VertexBuffer_RI.h"

#include "resources/Resources.h"

#include "graphics/HPLTexture.h"

#include "scene/Viewport.h"
#include "scene/World.h"
#include "scene/RenderableContainer.h"

#include <cstring>

namespace hpl {

	namespace {
		// Mirrors WireFramePass in wireframe.{vert,frag}.slang (binding 0, set 0).
		struct WireFramePass {
			float mvp[16];
			float color[4];
		};
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cRendererWireFrame::cRendererWireFrame(cGraphics *apGraphics,cResources* apResources)
		: iRenderer("WireFrame",apGraphics, apResources,0)
	{
		// Standalone program (no bindless set) — mirrors the RI.gui load in
		// cGraphics::Init; per-draw state arrives via a frame-scratch UBO
		// ("pass", see Draw below).
		auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "wireframe.vert.spv");
		auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "wireframe.frag.spv");
		std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
		};
		m_wireframe.initialize(&RI.device, stages);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cRendererWireFrame::Draw(RIBootstrap::FrameContext* cntx,
								  cViewport* viewport,
								  float afFrameTime,
								  cFrustum* apFrustum,
								  cWorld* apWorld,
								  cRenderSettings* apSettings,
								  bool abSendFrameBufferToPostEffects)
	{
		////////////////////////////////////////////
		// Build the render list (same walk as cHybridRenderer::Draw, minus the
		// BLAS parking — nothing here is ray traced).
		m_rendererList.BeginAndReset(afFrameTime, apFrustum);
		auto *dynamicContainer = apWorld->GetRenderableContainer(eWorldContainerType_Dynamic);
		auto *staticContainer = apWorld->GetRenderableContainer(eWorldContainerType_Static);
		dynamicContainer->UpdateBeforeRendering();
		staticContainer->UpdateBeforeRendering();

		auto prepareObjectHandler = [&](iRenderable *pObject) {
			if(!rendering::IsObjectIsVisible(pObject, eRenderableFlag_VisibleInNonReflection, {})) {
				return;
			}
			m_rendererList.AddObject(pObject);
		};
		rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum, prepareObjectHandler,
											eRenderableFlag_VisibleInNonReflection);
		rendering::WalkAndPrepareRenderList(staticContainer, apFrustum, prepareObjectHandler,
											eRenderableFlag_VisibleInNonReflection);
		m_rendererList.End(eRenderListCompileFlag_Diffuse |
						   eRenderListCompileFlag_Decal |
						   eRenderListCompileFlag_Translucent);

		const eRenderListType lists[] = {
			eRenderListType_Diffuse,
			eRenderListType_Decal,
			eRenderListType_Translucent
		};

		////////////////////////////////////////////
		// Upload vertex/index streams. Must run BEFORE vkCmdBeginRendering — the
		// uploader records barriers that can't live inside a dynamic-rendering
		// scope. abBuildBlas=false: wireframe never traces.
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				// Dynamic translucent geometry (billboards/beams/particles)
				// rebuilds its vertex data per viewport before upload.
				if(listType == eRenderListType_Translucent)
				{
					pObject->UpdateGraphicsForFrame(afFrameTime);
					pObject->UpdateGraphicsForViewport(apFrustum, afFrameTime);
				}

				iVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<VertexBuffer_RI*>(pVB);
				vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx, /*abBuildBlas=*/false);
				vbri->AttachResourceToCntx(cntx);
			}
		}

		RI_PogoBuffer *pogo = &RI.pogoBuffer[RI.swapchainIndex];

		// Offscreen viewport target (editor ortho panes): render the pane's
		// 1:1 extent into the pogo attach half, overlay DebugDraw, then blit
		// the drawn window into the target's sampled texture — mirroring the
		// offscreen tail in cHybridRenderer::Draw. Panes always fit inside
		// the authored pogo (they tile the window).
		cViewportTarget *offTarget =
			viewport ? viewport->GetViewportTarget() : nullptr;
		if(offTarget && !offTarget->IsValid()) {
			offTarget = nullptr;
		}
		const uint32_t renderWidth =
			offTarget ? offTarget->GetWidth() : RI.swapchain.width;
		const uint32_t renderHeight =
			offTarget ? offTarget->GetHeight() : RI.swapchain.height;
		assert(renderWidth <= RI.swapchain.width && renderHeight <= RI.swapchain.height &&
			   "offscreen target larger than the authored pogo");

		////////////////////////////////////////////
		// Pre-render transitions: pogo attach half UNDEFINED -> COLOR (contents
		// discarded, we clear) and depth UNDEFINED -> DEPTH_ATTACHMENT (cleared
		// via loadOp; also the layout Scene's GUI block expects afterwards).
		{
			VkImageMemoryBarrier2 barriers[2] = {};
			barriers[0] = VK_RI_PogoAttachmentMemoryBarrier2(
				pogo->textures[pogo->attachmentIndex].vk.image, /*initial=*/true);

			barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
			barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
			barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
									   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
										VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[1].image = RI.depthTextures[RI.swapchainIndex].vk.image;
			barriers[1].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

			VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.imageMemoryBarrierCount = 2;
			dep.pImageMemoryBarriers = barriers;
			vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
		}

		////////////////////////////////////////////
		// Begin rendering into the pogo attach half at authored (swapchain)
		// size — no overscan; Scene's tail blit samples the read half 1:1.
		{
			VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			colorAttachment.imageView = RI_PogoBufferAttachment(pogo)->vk.image.imageView;
			colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

			VkRenderingAttachmentInfo depthStencil = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			RI_VK_FillDepthAttachment(&depthStencil, &RI.depthView[RI.swapchainIndex], /*attachAndClear=*/true);

			VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			renderingInfo.renderArea = VkRect2D{ { 0, 0 }, { renderWidth, renderHeight } };
			renderingInfo.layerCount = 1;
			renderingInfo.colorAttachmentCount = 1;
			renderingInfo.pColorAttachments = &colorAttachment;
			renderingInfo.pDepthAttachment = &depthStencil;
			vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderingInfo);
		}

		// Y-flipped viewport — same convention as the forward passes, so the
		// unmodified projection matrix lands the right way up.
		VkViewport viewport_vk = { 0.0f, (float)renderHeight,
								   (float)renderWidth, -(float)renderHeight,
								   0.0f, 1.0f };
		VkRect2D scissor = { { 0, 0 }, { renderWidth, renderHeight } };
		vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &viewport_vk);
		vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &scissor);

		////////////////////////////////////////////
		// Pipeline: position-only fetch, triangle list rasterised as lines.
		// Formats are constexpr so a single cached variant suffices.
		{
			VkVertexInputAttributeDescription vertexAttributeDesc[] = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 } // position (engine stream is float4, stride 16)
			};
			VkVertexInputBindingDescription vertexBindingDesc[] = {
				{ 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }
			};
			VkPipelineVertexInputStateCreateInfo vertexInputState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vertexInputState.pVertexAttributeDescriptions = vertexAttributeDesc;
			vertexInputState.vertexAttributeDescriptionCount = ARRAY_COUNT(vertexAttributeDesc);
			vertexInputState.pVertexBindingDescriptions = vertexBindingDesc;
			vertexInputState.vertexBindingDescriptionCount = ARRAY_COUNT(vertexBindingDesc);

			VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			VkPipelineRasterizationStateCreateInfo rasterizationState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rasterizationState.polygonMode = VK_POLYGON_MODE_LINE;
			rasterizationState.cullMode = VK_CULL_MODE_NONE;
			rasterizationState.lineWidth = 1.0f;

			VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
			dynamicState.pDynamicStates = dynamicStates;

			VkPipelineRenderingCreateInfo pipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
			VkFormat colorFormats[1] = { RIFormatToVK(RIBootstrap::PogoColorFormat) };
			pipelineRenderingCreateInfo.colorAttachmentCount = 1;
			pipelineRenderingCreateInfo.pColorAttachmentFormats = colorFormats;
			pipelineRenderingCreateInfo.depthAttachmentFormat = RIFormatToVK(RIBootstrap::DepthFormat);
			pipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

			VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			VkPipelineMultisampleStateCreateInfo multisampleState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineDepthStencilStateCreateInfo depthStencilState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			depthStencilState.depthTestEnable = VK_TRUE;
			depthStencilState.depthWriteEnable = VK_TRUE;
			depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
			depthStencilState.minDepthBounds = 0.0f;
			depthStencilState.maxDepthBounds = 1.0f;

			VkPipelineColorBlendAttachmentState blendAttachmentState[] = { {
				VK_FALSE,
				VK_BLEND_FACTOR_ONE,
				VK_BLEND_FACTOR_ZERO,
				VK_BLEND_OP_ADD,
				VK_BLEND_FACTOR_ONE,
				VK_BLEND_FACTOR_ZERO,
				VK_BLEND_OP_ADD,
				VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
			} };
			VkPipelineColorBlendStateCreateInfo colorBlendState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			colorBlendState.attachmentCount = ARRAY_COUNT(blendAttachmentState);
			colorBlendState.pAttachments = blendAttachmentState;

			VkGraphicsPipelineCreateInfo pipelineCreateInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			pipelineCreateInfo.pNext = &pipelineRenderingCreateInfo;
			pipelineCreateInfo.pVertexInputState = &vertexInputState;
			pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
			pipelineCreateInfo.pRasterizationState = &rasterizationState;
			pipelineCreateInfo.pDynamicState = &dynamicState;
			pipelineCreateInfo.pViewportState = &viewportState;
			pipelineCreateInfo.pMultisampleState = &multisampleState;
			pipelineCreateInfo.pDepthStencilState = &depthStencilState;
			pipelineCreateInfo.pColorBlendState = &colorBlendState;

			const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, 0u);
			m_wireframe.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash, "wireframe", &pipelineCreateInfo);
		}

		////////////////////////////////////////////
		// Draw every list with the same flat pipeline. Per-object MVP flows
		// through a frame-scratch UBO slice (RI.UpdateFrameUBO), GUI-style.
		const ml::float4x4 viewProjMtx = apFrustum->GetProjectionMat() * apFrustum->GetViewMat();
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;
				iVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<VertexBuffer_RI*>(pVB);
				const auto *posElement = vbri->GetElement(eVertexBufferElement_Position);
				const auto &indexBuffer = vbri->GetIndexRIBuffer();
				const int indexCount = vbri->GetIndexNum();
				if(posElement == NULL || !posElement->buffer || !indexBuffer || indexCount <= 0) {
					continue;
				}

				cMatrixf *pModelMtx = pObject->GetModelMatrix(apFrustum);
				const ml::float4x4 mvp = viewProjMtx *
					cMath::ToFloatTranspose4x4(pModelMtx ? *pModelMtx : cMatrixf::Identity);

				WireFramePass uniformBlock = {};
				std::memcpy(uniformBlock.mvp, mvp.a, sizeof(uniformBlock.mvp));
				uniformBlock.color[0] = 1.0f;
				uniformBlock.color[1] = 1.0f;
				uniformBlock.color[2] = 1.0f;
				uniformBlock.color[3] = 1.0f;

				RIProgram::DescriptorBinding binding = {};
				RI.UpdateFrameUBO(&binding.descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
				binding.handle = DescriptorBindingID::Create("pass");
				m_wireframe.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, &binding, 1);

				RIBuffer_s *vertBufs[1] = { posElement->buffer.get() };
				const VkDeviceSize vertOffsets[1] = { 0 };
				CmdBindVertexBuffers<1>(&RI.primary.cmds[0], 0, 1, vertBufs, vertOffsets);
				CmdBindIndexBuffer(&RI.primary.cmds[0], indexBuffer.get(), 0, VK_INDEX_TYPE_UINT32);
				CmdDrawIndexed(&RI.primary.cmds[0], (uint32_t)indexCount, 1, 0, 0, 0);
			}
		}

		////////////////////////////////////////////
		// Offscreen panes: overlay the queued DebugDraw requests (grid, axes,
		// gizmos — enqueued by the editor's OnPreWorldDraw callbacks) into the
		// same rendering scope before it closes. Same color/depth formats, so
		// the batcher's pipelines match.
		DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
		if(offTarget && debugDraw && debugDraw->HasRequests())
		{
			debugDraw->flush(cntx, &RI.primary.cmds[0], apFrustum, renderWidth,
							 renderHeight, RIBootstrap::PogoColorFormatVk);
		}

		vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

		////////////////////////////////////////////
		// Offscreen delivery: blit the drawn {0,0,w,h} window into the
		// viewport target's sampled texture (left SHADER_READ_ONLY for the
		// GUI), then restore the pogo invariants exactly like the swapchain
		// handoff below — drawn half ends SHADER_READ, other half COLOR.
		if(offTarget)
		{
			const uint16_t drawnIdx = pogo->attachmentIndex;
			const uint16_t nextIdx = (drawnIdx + 1) % 2;
			VkImage srcImage = pogo->textures[drawnIdx].vk.image;
			VkImage dstImage = offTarget->GetTexture()->handle.vk.image;

			// Pin the target texture so a mid-flight Resize can't free it
			// before this frame's GPU work completes.
			cntx->textureLink.push_back(offTarget->GetTexture());

			{
				VkImageMemoryBarrier2 pre[2] = {};
				pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				pre[0].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				pre[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
				pre[0].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
				pre[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
				pre[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				pre[0].image = srcImage;
				pre[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

				// Fully overwritten every frame — UNDEFINED discard also covers
				// the image's very first use.
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

				VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dep.imageMemoryBarrierCount = 2;
				dep.pImageMemoryBarriers = pre;
				vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
			}

			VkImageBlit region = {};
			region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.srcOffsets[0]  = { 0, 0, 0 };
			region.srcOffsets[1]  = { (int32_t)renderWidth, (int32_t)renderHeight, 1 };
			region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.dstOffsets[0]  = { 0, 0, 0 };
			region.dstOffsets[1]  = { (int32_t)renderWidth, (int32_t)renderHeight, 1 };
			vkCmdBlitImage(RI.primary.cmds[0].vk.cmd, srcImage,
						   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
						   VK_FILTER_NEAREST);

			{
				VkImageMemoryBarrier2 post[3] = {};
				// Drawn half -> SHADER_READ (the pogo invariant the next user
				// expects of the read half).
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

				// Target -> SHADER_READ_ONLY for GUI sampling.
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

				// Other half -> COLOR, ready as the next attach half.
				post[2] = VK_RI_PogoAttachmentMemoryBarrier2(pogo->textures[nextIdx].vk.image, /*initial=*/true);

				VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dep.imageMemoryBarrierCount = 3;
				dep.pImageMemoryBarriers = post;
				vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
			}

			pogo->attachmentIndex = nextIdx;
			return;
		}

		////////////////////////////////////////////
		// Hand off: drawn half COLOR -> SHADER_READ (becomes the read half the
		// post-effects/tail blit sample) and the other half UNDEFINED -> COLOR
		// (the post-effect chain renders into the attach half with no barrier
		// of its own).
		{
			const uint16_t drawnIdx = pogo->attachmentIndex;
			const uint16_t nextIdx = (drawnIdx + 1) % 2;

			VkImageMemoryBarrier2 barriers[2] = {};
			barriers[0] = VK_RI_PogoShaderMemoryBarrier2(pogo->textures[drawnIdx].vk.image, /*initial=*/false);
			barriers[1] = VK_RI_PogoAttachmentMemoryBarrier2(pogo->textures[nextIdx].vk.image, /*initial=*/true);

			VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.imageMemoryBarrierCount = 2;
			dep.pImageMemoryBarriers = barriers;
			vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);

			pogo->attachmentIndex = nextIdx;
		}
	}


	//-----------------------------------------------------------------------

}
