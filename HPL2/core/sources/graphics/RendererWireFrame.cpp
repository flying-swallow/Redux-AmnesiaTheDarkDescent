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

		//------------------------------------------------------------------
		// Explicit per-backend binding tables for m_wireframe (no bindless
		// set — every [vk::binding] is program-managed). Authored from
		// WireFrame/wireframe.{vert,frag}.slang and the generated
		// build-mtl/amnesia/compiled_shaders/wireframe.{vert,frag}.metal
		// entry-point signatures. Both stages take only the "pass" UBO; no
		// push constants, so pushConstantSize stays 0.
		// Both stages take only the "pass" UBO (Metal buffer 0).
		static const RIProgram::RIProgramBinding kWireFrame[] = {
			{ "pass", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX | RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0} },
		};
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cRendererWireFrame::cRendererWireFrame(cGraphics *apGraphics,cResources* apResources)
		: iRenderer("WireFrame",apGraphics, apResources)
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
		RIProgram::RIProgramDescriptor desc = {};
		desc.stages = stages;
		desc.bindings = kWireFrame;
		m_wireframe.initialize(&RI.device, desc);
	}

	//-----------------------------------------------------------------------

	// cViewport::SimpleViewportState (shared with this renderer) is
	// implemented in RendererSimple.cpp.

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
		// Upload vertex/index streams. Must run BEFORE beginRendering — the
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

				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<cVertexBuffer*>(pVB);
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
			RITextureBarrier barriers[2] = {};
			barriers[0] = RI_PogoAttachmentBarrier(
				&state.renderTarget[RI.swapchainIndex], /*initial=*/true);

			barriers[1].texture = &state.depthTextures[RI.swapchainIndex];
			barriers[1].before = RI_RESOURCE_STATE_UNDEFINED;
			barriers[1].after = RI_RESOURCE_STATE_DEPTH_WRITE;
			barriers[1].aspect = RI_BARRIER_ASPECT_DEPTH;
			barriers[1].mipCount = 1;
			barriers[1].layerCount = 1;

			RI.primary.cmds[0].vk_d3d12_textureBarriers<2>(2, barriers);
		}

		////////////////////////////////////////////
		// Begin rendering into the state's render target at its 1:1 extent —
		// no overscan; cScene's pogo feed consumes it afterwards.
		{
			// The descriptor references the state's owned color view; copy it out
			// to feed the backend-neutral attachment desc.
			RITextureView colorView =
				state.renderTargetColorView[RI.swapchainIndex];

			RIRenderingAttachment color = {};
			color.view = &colorView;
			color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			color.clearValue.color[3] = 1.0f;

			RIRenderingAttachment depth = {};
			depth.view = &state.depthView[RI.swapchainIndex];
			depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			depth.clearValue.depth = 1.0f;

			RIBeginRenderingDesc renderingInfo = {};
			renderingInfo.renderArea.width = (int16_t)renderWidth;
			renderingInfo.renderArea.height = (int16_t)renderHeight;
			renderingInfo.colorCount = 1;
			renderingInfo.colors = &color;
			renderingInfo.depthStencil = &depth;
			// Both render-scope openers run side by side: each is a no-op on the
			// other backend (mtl_encoderDraw is empty off-Metal; vk_d3d12_beginRendering
			// dispatches on the active API).
			RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
			RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);
		}

		// Y-flipped viewport — same convention as the forward passes, so the
		// unmodified projection matrix lands the right way up.
		RIViewport viewport_vk = {};
		viewport_vk.y = (float)renderHeight;
		viewport_vk.width = (float)renderWidth;
		viewport_vk.height = -(float)renderHeight;
		viewport_vk.depthMax = 1.0f;
		RIRect scissor = {};
		scissor.width = (int16_t)renderWidth;
		scissor.height = (int16_t)renderHeight;
		RI.primary.cmds[0].setViewport(&RI.renderer, viewport_vk);
		RI.primary.cmds[0].setScissor(&RI.renderer, scissor);

		////////////////////////////////////////////
		// Pipeline: position-only fetch, triangle list rasterised as lines.
		// Backend-neutral descriptor (same idiom as the post-effects) so the same
		// call site builds a Vulkan pipeline or a Metal one. Formats are constexpr
		// so a single cached variant suffices.
		{
			RIGraphicsPipelineDesc pipe{};
			pipe.topology = RI_TOPOLOGY_TRIANGLE_LIST;
			pipe.fillMode = RI_FILL_LINE; // rasterise the triangle list as wireframe
			pipe.cullMode = RI_CULL_MODE_NONE;
			pipe.depthTestEnable = true;
			pipe.depthWriteEnable = true;
			pipe.depthCompareOp = RI_COMPARE_LESS_EQUAL;
			pipe.depthStencilFormat = RIBootstrap::DepthFormat;
			pipe.colorCount = 1;
			pipe.colors[0].format = RIBootstrap::PogoColorFormat; // blend disabled (default)

			// position (engine stream is float4, stride 16)
			pipe.vertexStreamCount = 1;
			pipe.vertexStreams[0] = { /*binding*/ 0, /*stride*/ 16, /*perInstance*/ false };
			pipe.vertexAttributeCount = 1;
			pipe.vertexAttributes[0] = { /*location*/ 0, /*binding*/ 0, RI_FORMAT_RGB32_SFLOAT, /*offset*/ 0 };

			const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, 0u);
			m_wireframe.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash, "wireframe", pipe);
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

				////////////////////////////////////////////
				// Particles: per-viewport scratch path. Build this pane's
				// camera-facing quads straight into per-frame segments of
				// RI.translucentVtx/IdxBuffer and bind them at byte offsets —
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
					RI.UpdateFrameUBO(&binding.descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
					binding.handle = DescriptorBindingID::Create("pass");
					m_wireframe.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex, &binding, 1);

					RIBuffer *vertBufs[1] = { &RI.translucentVtxBuffer };
					const RIDeviceSize vertOffsets[1] = { (RIDeviceSize)geom.posByteOffset };
					RI.primary.cmds[0].bindVertexBuffers<1>(0, 1, vertBufs, vertOffsets);
					RI.primary.cmds[0].bindIndexBuffer(&RI.renderer, &RI.translucentIdxBuffer,
													   (RIDeviceSize)geom.idxByteOffset,
													   RI_INDEX_TYPE_32);
					RI.primary.cmds[0].drawIndexed(&RI.renderer, geom.indexCount, 1, 0, 0, 0);
					continue;
				}

				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<cVertexBuffer*>(pVB);
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

				RIBuffer *vertBufs[1] = { posElement->buffer.get() };
				const RIDeviceSize vertOffsets[1] = { 0 };
				RI.primary.cmds[0].bindVertexBuffers<1>(0, 1, vertBufs, vertOffsets);
				RI.primary.cmds[0].bindIndexBuffer(&RI.renderer, indexBuffer.get(), 0, RI_INDEX_TYPE_32);
				RI.primary.cmds[0].drawIndexed(&RI.renderer, (uint32_t)indexCount, 1, 0, 0, 0);
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
							 renderHeight, RIBootstrap::PogoColorFormat);
		}

		RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);
		RI.primary.cmds[0].mtl_encoderEnd();

		////////////////////////////////////////////
		// Hand off: render target COLOR -> SHADER_READ — the finished frame
		// cScene feeds into the viewport pogo (post processing) and delivers
		// to the Target.
		{
			RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
				&state.renderTarget[RI.swapchainIndex], /*initial=*/false));
		}
	}


	//-----------------------------------------------------------------------

}
