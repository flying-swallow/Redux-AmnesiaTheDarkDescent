/**
 * Copyright 2026 Michael Pollind
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics/DebugDraw.h"

#include "graphics/Texture.h"
#include "graphics/Image.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "resources/Resources.h"

#include "scene/Camera.h"

#include "system/Hasher.h"

#include <algorithm>
#include <cstring>

namespace hpl {

	namespace {

		// Mirrors DebugPass in the DebugDraw slang shaders (binding 0, set 0).
		struct DebugPass {
			float viewProjMat[16];
			float viewProj2DMat[16];
		};

		RICompareFunc_e toCompareOp(DebugDraw::DebugDepthTest aTest)
		{
			switch(aTest) {
			case DebugDraw::DebugDepthTest::None:         return RI_COMPARE_NEVER;
			case DebugDraw::DebugDepthTest::Less:         return RI_COMPARE_LESS;
			case DebugDraw::DebugDepthTest::LessEqual:    return RI_COMPARE_LESS_EQUAL;
			case DebugDraw::DebugDepthTest::Equal:        return RI_COMPARE_EQUAL;
			case DebugDraw::DebugDepthTest::GreaterEqual: return RI_COMPARE_GREATER_EQUAL;
			case DebugDraw::DebugDepthTest::Greater:      return RI_COMPARE_GREATER;
			case DebugDraw::DebugDepthTest::NotEqual:     return RI_COMPARE_NOT_EQUAL;
			case DebugDraw::DebugDepthTest::Always:       return RI_COMPARE_ALWAYS;
			default: break;
			}
			assert(false && "unhandled DebugDepthTest");
			return RI_COMPARE_ALWAYS;
		}

		struct DebugPipelineCfg {
			uint8_t topology = RI_TOPOLOGY_LINE_LIST;      // RITopology_e
			bool depthTestEnable = true;
			uint8_t depthOp = RI_COMPARE_LESS_EQUAL;       // RICompareFunc_e
			bool alphaBlend = false;   // false => additive ONE/ONE (TDD overlay look)
			bool uvLayout = false;     // declare the uv attribute (debug_uv.vert)
			uint32_t colorFormat = RIBootstrap::PogoColorFormat; // RI_Format_e
		};

		// One cached pipeline per (topology, depth, blend, format) combination —
		// non-bindless template cloned from RendererWireFrame.cpp (neutral desc).
		void bindDebugPipeline(RIProgram& aProgram, struct RICmd* cmd,
							   const DebugPipelineCfg& aCfg, const char* asDebugName)
		{
			RIGraphicsPipelineDesc pipe{};
			pipe.topology = aCfg.topology;
			pipe.cullMode = RI_CULL_MODE_NONE; // sidesteps the CW-front-face trap
			pipe.fillMode = RI_FILL_SOLID;
			// Overlay geometry never writes depth — it tests against the scene.
			pipe.depthTestEnable = aCfg.depthTestEnable;
			pipe.depthWriteEnable = false;
			pipe.depthCompareOp = aCfg.depthOp;
			pipe.depthStencilFormat = RIBootstrap::DepthFormat;

			// RGB-only write mask: keep the scene's alpha channel intact.
			pipe.colorCount = 1;
			pipe.colors[0].format = aCfg.colorFormat;
			pipe.colors[0].blendEnabled = true;
			pipe.colors[0].colorBlendOp = RI_BLEND_OP_ADD;
			pipe.colors[0].alphaBlendOp = RI_BLEND_OP_ADD;
			pipe.colors[0].writeMask = RI_COLOR_WRITE_RGB;
			if(aCfg.alphaBlend) {
				pipe.colors[0].srcColor = RI_BLEND_SRC_ALPHA;
				pipe.colors[0].dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
				pipe.colors[0].srcAlpha = RI_BLEND_SRC_ALPHA;
				pipe.colors[0].dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
			} else {
				pipe.colors[0].srcColor = RI_BLEND_ONE;
				pipe.colors[0].dstColor = RI_BLEND_ONE;
				pipe.colors[0].srcAlpha = RI_BLEND_ONE;
				pipe.colors[0].dstAlpha = RI_BLEND_ONE;
			}

			// Unified DebugVertex stream (stride 36); color-only stages skip uv.
			pipe.vertexStreamCount = 1;
			pipe.vertexStreams[0] = { /*binding*/ 0, /*stride*/ 36, /*perInstance*/ false };
			pipe.vertexAttributes[0] = { 0, 0, RI_FORMAT_RGB32_SFLOAT, 0 }; // position
			if(aCfg.uvLayout) {
				pipe.vertexAttributes[1] = { 1, 0, RI_FORMAT_RG32_SFLOAT, 12 };    // uv
				pipe.vertexAttributes[2] = { 2, 0, RI_FORMAT_RGBA32_SFLOAT, 20 };  // color
				pipe.vertexAttributeCount = 3;
			} else {
				pipe.vertexAttributes[1] = { 1, 0, RI_FORMAT_RGBA32_SFLOAT, 20 };  // color
				pipe.vertexAttributeCount = 2;
			}

			hash_t hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)aCfg.topology);
			hash = hash_u32(hash, (uint32_t)aCfg.depthTestEnable);
			hash = hash_u32(hash, (uint32_t)aCfg.depthOp);
			hash = hash_u32(hash, (uint32_t)aCfg.alphaBlend);
			hash = hash_u32(hash, (uint32_t)aCfg.colorFormat);
			aProgram.bindPipeline(&RI.device, cmd, hash, asDebugName, pipe);
		}

		//------------------------------------------------------------------
		// Explicit per-backend binding tables for the three DebugDraw programs
		// (m_colorProgram, m_color2DProgram, m_uvProgram). None take a bindless
		// set, so every [vk::binding] is program-managed. Authored from
		// DebugDraw/debug*.slang and the generated
		// build-mtl/amnesia/compiled_shaders/debug*.metal entry-point
		// signatures. State flows through the "pass" frame-scratch UBO — no
		// push constants, so pushConstantSize stays 0.

		// debug.frag / debug_2d: flat vertex color — the declared `pass` UBO is
		// unused in the fragment stage (slangc + the SPIR-V optimizer strip it),
		// so only the vertex `pass` UBO (Metal buffer 0) survives.
		const RIProgram::RIProgramBinding kDebugColor[] = {
			{ "pass", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX, {0, 0}, {}, {0} },
		};

		// debug_uv: vertex `pass` UBO (Metal buffer 0) + fragment diffuse
		// sampler/texture. The `pass` UBO is stripped from the fragment stage.
		const RIProgram::RIProgramBinding kDebugUv[] = {
			{ "pass",           RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX,   {0, 0}, {}, {0} },
			{ "diffuseSampler", RI_DESCRIPTOR_TYPE_SAMPLER,        1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0} },
			{ "diffuseMap",     RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {0} },
		};

	} // namespace

	//-----------------------------------------------------------------------

	void DebugDraw::Init(cResources* apResources)
	{
		auto loadProgram = [&](RIProgram& aProgram, const char* asVert, const char* asFrag,
							   std::span<const RIProgram::RIProgramBinding> bindings) {
			auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), asVert);
			auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), asFrag);
			std::array<RIProgram::ModuleStage, 2> stages = {
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
			};
			RIProgram::RIProgramDescriptor desc = {};
			desc.stages = stages;
			desc.bindings = bindings;
			aProgram.initialize(&RI.device, desc);
		};
		loadProgram(m_colorProgram, "debug.vert.spv", "debug.frag.spv", kDebugColor);
		loadProgram(m_color2DProgram, "debug_2d.vert.spv", "debug.frag.spv", kDebugColor);
		loadProgram(m_uvProgram, "debug_uv.vert.spv", "debug_uv.frag.spv", kDebugUv);
	}

	//-----------------------------------------------------------------------

	void DebugDraw::Reset()
	{
		m_lineSegments.clear();
		m_colorTriangles.clear();
		m_colorQuads.clear();
		m_uvQuads.clear();
		m_line2DSegments.clear();
	}

	bool DebugDraw::HasRequests() const
	{
		return !m_lineSegments.empty() || !m_colorTriangles.empty() || !m_colorQuads.empty() ||
			   !m_uvQuads.empty() || !m_line2DSegments.empty();
	}

	//-----------------------------------------------------------------------
	// Enqueue API — geometry builders ported from the TDD fork's DebugDraw,
	// with the option transform applied in HPL space at enqueue time.

	void DebugDraw::DebugDrawLine(const cVector3f& avStart, const cVector3f& avEnd, const cColor& aColor,
								  const DebugDrawOptions& aOptions)
	{
		LineSegmentRequest request;
		request.m_depthTest = aOptions.m_depthTest;
		request.m_start = cMath::MatrixMul(aOptions.m_transform, avStart);
		request.m_end = cMath::MatrixMul(aOptions.m_transform, avEnd);
		request.m_color = aColor;
		m_lineSegments.push_back(request);
	}

	void DebugDraw::DebugDrawBoxMinMax(const cVector3f& avMin, const cVector3f& avMax, const cColor& aColor,
									   const DebugDrawOptions& aOptions)
	{
		// Pos Z Quad
		DebugDrawLine(cVector3f(avMax.x, avMax.y, avMax.z), cVector3f(avMin.x, avMax.y, avMax.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMax.x, avMax.y, avMax.z), cVector3f(avMax.x, avMin.y, avMax.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMin.x, avMax.y, avMax.z), cVector3f(avMin.x, avMin.y, avMax.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMin.x, avMin.y, avMax.z), cVector3f(avMax.x, avMin.y, avMax.z), aColor, aOptions);

		// Neg Z Quad
		DebugDrawLine(cVector3f(avMax.x, avMax.y, avMin.z), cVector3f(avMin.x, avMax.y, avMin.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMax.x, avMax.y, avMin.z), cVector3f(avMax.x, avMin.y, avMin.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMin.x, avMax.y, avMin.z), cVector3f(avMin.x, avMin.y, avMin.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMin.x, avMin.y, avMin.z), cVector3f(avMax.x, avMin.y, avMin.z), aColor, aOptions);

		// Lines between
		DebugDrawLine(cVector3f(avMax.x, avMax.y, avMax.z), cVector3f(avMax.x, avMax.y, avMin.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMin.x, avMax.y, avMax.z), cVector3f(avMin.x, avMax.y, avMin.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMin.x, avMin.y, avMax.z), cVector3f(avMin.x, avMin.y, avMin.z), aColor, aOptions);
		DebugDrawLine(cVector3f(avMax.x, avMin.y, avMax.z), cVector3f(avMax.x, avMin.y, avMin.z), aColor, aOptions);
	}

	void DebugDraw::DebugDrawSphere(const cVector3f& avPos, float afRadius, const cColor& aColor,
									const DebugDrawOptions& aOptions)
	{
		DebugDrawSphere(avPos, afRadius, aColor, aColor, aColor, aOptions);
	}

	void DebugDraw::DebugDrawSphere(const cVector3f& avPos, float afRadius, const cColor& aC1, const cColor& aC2,
									const cColor& aC3, const DebugDrawOptions& aOptions)
	{
		constexpr int lSegments = 32;
		constexpr float fAngleStep = k2Pif / (float)lSegments;

		// X Circle
		for(float a = 0; a < k2Pif; a += fAngleStep) {
			DebugDrawLine(
				cVector3f(avPos.x, avPos.y + sin(a) * afRadius, avPos.z + cos(a) * afRadius),
				cVector3f(avPos.x, avPos.y + sin(a + fAngleStep) * afRadius, avPos.z + cos(a + fAngleStep) * afRadius),
				aC1, aOptions);
		}
		// Y Circle
		for(float a = 0; a < k2Pif; a += fAngleStep) {
			DebugDrawLine(
				cVector3f(avPos.x + cos(a) * afRadius, avPos.y, avPos.z + sin(a) * afRadius),
				cVector3f(avPos.x + cos(a + fAngleStep) * afRadius, avPos.y, avPos.z + sin(a + fAngleStep) * afRadius),
				aC2, aOptions);
		}
		// Z Circle
		for(float a = 0; a < k2Pif; a += fAngleStep) {
			DebugDrawLine(
				cVector3f(avPos.x + cos(a) * afRadius, avPos.y + sin(a) * afRadius, avPos.z),
				cVector3f(avPos.x + cos(a + fAngleStep) * afRadius, avPos.y + sin(a + fAngleStep) * afRadius, avPos.z),
				aC3, aOptions);
		}
	}

	void DebugDraw::DrawTri(const cVector3f& avV1, const cVector3f& avV2, const cVector3f& avV3, const cColor& aColor,
							const DebugDrawOptions& aOptions)
	{
		ColorTriRequest request;
		request.m_depthTest = aOptions.m_depthTest;
		request.m_v1 = cMath::MatrixMul(aOptions.m_transform, avV1);
		request.m_v2 = cMath::MatrixMul(aOptions.m_transform, avV2);
		request.m_v3 = cMath::MatrixMul(aOptions.m_transform, avV3);
		request.m_color = aColor;
		m_colorTriangles.push_back(request);
	}

	void DebugDraw::DrawQuad(const cVector3f& avV1, const cVector3f& avV2, const cVector3f& avV3, const cVector3f& avV4,
							 const cColor& aColor, const DebugDrawOptions& aOptions)
	{
		ColorQuadRequest request;
		request.m_depthTest = aOptions.m_depthTest;
		request.m_v1 = cMath::MatrixMul(aOptions.m_transform, avV1);
		request.m_v2 = cMath::MatrixMul(aOptions.m_transform, avV2);
		request.m_v3 = cMath::MatrixMul(aOptions.m_transform, avV3);
		request.m_v4 = cMath::MatrixMul(aOptions.m_transform, avV4);
		request.m_color = aColor;
		m_colorQuads.push_back(request);
	}

	void DebugDraw::DrawQuad(const cVector3f& avV1, const cVector3f& avV2, const cVector3f& avV3, const cVector3f& avV4,
							 const cVector2f& avUv0, const cVector2f& avUv1, Image* apImage, const cColor& aTint,
							 const DebugDrawOptions& aOptions)
	{
		std::shared_ptr<cTexture> texture = apImage ? apImage->GetTexture() : nullptr;
		if(!texture) {
			DrawQuad(avV1, avV2, avV3, avV4, aTint, aOptions);
			return;
		}
		UVQuadRequest request;
		request.m_depthTest = aOptions.m_depthTest;
		UVQuadRequest::Quad quad;
		quad.m_v1 = cMath::MatrixMul(aOptions.m_transform, avV1);
		quad.m_v2 = cMath::MatrixMul(aOptions.m_transform, avV2);
		quad.m_v3 = cMath::MatrixMul(aOptions.m_transform, avV3);
		quad.m_v4 = cMath::MatrixMul(aOptions.m_transform, avV4);
		request.m_type = quad;
		request.m_uv0 = avUv0;
		request.m_uv1 = avUv1;
		request.m_texture = texture;
		request.m_color = aTint;
		m_uvQuads.push_back(request);
	}

	void DebugDraw::DrawPyramid(const cVector3f& avBaseCenter, const cVector3f& avTop, float afHalfWidth,
								const cColor& aColor, const DebugDrawOptions& aOptions)
	{
		const cVector3f vNormal = avTop - avBaseCenter;
		const cVector3f vPoint = avBaseCenter + cVector3f(1, 1, 1);
		const cVector3f vRight = cMath::Vector3Normalize(cMath::Vector3Cross(vNormal, vPoint));
		const cVector3f vForward = cMath::Vector3Normalize(cMath::Vector3Cross(vNormal, vRight));

		const cVector3f topRight = avBaseCenter + (vRight + vForward) * afHalfWidth;
		const cVector3f topLeft = avBaseCenter + (vRight - vForward) * afHalfWidth;
		const cVector3f bottomLeft = avBaseCenter + (vRight * (-1) - vForward) * afHalfWidth;
		const cVector3f bottomRight = avBaseCenter + (vRight * (-1) + vForward) * afHalfWidth;

		DrawTri(avTop, topRight, topLeft, aColor, aOptions);
		DrawTri(avTop, topLeft, bottomLeft, aColor, aOptions);
		DrawTri(avTop, bottomLeft, bottomRight, aColor, aOptions);
		DrawTri(avTop, bottomRight, topRight, aColor, aOptions);
	}

	void DebugDraw::DrawBillboard(const cVector3f& avPos, const cVector2f& avSize, const cVector2f& avUv0,
								  const cVector2f& avUv1, Image* apImage, const cColor& aTint,
								  const DebugDrawOptions& aOptions)
	{
		std::shared_ptr<cTexture> texture = apImage ? apImage->GetTexture() : nullptr;
		if(!texture) {
			return;
		}
		UVQuadRequest request;
		request.m_depthTest = aOptions.m_depthTest;
		UVQuadRequest::Billboard billboard;
		billboard.m_pos = avPos;
		billboard.m_size = avSize;
		billboard.m_transform = aOptions.m_transform;
		request.m_type = billboard;
		request.m_uv0 = avUv0;
		request.m_uv1 = avUv1;
		request.m_texture = texture;
		request.m_color = aTint;
		m_uvQuads.push_back(request);
	}

	void DebugDraw::DebugWireFrameFromVertexBuffer(cVertexBuffer* apVertexBuffer, const cColor& aColor,
												   const DebugDrawOptions& aOptions)
	{
		const int lIndexNum = apVertexBuffer->GetIndexNum();
		unsigned int* pIndexArray = apVertexBuffer->GetIndices();

		float* pVertexArray = apVertexBuffer->GetFloatArray(eVertexBufferElement_Position);
		const int lVertexStride = apVertexBuffer->GetElementNum(eVertexBufferElement_Position);

		cVector3f vTriPos[3];
		for(int tri = 0; tri < lIndexNum; tri += 3)
		{
			for(int idx = 0; idx < 3; idx++)
			{
				const int lVtx = pIndexArray[tri + 2 - idx] * lVertexStride;
				vTriPos[idx].x = pVertexArray[lVtx + 0];
				vTriPos[idx].y = pVertexArray[lVtx + 1];
				vTriPos[idx].z = pVertexArray[lVtx + 2];
			}
			for(int i = 0; i < 3; ++i)
			{
				const int lNext = i == 2 ? 0 : i + 1;
				DebugDrawLine(vTriPos[i], vTriPos[lNext], aColor, aOptions);
			}
		}
	}

	void DebugDraw::DebugSolidFromVertexBuffer(cVertexBuffer* apVertexBuffer, const cColor& aColor,
											   const DebugDrawOptions& aOptions)
	{
		const int lIndexNum = apVertexBuffer->GetIndexNum();
		unsigned int* pIndexArray = apVertexBuffer->GetIndices();

		float* pVertexArray = apVertexBuffer->GetFloatArray(eVertexBufferElement_Position);
		const int lVertexStride = apVertexBuffer->GetElementNum(eVertexBufferElement_Position);

		cVector3f vTriPos[3];
		for(int tri = 0; tri < lIndexNum; tri += 3)
		{
			for(int idx = 0; idx < 3; idx++)
			{
				const int lVtx = pIndexArray[tri + 2 - idx] * lVertexStride;
				vTriPos[idx].x = pVertexArray[lVtx + 0];
				vTriPos[idx].y = pVertexArray[lVtx + 1];
				vTriPos[idx].z = pVertexArray[lVtx + 2];
			}
			DrawTri(vTriPos[0], vTriPos[1], vTriPos[2], aColor, aOptions);
		}
	}

	void DebugDraw::DebugDraw2DLine(const cVector2f& avStart, const cVector2f& avEnd, const cColor& aColor)
	{
		Line2DSegmentRequest request;
		request.m_start = avStart;
		request.m_end = avEnd;
		request.m_color = aColor;
		m_line2DSegments.push_back(request);
	}

	void DebugDraw::DebugDraw2DLineQuad(cRect2f aRect, const cColor& aColor)
	{
		DebugDraw2DLine(cVector2f(aRect.x, aRect.y), cVector2f(aRect.x + aRect.w, aRect.y), aColor);
		DebugDraw2DLine(cVector2f(aRect.x + aRect.w, aRect.y), cVector2f(aRect.x + aRect.w, aRect.y + aRect.h), aColor);
		DebugDraw2DLine(cVector2f(aRect.x + aRect.w, aRect.y + aRect.h), cVector2f(aRect.x, aRect.y + aRect.h), aColor);
		DebugDraw2DLine(cVector2f(aRect.x, aRect.y + aRect.h), cVector2f(aRect.x, aRect.y), aColor);
	}

	float DebugDraw::BillboardScale(cCamera* apCamera, const cVector3f& avPos)
	{
		const cVector3f vViewSpacePosition = cMath::MatrixMul(apCamera->GetViewMatrix(), avPos);
		switch(apCamera->GetProjectionType())
		{
		case eProjectionType_Orthographic:
			return apCamera->GetOrthoViewSize().x * 0.25f;
		case eProjectionType_Perspective:
			return cMath::Abs(vViewSpacePosition.z);
		default:
			break;
		}
		assert(false && "invalid projection type");
		return 0.0f;
	}

	//-----------------------------------------------------------------------

	bool DebugDraw::RequestStream(RIBootstrap::FrameContext* cntx, size_t alNumVertices, size_t alNumIndices,
								  struct RISegmentReq* apVtxReq, struct RISegmentReq* apIdxReq)
	{
		if(!IsRIBufferValid(&RI.renderer, &m_vertexBuffer) ||
		   !m_vertexAlloc.request(RI.frameIndex, alNumVertices, apVtxReq))
		{
			struct RISegmentAllocDesc segmentAllocDesc = { 0 };
			segmentAllocDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
			segmentAllocDesc.elementStride = sizeof(DebugVertex);
			segmentAllocDesc.maxElements = std::max<size_t>(m_vertexAlloc.maxElements, 1024);
			do {
				segmentAllocDesc.maxElements = (segmentAllocDesc.maxElements + (segmentAllocDesc.maxElements >> 1));
			} while(segmentAllocDesc.maxElements < alNumVertices);
			m_vertexAlloc = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&segmentAllocDesc);
			bool res = m_vertexAlloc.request(RI.frameIndex, alNumVertices, apVtxReq);
			assert(res);

			if(IsRIBufferValid(&RI.renderer, &m_vertexBuffer)) {
				cntx->freelist.push_back(m_vertexBuffer);
			}
			RIBufferDesc vbDesc = {};
			vbDesc.size = (uint64_t)segmentAllocDesc.maxElements * segmentAllocDesc.elementStride;
			vbDesc.usage = RI_BUFFER_USAGE_VERTEX_BUFFER;
			vbDesc.location = RI_MEMORY_HOST_UPLOAD;
			m_vertexBuffer = RIBuffer::create(&RI.device, vbDesc);
		}

		if(!IsRIBufferValid(&RI.renderer, &m_indexBuffer) ||
		   !m_indexAlloc.request(RI.frameIndex, alNumIndices, apIdxReq))
		{
			struct RISegmentAllocDesc segmentAllocDesc = { 0 };
			segmentAllocDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
			segmentAllocDesc.elementStride = sizeof(uint32_t);
			segmentAllocDesc.maxElements = std::max<size_t>(m_indexAlloc.maxElements, 1024);
			do {
				segmentAllocDesc.maxElements = (segmentAllocDesc.maxElements + (segmentAllocDesc.maxElements >> 1));
			} while(segmentAllocDesc.maxElements < alNumIndices);
			m_indexAlloc = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&segmentAllocDesc);
			bool res = m_indexAlloc.request(RI.frameIndex, alNumIndices, apIdxReq);
			assert(res);

			if(IsRIBufferValid(&RI.renderer, &m_indexBuffer)) {
				cntx->freelist.push_back(m_indexBuffer);
			}
			RIBufferDesc ibDesc = {};
			ibDesc.size = (uint64_t)segmentAllocDesc.maxElements * segmentAllocDesc.elementStride;
			ibDesc.usage = RI_BUFFER_USAGE_INDEX_BUFFER;
			ibDesc.location = RI_MEMORY_HOST_UPLOAD;
			m_indexBuffer = RIBuffer::create(&RI.device, ibDesc);
		}
		return true;
	}

	//-----------------------------------------------------------------------

	void DebugDraw::flush(RIBootstrap::FrameContext* cntx, struct RICmd* cmd, const cFrustum* apFrustum,
						  uint32_t alTargetWidth, uint32_t alTargetHeight, RI_Format_e aColorFormat)
	{
		if(!HasRequests()) {
			return;
		}

		////////////////////////////////////////////
		// One scratch UBO for every batch this flush; cMatrixf -> ml conversion
		// happens only here. The 2D ortho mirrors GuiSet's screen projection
		// (y-flip + GL->Vulkan clip-z remap, paired with the negative-height
		// viewport below) so 2D overlays use GUI pixel coordinates.
		DebugPass uniformBlock = {};
		{
			const ml::float4x4 viewProjMtx = apFrustum->GetProjectionMat() * apFrustum->GetViewMat();
			std::memcpy(uniformBlock.viewProjMat, viewProjMtx.a, sizeof(uniformBlock.viewProjMat));

			ml::float4x4 orthoMtx = ml::float4x4::Identity();
			orthoMtx.SetupByOrthoProjection(0.0f, (float)alTargetWidth, 0.0f, (float)alTargetHeight, -1000.0f, 1000.0f);
			ml::float4x4 clipRemap = ml::float4x4::Identity();
			clipRemap.a11 = -1.0f;
			clipRemap.a22 = 0.5f;
			clipRemap.a23 = 0.5f;
			orthoMtx = clipRemap * orthoMtx;
			std::memcpy(uniformBlock.viewProj2DMat, orthoMtx.a, sizeof(uniformBlock.viewProj2DMat));
		}
		RIDescriptor passDescriptor = {};
		RI.UpdateFrameUBO(&passDescriptor, (void*)&uniformBlock, sizeof(uniformBlock));

		////////////////////////////////////////////
		// Y-flipped viewport to the target extent — same convention as the
		// scene passes, so the unmodified projection lands the right way up.
		{
			RIViewport viewport_vk = {};
			viewport_vk.y = (float)alTargetHeight;
			viewport_vk.width = (float)alTargetWidth;
			viewport_vk.height = -(float)alTargetHeight;
			viewport_vk.depthMax = 1.0f;
			RIRect scissor = {};
			scissor.width = (int16_t)alTargetWidth;
			scissor.height = (int16_t)alTargetHeight;
			cmd->setViewport(&RI.renderer, viewport_vk);
			cmd->setScissor(&RI.renderer, scissor);
		}

		////////////////////////////////////////////
		// Reserve one vertex/index window for the whole flush.
		const size_t numVertices = m_lineSegments.size() * 2 + m_colorTriangles.size() * 3 +
								   m_colorQuads.size() * 4 + m_uvQuads.size() * 4 +
								   m_line2DSegments.size() * 2;
		const size_t numIndices = m_lineSegments.size() * 2 + m_colorTriangles.size() * 3 +
								  m_colorQuads.size() * 6 + m_uvQuads.size() * 6 +
								  m_line2DSegments.size() * 2;
		RISegmentReq vtxReq = {};
		RISegmentReq idxReq = {};
		if(!RequestStream(cntx, numVertices, numIndices, &vtxReq, &idxReq)) {
			Reset();
			return;
		}
		const RIDeviceSize vtxBase = (RIDeviceSize)vtxReq.elementOffset * sizeof(DebugVertex);
		const RIDeviceSize idxBase = (RIDeviceSize)idxReq.elementOffset * sizeof(uint32_t);
		DebugVertex* vboMemory = reinterpret_cast<DebugVertex*>((uint8_t*)m_vertexBuffer.mappedAddress + vtxBase);
		uint32_t* eleMemory = reinterpret_cast<uint32_t*>((uint8_t*)m_indexBuffer.mappedAddress + idxBase);

		size_t vertexCursor = 0; // in vertices, relative to vtxBase
		size_t indexCursor = 0;  // in indices, relative to idxBase

		// Per-run draw: vertex/index buffers bound at the run's byte offset,
		// indices written 0-based within the run (GuiSet convention).
		auto drawRun = [&](RIProgram& aProgram, size_t alRunVertexStart, size_t alRunIndexStart,
						   size_t alRunIndexCount, RIProgram::DescriptorBinding* apBindings, size_t alNumBindings) {
			aProgram.bindDescriptors(&RI.device, cmd, RI.frameIndex, apBindings, alNumBindings);
			const RIDeviceSize vbOffset = vtxBase + alRunVertexStart * sizeof(DebugVertex);
			const RIDeviceSize ibOffset = idxBase + alRunIndexStart * sizeof(uint32_t);
			struct RIBuffer *vbufs[1] = { &m_vertexBuffer };
			cmd->bindVertexBuffers<1>(0, 1, vbufs, &vbOffset);
			cmd->bindIndexBuffer(&RI.renderer, &m_indexBuffer, ibOffset, RI_INDEX_TYPE_32);
			cmd->drawIndexed(&RI.renderer, (uint32_t)alRunIndexCount, 1, 0, 0, 0);
		};

		////////////////////////////////////////////
		// Solid triangles
		if(!m_colorTriangles.empty())
		{
			std::sort(m_colorTriangles.begin(), m_colorTriangles.end(),
					  [](const ColorTriRequest& a, const ColorTriRequest& b) { return a.m_depthTest < b.m_depthTest; });

			auto it = m_colorTriangles.begin();
			while(it != m_colorTriangles.end())
			{
				const size_t runVertexStart = vertexCursor;
				const size_t runIndexStart = indexCursor;
				size_t runVertexCount = 0;
				size_t runIndexCount = 0;
				const DebugDepthTest depthTest = it->m_depthTest;
				do {
					const cColor& c = it->m_color;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v1.x, it->m_v1.y, it->m_v1.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v2.x, it->m_v2.y, it->m_v2.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v3.x, it->m_v3.y, it->m_v3.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					++it;
				} while(it != m_colorTriangles.end() && it->m_depthTest == depthTest);
				vertexCursor += runVertexCount;
				indexCursor += runIndexCount;

				DebugPipelineCfg cfg;
				cfg.topology = RI_TOPOLOGY_TRIANGLE_LIST;
				cfg.depthTestEnable = true;
				cfg.depthOp = toCompareOp(depthTest);
				cfg.colorFormat = aColorFormat;
				bindDebugPipeline(m_colorProgram, cmd, cfg, "debug.solidTri");

				RIProgram::DescriptorBinding bindings[1] = {};
				bindings[0].descriptor = passDescriptor;
				bindings[0].handle = DescriptorBindingID::Create("pass");
				drawRun(m_colorProgram, runVertexStart, runIndexStart, runIndexCount, bindings, 1);
			}
		}

		////////////////////////////////////////////
		// Solid quads
		if(!m_colorQuads.empty())
		{
			std::sort(m_colorQuads.begin(), m_colorQuads.end(),
					  [](const ColorQuadRequest& a, const ColorQuadRequest& b) { return a.m_depthTest < b.m_depthTest; });

			auto it = m_colorQuads.begin();
			while(it != m_colorQuads.end())
			{
				const size_t runVertexStart = vertexCursor;
				const size_t runIndexStart = indexCursor;
				size_t runVertexCount = 0;
				size_t runIndexCount = 0;
				const DebugDepthTest depthTest = it->m_depthTest;
				do {
					const cColor& c = it->m_color;
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v1.x, it->m_v1.y, it->m_v1.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v2.x, it->m_v2.y, it->m_v2.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v3.x, it->m_v3.y, it->m_v3.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_v4.x, it->m_v4.y, it->m_v4.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };

					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 4;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 3;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 2;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 3;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 2;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 1;
					++it;
				} while(it != m_colorQuads.end() && it->m_depthTest == depthTest);
				vertexCursor += runVertexCount;
				indexCursor += runIndexCount;

				DebugPipelineCfg cfg;
				cfg.topology = RI_TOPOLOGY_TRIANGLE_LIST;
				cfg.depthTestEnable = true;
				cfg.depthOp = toCompareOp(depthTest);
				cfg.colorFormat = aColorFormat;
				bindDebugPipeline(m_colorProgram, cmd, cfg, "debug.solidQuad");

				RIProgram::DescriptorBinding bindings[1] = {};
				bindings[0].descriptor = passDescriptor;
				bindings[0].handle = DescriptorBindingID::Create("pass");
				drawRun(m_colorProgram, runVertexStart, runIndexStart, runIndexCount, bindings, 1);
			}
		}

		////////////////////////////////////////////
		// Textured quads / billboards — batched by (texture, depthTest).
		if(!m_uvQuads.empty())
		{
			std::sort(m_uvQuads.begin(), m_uvQuads.end(), [](const UVQuadRequest& a, const UVQuadRequest& b) {
				if(a.m_texture != b.m_texture) {
					return a.m_texture < b.m_texture;
				}
				return a.m_depthTest < b.m_depthTest;
			});

			// Billboards face the camera: rotation = transpose of the view
			// rotation, translation = billboard position.
			const cMatrixf mtxViewRotInv = apFrustum->GetViewMatrix().GetRotation().GetTranspose();

			auto it = m_uvQuads.begin();
			while(it != m_uvQuads.end())
			{
				const size_t runVertexStart = vertexCursor;
				const size_t runIndexStart = indexCursor;
				size_t runVertexCount = 0;
				size_t runIndexCount = 0;
				const DebugDepthTest depthTest = it->m_depthTest;
				const std::shared_ptr<cTexture> texture = it->m_texture;
				do {
					const cColor& c = it->m_color;
					const float color[4] = { c.r, c.g, c.b, c.a };
					const cVector2f& uv0 = it->m_uv0;
					const cVector2f& uv1 = it->m_uv1;
					cVector3f v1, v2, v3, v4;
					if(auto* quad = std::get_if<UVQuadRequest::Quad>(&it->m_type)) {
						v1 = quad->m_v1; v2 = quad->m_v2; v3 = quad->m_v3; v4 = quad->m_v4;
					} else {
						const auto& billboard = std::get<UVQuadRequest::Billboard>(it->m_type);
						cMatrixf mtxBillboard = mtxViewRotInv;
						mtxBillboard.SetTranslation(billboard.m_pos);
						const cMatrixf mtxFinal = cMath::MatrixMul(billboard.m_transform, mtxBillboard);
						const cVector2f vHalf = billboard.m_size * 0.5f;
						v1 = cMath::MatrixMul(mtxFinal, cVector3f( vHalf.x,  vHalf.y, 0));
						v2 = cMath::MatrixMul(mtxFinal, cVector3f(-vHalf.x,  vHalf.y, 0));
						v3 = cMath::MatrixMul(mtxFinal, cVector3f( vHalf.x, -vHalf.y, 0));
						v4 = cMath::MatrixMul(mtxFinal, cVector3f(-vHalf.x, -vHalf.y, 0));
					}
					vboMemory[vertexCursor + runVertexCount++] = { { v1.x, v1.y, v1.z }, { uv0.x, uv0.y }, { color[0], color[1], color[2], color[3] } };
					vboMemory[vertexCursor + runVertexCount++] = { { v2.x, v2.y, v2.z }, { uv1.x, uv0.y }, { color[0], color[1], color[2], color[3] } };
					vboMemory[vertexCursor + runVertexCount++] = { { v3.x, v3.y, v3.z }, { uv0.x, uv1.y }, { color[0], color[1], color[2], color[3] } };
					vboMemory[vertexCursor + runVertexCount++] = { { v4.x, v4.y, v4.z }, { uv1.x, uv1.y }, { color[0], color[1], color[2], color[3] } };

					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 4;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 3;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 2;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 3;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 2;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount - 1;
					++it;
				} while(it != m_uvQuads.end() && it->m_depthTest == depthTest && it->m_texture == texture);
				vertexCursor += runVertexCount;
				indexCursor += runIndexCount;

				DebugPipelineCfg cfg;
				cfg.topology = RI_TOPOLOGY_TRIANGLE_LIST;
				cfg.depthTestEnable = true;
				cfg.depthOp = toCompareOp(depthTest);
				cfg.alphaBlend = true; // icons have cutout alpha
				cfg.uvLayout = true;
				cfg.colorFormat = aColorFormat;
				bindDebugPipeline(m_uvProgram, cmd, cfg, "debug.uvQuad");

				// Pin the texture so a mid-frame destroy can't free the image
				// before this submit retires.
				cntx->resourceLink.push_back(texture);

				auto samplerDesc = RI.resolve_filter_descriptor(
					eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
					eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
				assert(samplerDesc);

				RIProgram::DescriptorBinding bindings[3] = {};
				bindings[0].descriptor = passDescriptor;
				bindings[0].handle = DescriptorBindingID::Create("pass");
				bindings[1].descriptor = *samplerDesc;
				bindings[1].handle = DescriptorBindingID::Create("diffuseSampler");
				bindings[2].descriptor = texture->binding;
				bindings[2].handle = DescriptorBindingID::Create("diffuseMap");
				drawRun(m_uvProgram, runVertexStart, runIndexStart, runIndexCount, bindings, 3);
			}
		}

		////////////////////////////////////////////
		// 3D lines
		if(!m_lineSegments.empty())
		{
			std::sort(m_lineSegments.begin(), m_lineSegments.end(),
					  [](const LineSegmentRequest& a, const LineSegmentRequest& b) { return a.m_depthTest < b.m_depthTest; });

			auto it = m_lineSegments.begin();
			while(it != m_lineSegments.end())
			{
				const size_t runVertexStart = vertexCursor;
				const size_t runIndexStart = indexCursor;
				size_t runVertexCount = 0;
				size_t runIndexCount = 0;
				const DebugDepthTest depthTest = it->m_depthTest;
				do {
					const cColor& c = it->m_color;
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_start.x, it->m_start.y, it->m_start.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
					vboMemory[vertexCursor + runVertexCount++] = { { it->m_end.x, it->m_end.y, it->m_end.z }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
					++it;
				} while(it != m_lineSegments.end() && it->m_depthTest == depthTest);
				vertexCursor += runVertexCount;
				indexCursor += runIndexCount;

				DebugPipelineCfg cfg;
				cfg.topology = RI_TOPOLOGY_LINE_LIST;
				cfg.depthTestEnable = true;
				cfg.depthOp = toCompareOp(depthTest);
				cfg.colorFormat = aColorFormat;
				bindDebugPipeline(m_colorProgram, cmd, cfg, "debug.line");

				RIProgram::DescriptorBinding bindings[1] = {};
				bindings[0].descriptor = passDescriptor;
				bindings[0].handle = DescriptorBindingID::Create("pass");
				drawRun(m_colorProgram, runVertexStart, runIndexStart, runIndexCount, bindings, 1);
			}
		}

		////////////////////////////////////////////
		// 2D lines (drawn last — screen-space overlay on top of everything)
		if(!m_line2DSegments.empty())
		{
			const size_t runVertexStart = vertexCursor;
			const size_t runIndexStart = indexCursor;
			size_t runVertexCount = 0;
			size_t runIndexCount = 0;
			for(const auto& segment : m_line2DSegments)
			{
				const cColor& c = segment.m_color;
				eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
				vboMemory[vertexCursor + runVertexCount++] = { { segment.m_start.x, segment.m_start.y, 0.0f }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
				eleMemory[indexCursor + runIndexCount++] = (uint32_t)runVertexCount;
				vboMemory[vertexCursor + runVertexCount++] = { { segment.m_end.x, segment.m_end.y, 0.0f }, { 0, 0 }, { c.r, c.g, c.b, c.a } };
			}
			vertexCursor += runVertexCount;
			indexCursor += runIndexCount;

			DebugPipelineCfg cfg;
			cfg.topology = RI_TOPOLOGY_LINE_LIST;
			cfg.depthTestEnable = false;
			cfg.depthOp = RI_COMPARE_ALWAYS;
			cfg.colorFormat = aColorFormat;
			bindDebugPipeline(m_color2DProgram, cmd, cfg, "debug.line2D");

			RIProgram::DescriptorBinding bindings[1] = {};
			bindings[0].descriptor = passDescriptor;
			bindings[0].handle = DescriptorBindingID::Create("pass");
			drawRun(m_color2DProgram, runVertexStart, runIndexStart, runIndexCount, bindings, 1);
		}

		assert(vertexCursor == numVertices && indexCursor == numIndices);
		Reset();
	}

	//-----------------------------------------------------------------------

} // namespace hpl
