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
#include "graphics/Graphics.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"

#include "resources/Resources.h"

#include "graphics/Texture.h"

#include "scene/ParticleEmitter.h"
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
		: iRenderer("WireFrame",apGraphics, apResources)
	{
		// Standalone program (no bindless set) — mirrors the Interface<cGraphics>::Get()->gui load in
		// cGraphics::Init; per-draw state arrives via a frame-scratch UBO
		// ("pass", see Draw below).
		auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "wireframe.vert.spv");
		auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "wireframe.frag.spv");
		std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
		};
		m_wireframe.initialize(&mpGraphics->device, stages);
	}

	//-----------------------------------------------------------------------

	// cViewport::SimpleViewportState (shared with this renderer) is
	// implemented in RendererSimple.cpp.

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cRendererWireFrame::Draw(cGraphics::FrameContext* cntx,
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
		// scope. No BuildBlas: wireframe never traces.
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				if(listType == eRenderListType_Translucent)
				{
					// Particle emitters take the per-viewport scratch path in
					// the draw loop (BuildViewportVertices straight into
					// Interface<cGraphics>::Get()->translucentVtx/Idx segments) — panes must NOT write
					// the persistent VB: its uploader copies coalesce in the
					// fenced pre-pass, so the last pane would win for EVERY
					// pane (and poison the hybrid view).
					if(pObject->GetRenderType() == eRenderableType_ParticleEmitter)
						continue;

					// Dynamic translucent geometry (billboards/beams) still
					// rebuilds its vertex data per viewport before upload.
					// TODO: same shared-buffer disease as particles — migrate
					// to the scratch path.
					pObject->UpdateGraphicsForFrame(afFrameTime);
					pObject->UpdateGraphicsForViewport(apFrustum, afFrameTime);
				}

				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<cVertexBuffer*>(pVB);
				vbri->SubmitToGPU(&mpGraphics->blasSubmit.cmds[0], &mpGraphics->device, cntx);
			}
		}

		// Target-agnostic: the viewport resolves its extent from its Target
		// (swapchain extent for TargetSwapchain, the view's for TargetView)
		// and PrepareToRender configures this backend's state for it. This
		// renderer draws 1:1 — no guard band.
		const cVector2l vTargetSize = viewport->GetTargetSize();
		if(vTargetSize.x <= 0 || vTargetSize.y <= 0) {
			return;
		}
		const uint32_t renderWidth = (uint32_t)vTargetSize.x;
		const uint32_t renderHeight = (uint32_t)vTargetSize.y;

		////////////////////////////////////////////
		// Viewport-owned targets: PrepareToRender configures this backend's
		// state (1:1 color render target + depth) for the target size. cScene
		// feeds the finished render target into the viewport pogo afterwards.
		cViewport::SimpleViewportState *pState =
			viewport->PrepareToRender<cViewport::SimpleViewportState>(cntx);
		if(pState == nullptr || pState->width == 0) {
			return;
		}
		cViewport::SimpleViewportState &state = *pState;

		////////////////////////////////////////////
		// Pre-render transitions: render target UNDEFINED -> COLOR (contents
		// discarded, we clear) and depth UNDEFINED -> DEPTH_ATTACHMENT
		// (cleared via loadOp).
		{
			RITextureBarrier barriers[2] = {};
			barriers[0] = RI_PogoAttachmentBarrier(
				state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/true);

			barriers[1].texture = state.depthTextures[mpGraphics->swapchainIndex].Get();
			barriers[1].before = RI_RESOURCE_STATE_UNDEFINED;
			barriers[1].after = RI_RESOURCE_STATE_DEPTH_WRITE;
			barriers[1].aspect = RI_BARRIER_ASPECT_DEPTH;
			barriers[1].mipCount = 1;
			barriers[1].layerCount = 1;

			mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<2>(2, barriers);
		}

		////////////////////////////////////////////
		// Begin rendering into the state's render target at its 1:1 extent —
		// no overscan; cScene's pogo feed consumes it afterwards.
		{

			RIRenderingAttachment color = {};
			color.view = *state.renderTargetView[mpGraphics->swapchainIndex];
			color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			color.clearValue.color[0] = 0.0f;
			color.clearValue.color[1] = 0.0f;
			color.clearValue.color[2] = 0.0f;
			color.clearValue.color[3] = 1.0f;

			RIRenderingAttachment depth = {};
			depth.view = *state.depthView[mpGraphics->swapchainIndex];
			depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			depth.clearValue.depth = 1.0f;

			// renderArea fields are int16_t; viewport sizes stay well under 32767.
			RIBeginRenderingDesc beginDesc = {};
			beginDesc.renderArea.x = 0;
			beginDesc.renderArea.y = 0;
			beginDesc.renderArea.width = (int16_t)renderWidth;
			beginDesc.renderArea.height = (int16_t)renderHeight;
			beginDesc.colorCount = 1;
			beginDesc.colors = &color;
			beginDesc.depthStencil = &depth;
			mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);
		}

		// Y-flipped viewport — same convention as the forward passes, so the
		// unmodified projection matrix lands the right way up.
		VkViewport viewport_vk = { 0.0f, (float)renderHeight,
								   (float)renderWidth, -(float)renderHeight,
								   0.0f, 1.0f };
		VkRect2D scissor = { { 0, 0 }, { renderWidth, renderHeight } };
		vkCmdSetViewport(mpGraphics->primary.cmds[0].vk.cmd, 0, 1, &viewport_vk);
		vkCmdSetScissor(mpGraphics->primary.cmds[0].vk.cmd, 0, 1, &scissor);

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
			VkFormat colorFormats[1] = { RIFormatToVK(cGraphics::PogoColorFormat) };
			pipelineRenderingCreateInfo.colorAttachmentCount = 1;
			pipelineRenderingCreateInfo.pColorAttachmentFormats = colorFormats;
			pipelineRenderingCreateInfo.depthAttachmentFormat = RIFormatToVK(cGraphics::DepthFormat);
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
			m_wireframe.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], kHash, "wireframe", &pipelineCreateInfo);
		}

		////////////////////////////////////////////
		// Draw every list with the same flat pipeline. Per-object MVP flows
		// through a frame-scratch UBO slice (Interface<cGraphics>::Get()->UpdateFrameUBO), GUI-style.
		const ml::float4x4 viewProjMtx = apFrustum->GetProjectionMat() * apFrustum->GetViewMat();
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				////////////////////////////////////////////
				// Particles: per-viewport scratch path. Build this pane's
				// camera-facing quads straight into per-frame segments of
				// Interface<cGraphics>::Get()->translucentVtx/IdxBuffer and bind them at byte offsets —
				// each pane gets its own correctly-billboarded geometry (the
				// persistent VB stays the hybrid renderer's).
				if(listType == eRenderListType_Translucent &&
				   pObject->GetRenderType() == eRenderableType_ParticleEmitter)
				{
					// Build this pane's camera-facing quads into the shared
					// per-frame scratch (one copy per viewport, billboarded to
					// its own camera). withUv=false — this pipeline reads only
					// position.
					auto *pEmitter = static_cast<iParticleEmitter*>(pObject);
					auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime, /*withUv=*/false);
					if(!geom.valid) continue;

					// Particle verts are in VIEW space; GetModelMatrix returns
					// the inverse view so MVP collapses to the projection.
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
					mpGraphics->UpdateFrameUBO(&binding.descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
					binding.handle = DescriptorBindingID::Create("pass");
					m_wireframe.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex, &binding, 1);

					RIBuffer *vertBufs[1] = { mpGraphics->translucentVtxBuffer.Get() };
					const VkDeviceSize vertOffsets[1] = { (VkDeviceSize)geom.posByteOffset };
					mpGraphics->primary.cmds[0].bindVertexBuffers<1>(0, 1, vertBufs, vertOffsets);
					mpGraphics->primary.cmds[0].bindIndexBuffer(&mpGraphics->device, mpGraphics->translucentIdxBuffer.Get(),
													   (VkDeviceSize)geom.idxByteOffset,
													   RI_INDEX_TYPE_32);
					mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, geom.indexCount, 1, 0, 0, 0);
					continue;
				}

				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<cVertexBuffer*>(pVB);
				const auto *posElement = vbri->GetElement(eVertexBufferElement_Position);
				const auto &indexBuffer = vbri->GetIndexRIBuffer();
				const int indexCount = vbri->GetIndexNum();
				if(posElement == NULL || !posElement->GetBuffer() || !indexBuffer || indexCount <= 0) {
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
				mpGraphics->UpdateFrameUBO(&binding.descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
				binding.handle = DescriptorBindingID::Create("pass");
				m_wireframe.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex, &binding, 1);

				RIBuffer *vertBufs[1] = { posElement->GetBuffer() };
				const VkDeviceSize vertOffsets[1] = { 0 };
				mpGraphics->primary.cmds[0].bindVertexBuffers<1>(0, 1, vertBufs, vertOffsets);
				mpGraphics->primary.cmds[0].bindIndexBuffer(&mpGraphics->device, indexBuffer, 0, RI_INDEX_TYPE_32);
				mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1, 0, 0, 0);
			}
		}

		////////////////////////////////////////////
		// Offscreen panes: overlay the queued DebugDraw requests (grid, axes,
		// gizmos — enqueued by the editor's OnPreWorldDraw callbacks) into the
		// same rendering scope before it closes. Same color/depth formats, so
		// the batcher's pipelines match.
		DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
		if(debugDraw && debugDraw->HasRequests())
		{
			debugDraw->flush(cntx, &mpGraphics->primary.cmds[0], apFrustum, renderWidth,
							 renderHeight, cGraphics::PogoColorFormat);
		}

		mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

		////////////////////////////////////////////
		// Hand off: render target COLOR -> SHADER_READ — the finished frame
		// cScene feeds into the viewport pogo (post processing) and delivers
		// to the Target.
		{
			mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
				state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
		}
	}


	//-----------------------------------------------------------------------

}
