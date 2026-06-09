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

#include "graphics/RendererSimple.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "system/Hasher.h"

#include "graphics/Graphics.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/Renderable.h"

#include "graphics/DebugDraw.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer_RI.h"

#include "resources/Resources.h"

#include "graphics/HPLTexture.h"

#include "scene/ParticleEmitter.h"
#include "scene/Viewport.h"
#include "scene/World.h"
#include "scene/RenderableContainer.h"

#include <cstring>

namespace hpl {

	namespace {
		// Mirrors SimplePass in Simple.{vert,frag}.slang (binding 0, set 0).
		struct SimplePass {
			float mvp[16];
			float uvMtx[16];
			float color[4];
			float alphaReject;
			float pad[3];
		};

		// Pipeline variants, folded into the cache hash. OPAQUE is the diffuse
		// list (blend off, depth write on); the rest are the decal/translucent
		// hardware blend modes (depth read-only ≤), same eMaterialBlendMode
		// remap as the particle/decal passes in cHybridRenderer.
		enum SimpleBlendVariant : uint32_t {
			SIMPLE_OPAQUE = 0,
			SIMPLE_BLEND_ADD,
			SIMPLE_BLEND_MUL,
			SIMPLE_BLEND_MULX2,
			SIMPLE_BLEND_ALPHA,
			SIMPLE_BLEND_PREMUL_ALPHA,
		};

		SimpleBlendVariant remapBlend(eMaterialBlendMode aMode) {
			switch(aMode) {
			case eMaterialBlendMode_Add:         return SIMPLE_BLEND_ADD;
			case eMaterialBlendMode_Mul:         return SIMPLE_BLEND_MUL;
			case eMaterialBlendMode_MulX2:       return SIMPLE_BLEND_MULX2;
			case eMaterialBlendMode_PremulAlpha: return SIMPLE_BLEND_PREMUL_ALPHA;
			case eMaterialBlendMode_Alpha:
			default:                             return SIMPLE_BLEND_ALPHA;
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cRendererSimple::cRendererSimple(cGraphics *apGraphics,cResources* apResources)
		: iRenderer("Simple",apGraphics, apResources,0)
	{
		// Standalone program (no bindless set) — mirrors cRendererWireFrame;
		// per-draw state arrives via a frame-scratch UBO ("pass") plus a
		// per-draw diffuse texture + sampler (DebugDraw-style named bindings).
		auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "Simple.vert.spv");
		auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "Simple.frag.spv");
		std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
		};
		m_simple.initialize(&RI.device, stages);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------


	//-----------------------------------------------------------------------

	// cViewport::SimpleViewportState — the viewport holds the state, this
	// backend refines it (shared by cRendererWireFrame + cRendererSimple): a
	// 1:1 color render target the renderer draws into plus a matching depth
	// attachment. cScene feeds the finished render target (GetBackBuffer)
	// into the viewport pogo in the post-processing step. Replacement/resize
	// hand the old targets to the frame freelist (drained once the pipeline
	// is done with them) — no stall.
	void cViewport::SimpleViewportState::Update(RIBootstrap::FrameContext *cntx, cVector2l size)
	{
		if(size.x <= 0 || size.y <= 0)
		{
			return;
		}
		const uint32_t w = (uint32_t)size.x;
		const uint32_t h = (uint32_t)size.y;
		if(width == w && height == h)
		{
			return;
		}

		Dispose(cntx);

		width = w;
		height = h;
		for(uint32_t i = 0; i < RI.swapchain.imageCount; i++)
		{
			// 1:1 color target: drawn by the renderer, sampled by cScene's
			// pogo feed (TRANSFER_SRC backs the feed blit).
			CreateViewportColorTexture(
				&RI.device, w, h, RIBootstrap::PogoColorFormat,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
					VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				&renderTarget[i], &renderTargetDescriptor[i],
				"SimpleViewportState.renderTarget");

			CreateViewportAttachmentTexture(
				&RI.device, w, h, RIBootstrap::DepthFormat,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT, &depthTextures[i], &depthView[i],
				"SimpleViewportState.depth");
		}
	}

	void cViewport::SimpleViewportState::Dispose(RIBootstrap::FrameContext *cntx)
	{
		for(uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++)
		{
			ReleaseViewportColorTexture(cntx->freelist, &renderTarget[i], &renderTargetDescriptor[i]);
			ReleaseViewportAttachmentTexture(cntx->freelist, &depthTextures[i], &depthView[i]);
		}
		width = height = 0;
	}

	//-----------------------------------------------------------------------

	void cRendererSimple::Draw(RIBootstrap::FrameContext* cntx,
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
		// scope. No BuildBlas: simple never traces.
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				if(listType == eRenderListType_Translucent)
				{
					// Particle emitters take the per-viewport scratch path in
					// the draw loop (BuildViewportVertices straight into
					// RI.translucentVtx/Idx segments) — panes must NOT write
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

				iVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<VertexBuffer_RI*>(pVB);
				vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
				vbri->AttachResourceToCntx(cntx);
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
			RITextureBarrier_s barriers[2] = {};
			barriers[0] = RI_PogoAttachmentBarrier(
				&state.renderTarget[RI.swapchainIndex], /*initial=*/true);

			barriers[1].texture = &state.depthTextures[RI.swapchainIndex];
			barriers[1].before = RI_RESOURCE_STATE_UNDEFINED;
			barriers[1].after = RI_RESOURCE_STATE_DEPTH_WRITE;
			barriers[1].aspect = RI_BARRIER_ASPECT_DEPTH;
			barriers[1].mipCount = 1;
			barriers[1].layerCount = 1;

			RI.primary.cmds[0].textureBarriers<2>(2, barriers);
		}

		////////////////////////////////////////////
		// Begin rendering into the state's render target at its 1:1 extent —
		// no overscan; cScene's pogo feed consumes it afterwards.
		{
			VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			colorAttachment.imageView = state.renderTargetDescriptor[RI.swapchainIndex].vk.image.imageView;
			colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			// Honor the viewport's clear color (e.g. the thumbnail builder's
			// gray backdrop) like the other renderers do.
			colorAttachment.clearValue.color = { { apSettings->mClearColor.r,
												   apSettings->mClearColor.g,
												   apSettings->mClearColor.b,
												   apSettings->mClearColor.a } };

			VkRenderingAttachmentInfo depthStencil = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			RI_VK_FillDepthAttachment(&depthStencil, &state.depthView[RI.swapchainIndex], /*attachAndClear=*/true);

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
		// Pipeline builder: 3 fixed-function streams (position always present;
		// color / uv fall back to the single-vertex defaults with a zeroed
		// binding stride, same trick as TranslucentMeshPipelineDesc). Variant
		// (blend/depth-write) + stream-present mask fold into the cache hash so
		// RIProgram keeps one pipeline per combination.
		auto bindSimplePipeline = [&](SimpleBlendVariant aVariant, uint32_t alVtxMask)
		{
			VkVertexInputBindingDescription vertexBindingDesc[3] = {
				{ 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }, // position (engine stream is float4, stride 16)
				{ 1, (alVtxMask & eVertexElementFlag_Color0)   ? 16u : 0u, VK_VERTEX_INPUT_RATE_VERTEX }, // color
				{ 2, (alVtxMask & eVertexElementFlag_Texture0) ? 12u : 0u, VK_VERTEX_INPUT_RATE_VERTEX }, // uv (float3 stream, .xy consumed)
			};
			VkVertexInputAttributeDescription vertexAttributeDesc[3] = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0 }, // position
				{ 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, // color
				{ 2, 2, VK_FORMAT_R32G32_SFLOAT,       0 }, // uv
			};
			VkPipelineVertexInputStateCreateInfo vertexInputState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vertexInputState.pVertexAttributeDescriptions = vertexAttributeDesc;
			vertexInputState.vertexAttributeDescriptionCount = ARRAY_COUNT(vertexAttributeDesc);
			vertexInputState.pVertexBindingDescriptions = vertexBindingDesc;
			vertexInputState.vertexBindingDescriptionCount = ARRAY_COUNT(vertexBindingDesc);

			VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			// CLOCKWISE front face to match the opaque/translucent raster
			// passes — all run under the same Y-flipped viewport.
			VkPipelineRasterizationStateCreateInfo rasterizationState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
			rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
			rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
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

			// Opaque writes depth; decals/translucents test against it only.
			VkPipelineDepthStencilStateCreateInfo depthStencilState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			depthStencilState.depthTestEnable = VK_TRUE;
			depthStencilState.depthWriteEnable = (aVariant == SIMPLE_OPAQUE) ? VK_TRUE : VK_FALSE;
			depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
			depthStencilState.minDepthBounds = 0.0f;
			depthStencilState.maxDepthBounds = 1.0f;

			VkPipelineColorBlendAttachmentState blendAttachmentState = {};
			blendAttachmentState.colorWriteMask =
				VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
			blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
			switch(aVariant)
			{
			case SIMPLE_OPAQUE:
				blendAttachmentState.blendEnable = VK_FALSE;
				blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
				blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				break;
			case SIMPLE_BLEND_ADD:
				blendAttachmentState.blendEnable = VK_TRUE;
				blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				break;
			case SIMPLE_BLEND_MUL:
				blendAttachmentState.blendEnable = VK_TRUE;
				blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
				blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
				blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				break;
			case SIMPLE_BLEND_MULX2:
				blendAttachmentState.blendEnable = VK_TRUE;
				blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
				blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
				blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
				blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				break;
			case SIMPLE_BLEND_ALPHA:
				blendAttachmentState.blendEnable = VK_TRUE;
				blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;
			case SIMPLE_BLEND_PREMUL_ALPHA:
				blendAttachmentState.blendEnable = VK_TRUE;
				blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				break;
			}
			VkPipelineColorBlendStateCreateInfo colorBlendState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			colorBlendState.attachmentCount = 1;
			colorBlendState.pAttachments = &blendAttachmentState;

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

			hash_t hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)aVariant);
			hash = hash_u32(hash, alVtxMask);
			m_simple.bindPipeline(&RI.device, &RI.primary.cmds[0], hash, "simple", &pipelineCreateInfo);
		};

		////////////////////////////////////////////
		// Draw the lists in legacy order: diffuse (depth write, no blend) →
		// decals → translucents (depth read-only, hardware blend). Per-object
		// state flows through a frame-scratch UBO (RI.UpdateFrameUBO) plus a
		// per-draw diffuse texture + sampler, DebugDraw-style.
		const ml::float4x4 viewProjMtx = apFrustum->GetProjectionMat() * apFrustum->GetViewMat();
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				// Draw sources — persistent VB by default; particles use the
				// per-viewport scratch path below.
				RIBuffer_s *posBuffer = nullptr;
				RIBuffer_s *colBuffer = nullptr;
				RIBuffer_s *uvBuffer = nullptr;
				RIBuffer_s *idxBuffer = nullptr;
				VkDeviceSize posOffset = 0, colOffset = 0, uvOffset = 0, idxOffset = 0;
				int indexCount = 0;

				if(listType == eRenderListType_Translucent &&
				   pObject->GetRenderType() == eRenderableType_ParticleEmitter)
				{
					////////////////////////////////////////////
					// Particles: build this pane's camera-facing quads straight
					// into per-frame segments of RI.translucentVtx/IdxBuffer —
					// each viewport gets its own correctly-billboarded geometry
					// (the persistent VB stays the hybrid renderer's; its
					// uploader copies coalesce in the fenced pre-pass, so panes
					// writing it would let the last pane win for EVERY pane).
					auto *pEmitter = static_cast<iParticleEmitter*>(pObject);
					auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime, /*withUv=*/true);
					if(!geom.valid) continue;

					posBuffer = &RI.translucentVtxBuffer;
					colBuffer = &RI.translucentVtxBuffer;
					uvBuffer  = &RI.translucentVtxBuffer;
					idxBuffer = &RI.translucentIdxBuffer;
					posOffset = (VkDeviceSize)geom.posByteOffset;
					colOffset = (VkDeviceSize)geom.colByteOffset;
					uvOffset  = (VkDeviceSize)geom.uvByteOffset;
					idxOffset = (VkDeviceSize)geom.idxByteOffset;
					indexCount = (int)geom.indexCount;
				}
				else
				{
					iVertexBuffer *pVB = pObject->GetVertexBuffer();
					if(pVB == NULL) continue;

					auto *vbri = static_cast<VertexBuffer_RI*>(pVB);
					const auto *posElement = vbri->GetElement(eVertexBufferElement_Position);
					const auto *colElement = vbri->GetElement(eVertexBufferElement_Color0);
					const auto *uvElement = vbri->GetElement(eVertexBufferElement_Texture0);
					const auto &indexBufferPtr = vbri->GetIndexRIBuffer();
					indexCount = vbri->GetIndexNum();
					if(posElement == NULL || !posElement->buffer || !indexBufferPtr || indexCount <= 0) {
						continue;
					}
					posBuffer = posElement->buffer.get();
					idxBuffer = indexBufferPtr.get();
					colBuffer = (colElement && colElement->buffer) ? colElement->buffer.get() : nullptr;
					uvBuffer = (uvElement && uvElement->buffer) ? uvElement->buffer.get() : nullptr;
				}

				cMaterial *pMat = pObject->GetMaterial();

				const SimpleBlendVariant variant = (listType == eRenderListType_Diffuse)
					? SIMPLE_OPAQUE
					: remapBlend(pMat ? pMat->GetBlendMode() : eMaterialBlendMode_Alpha);
				uint32_t vtxMask = 0;
				if(colBuffer) vtxMask |= eVertexElementFlag_Color0;
				if(uvBuffer) vtxMask |= eVertexElementFlag_Texture0;
				bindSimplePipeline(variant, vtxMask);

				cMatrixf *pModelMtx = pObject->GetModelMatrix(apFrustum);
				const ml::float4x4 mvp = viewProjMtx *
					cMath::ToFloatTranspose4x4(pModelMtx ? *pModelMtx : cMatrixf::Identity);
				const ml::float4x4 uvMtx =
					cMath::ToFloatTranspose4x4(pMat ? pMat->GetUvMatrix() : cMatrixf::Identity);

				SimplePass uniformBlock = {};
				std::memcpy(uniformBlock.mvp, mvp.a, sizeof(uniformBlock.mvp));
				std::memcpy(uniformBlock.uvMtx, uvMtx.a, sizeof(uniformBlock.uvMtx));
				uniformBlock.color[0] = 1.0f;
				uniformBlock.color[1] = 1.0f;
				uniformBlock.color[2] = 1.0f;
				uniformBlock.color[3] = 1.0f;
				// Cutout solids (alpha-tested grates/foliage) reject in the
				// fragment stage; blended lists keep their hardware blend.
				uniformBlock.alphaReject =
					(listType == eRenderListType_Diffuse && pMat &&
					 pMat->GetAlphaMode() == eMaterialAlphaMode_Trans) ? 0.5f : 0.0f;

				// Per-draw diffuse: material texture when present, else the
				// 1x1 white default. Pin real textures so a mid-frame destroy
				// can't free the VkImage before this submit retires.
				Image *pDiffuseImage = pMat ? pMat->GetImage(eMaterialTexture_Diffuse) : nullptr;
				std::shared_ptr<HPLTexture> texture = pDiffuseImage ? pDiffuseImage->GetTexture() : nullptr;
				RIDescriptor_s textureDescriptor = RI.whiteTexture2DBinding;
				if(texture) {
					cntx->resourceLink.push_back(texture);
					textureDescriptor = texture->binding;
				}

				auto samplerDesc = RI.resolve_filter_descriptor(
					pMat ? pMat->GetTextureWrap() : eTextureWrap_Repeat,
					pMat ? pMat->GetTextureWrap() : eTextureWrap_Repeat,
					pMat ? pMat->GetTextureWrap() : eTextureWrap_Repeat,
					pMat ? pMat->GetTextureFilter() : eTextureFilter_Bilinear);
				assert(samplerDesc);

				RIProgram::DescriptorBinding bindings[3] = {};
				RI.UpdateFrameUBO(&bindings[0].descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
				bindings[0].handle = DescriptorBindingID::Create("pass");
				bindings[1].descriptor = *samplerDesc;
				bindings[1].handle = DescriptorBindingID::Create("diffuseSampler");
				bindings[2].descriptor = textureDescriptor;
				bindings[2].handle = DescriptorBindingID::Create("diffuseMap");
				m_simple.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, bindings, 3);

				RIBuffer_s *vertBufs[3] = {
					posBuffer,
					colBuffer ? colBuffer : &RI.fallbackColorVertex,
					uvBuffer ? uvBuffer : &RI.fallbackUv0Vertex,
				};
				const VkDeviceSize vertOffsets[3] = { posOffset, colOffset, uvOffset };
				RI.primary.cmds[0].bindVertexBuffers<3>(0, 3, vertBufs, vertOffsets);
				RI.primary.cmds[0].bindIndexBuffer(idxBuffer, idxOffset, VK_INDEX_TYPE_UINT32);
				RI.primary.cmds[0].drawIndexed((uint32_t)indexCount, 1, 0, 0, 0);
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
			debugDraw->flush(cntx, &RI.primary.cmds[0], apFrustum, renderWidth,
							 renderHeight, RIBootstrap::PogoColorFormatVk);
		}

		vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

		////////////////////////////////////////////
		// Hand off: render target COLOR -> SHADER_READ — the finished frame
		// cScene feeds into the viewport pogo (post processing) and delivers
		// to the Target.
		{
			RI.primary.cmds[0].textureBarrier(RI_PogoShaderBarrier(
				&state.renderTarget[RI.swapchainIndex], /*initial=*/false));
		}
	}


	//-----------------------------------------------------------------------

}
