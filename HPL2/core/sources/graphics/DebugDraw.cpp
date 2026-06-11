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

		VkCompareOp toCompareOp(DebugDraw::DebugDepthTest aTest)
		{
			switch(aTest) {
			case DebugDraw::DebugDepthTest::None:         return VK_COMPARE_OP_NEVER;
			case DebugDraw::DebugDepthTest::Less:         return VK_COMPARE_OP_LESS;
			case DebugDraw::DebugDepthTest::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
			case DebugDraw::DebugDepthTest::Equal:        return VK_COMPARE_OP_EQUAL;
			case DebugDraw::DebugDepthTest::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case DebugDraw::DebugDepthTest::Greater:      return VK_COMPARE_OP_GREATER;
			case DebugDraw::DebugDepthTest::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
			case DebugDraw::DebugDepthTest::Always:       return VK_COMPARE_OP_ALWAYS;
			default: break;
			}
			assert(false && "unhandled DebugDepthTest");
			return VK_COMPARE_OP_ALWAYS;
		}

		struct DebugPipelineCfg {
			VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			bool depthTestEnable = true;
			VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL;
			bool alphaBlend = false;   // false => additive ONE/ONE (TDD overlay look)
			bool uvLayout = false;     // declare the uv attribute (debug_uv.vert)
			VkFormat colorFormat = RIBootstrap::PogoColorFormatVk;
		};

		// One cached pipeline per (topology, depth, blend, format) combination —
		// non-bindless template cloned from GuiSet.cpp / RendererWireFrame.cpp.
		void bindDebugPipeline(RIProgram& aProgram, struct RICmd* cmd,
							   const DebugPipelineCfg& aCfg, const char* asDebugName)
		{
			// Unified DebugVertex stream (stride 36); color-only stages skip uv.
			VkVertexInputAttributeDescription colorAttribs[] = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },     // position
				{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 }, // color
			};
			VkVertexInputAttributeDescription uvAttribs[] = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },     // position
				{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },       // uv
				{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 }, // color
			};
			VkVertexInputBindingDescription vertexBindingDesc[] = {
				{ 0, 36, VK_VERTEX_INPUT_RATE_VERTEX }
			};
			VkPipelineVertexInputStateCreateInfo vertexInputState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vertexInputState.pVertexAttributeDescriptions = aCfg.uvLayout ? uvAttribs : colorAttribs;
			vertexInputState.vertexAttributeDescriptionCount = aCfg.uvLayout ? 3 : 2;
			vertexInputState.pVertexBindingDescriptions = vertexBindingDesc;
			vertexInputState.vertexBindingDescriptionCount = 1;

			VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			inputAssemblyState.topology = aCfg.topology;

			VkPipelineRasterizationStateCreateInfo rasterizationState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
			rasterizationState.cullMode = VK_CULL_MODE_NONE; // sidesteps the CW-front-face trap
			rasterizationState.lineWidth = 1.0f;

			VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
			dynamicState.pDynamicStates = dynamicStates;

			VkPipelineRenderingCreateInfo pipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
			VkFormat colorFormats[1] = { aCfg.colorFormat };
			pipelineRenderingCreateInfo.colorAttachmentCount = 1;
			pipelineRenderingCreateInfo.pColorAttachmentFormats = colorFormats;
			pipelineRenderingCreateInfo.depthAttachmentFormat = RIFormatToVK(RIBootstrap::DepthFormat);
			pipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

			VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			VkPipelineMultisampleStateCreateInfo multisampleState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			// Overlay geometry never writes depth — it tests against the scene.
			VkPipelineDepthStencilStateCreateInfo depthStencilState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			depthStencilState.depthTestEnable = aCfg.depthTestEnable ? VK_TRUE : VK_FALSE;
			depthStencilState.depthWriteEnable = VK_FALSE;
			depthStencilState.depthCompareOp = aCfg.depthOp;
			depthStencilState.minDepthBounds = 0.0f;
			depthStencilState.maxDepthBounds = 1.0f;

			// RGB-only write mask: keep the scene's alpha channel intact.
			VkPipelineColorBlendAttachmentState blendAttachmentState[1] = {};
			blendAttachmentState[0].blendEnable = VK_TRUE;
			blendAttachmentState[0].colorBlendOp = VK_BLEND_OP_ADD;
			blendAttachmentState[0].alphaBlendOp = VK_BLEND_OP_ADD;
			blendAttachmentState[0].colorWriteMask =
				VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
			if(aCfg.alphaBlend) {
				blendAttachmentState[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blendAttachmentState[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachmentState[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blendAttachmentState[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			} else {
				blendAttachmentState[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachmentState[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			}
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

			hash_t hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)aCfg.topology);
			hash = hash_u32(hash, (uint32_t)aCfg.depthTestEnable);
			hash = hash_u32(hash, (uint32_t)aCfg.depthOp);
			hash = hash_u32(hash, (uint32_t)aCfg.alphaBlend);
			hash = hash_u32(hash, (uint32_t)aCfg.colorFormat);
			aProgram.bindPipeline(&RI.device, cmd, hash, asDebugName, &pipelineCreateInfo);
		}

	} // namespace

	//-----------------------------------------------------------------------

	void DebugDraw::Init(cResources* apResources)
	{
		auto loadProgram = [&](RIProgram& aProgram, const char* asVert, const char* asFrag) {
			auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), asVert);
			auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), asFrag);
			std::array<RIProgram::ModuleStage, 2> stages = {
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
			};
			aProgram.initialize(&RI.device, stages);
		};
		loadProgram(m_colorProgram, "debug.vert.spv", "debug.frag.spv");
		loadProgram(m_color2DProgram, "debug_2d.vert.spv", "debug.frag.spv");
		loadProgram(m_uvProgram, "debug_uv.vert.spv", "debug_uv.frag.spv");
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

			uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
			VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			VK_ConfigureBufferQueueFamilies(&bufferCreateInfo, RI.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);
			bufferCreateInfo.size = (VkDeviceSize)segmentAllocDesc.maxElements * segmentAllocDesc.elementStride;
			bufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

			VmaAllocationInfo allocationInfo = { 0 };
			VmaAllocationCreateInfo allocInfo = { 0 };
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

			if(m_vertexBuffer.vk.buffer) {
				cntx->freelist.push_back(m_vertexBuffer);
			}
			VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bufferCreateInfo, &allocInfo,
										  &m_vertexBuffer.vk.buffer, &m_vertexBuffer.vk.allocation, &allocationInfo));
			m_vertexBuffer.mappedAddress = allocationInfo.pMappedData;
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

			uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
			VkBufferCreateInfo bufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			VK_ConfigureBufferQueueFamilies(&bufferCreateInfo, RI.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);
			bufferCreateInfo.size = (VkDeviceSize)segmentAllocDesc.maxElements * segmentAllocDesc.elementStride;
			bufferCreateInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

			VmaAllocationInfo allocationInfo = { 0 };
			VmaAllocationCreateInfo allocInfo = { 0 };
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

			if(m_indexBuffer.vk.buffer) {
				cntx->freelist.push_back(m_indexBuffer);
			}
			VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bufferCreateInfo, &allocInfo,
										  &m_indexBuffer.vk.buffer, &m_indexBuffer.vk.allocation, &allocationInfo));
			m_indexBuffer.mappedAddress = allocationInfo.pMappedData;
		}
		return true;
	}

	//-----------------------------------------------------------------------

	void DebugDraw::flush(RIBootstrap::FrameContext* cntx, struct RICmd* cmd, const cFrustum* apFrustum,
						  uint32_t alTargetWidth, uint32_t alTargetHeight, VkFormat aColorFormat)
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
			VkViewport viewport_vk = { 0.0f, (float)alTargetHeight,
									   (float)alTargetWidth, -(float)alTargetHeight,
									   0.0f, 1.0f };
			VkRect2D scissor = { { 0, 0 }, { alTargetWidth, alTargetHeight } };
			vkCmdSetViewport(cmd->vk.cmd, 0, 1, &viewport_vk);
			vkCmdSetScissor(cmd->vk.cmd, 0, 1, &scissor);
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
		const VkDeviceSize vtxBase = (VkDeviceSize)vtxReq.elementOffset * sizeof(DebugVertex);
		const VkDeviceSize idxBase = (VkDeviceSize)idxReq.elementOffset * sizeof(uint32_t);
		DebugVertex* vboMemory = reinterpret_cast<DebugVertex*>((uint8_t*)m_vertexBuffer.mappedAddress + vtxBase);
		uint32_t* eleMemory = reinterpret_cast<uint32_t*>((uint8_t*)m_indexBuffer.mappedAddress + idxBase);

		size_t vertexCursor = 0; // in vertices, relative to vtxBase
		size_t indexCursor = 0;  // in indices, relative to idxBase

		// Per-run draw: vertex/index buffers bound at the run's byte offset,
		// indices written 0-based within the run (GuiSet convention).
		auto drawRun = [&](RIProgram& aProgram, size_t alRunVertexStart, size_t alRunIndexStart,
						   size_t alRunIndexCount, RIProgram::DescriptorBinding* apBindings, size_t alNumBindings) {
			aProgram.bindDescriptors(&RI.device, cmd, RI.frameIndex, apBindings, alNumBindings);
			const VkDeviceSize vbOffset = vtxBase + alRunVertexStart * sizeof(DebugVertex);
			const VkDeviceSize ibOffset = idxBase + alRunIndexStart * sizeof(uint32_t);
			vkCmdBindVertexBuffers(cmd->vk.cmd, 0, 1, &m_vertexBuffer.vk.buffer, &vbOffset);
			vkCmdBindIndexBuffer(cmd->vk.cmd, m_indexBuffer.vk.buffer, ibOffset, VK_INDEX_TYPE_UINT32);
			cmd->drawIndexed((uint32_t)alRunIndexCount, 1, 0, 0, 0);
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
				cfg.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
				cfg.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
				cfg.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				cfg.depthTestEnable = true;
				cfg.depthOp = toCompareOp(depthTest);
				cfg.alphaBlend = true; // icons have cutout alpha
				cfg.uvLayout = true;
				cfg.colorFormat = aColorFormat;
				bindDebugPipeline(m_uvProgram, cmd, cfg, "debug.uvQuad");

				// Pin the texture so a mid-frame destroy can't free the VkImage
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
				cfg.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
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
			cfg.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			cfg.depthTestEnable = false;
			cfg.depthOp = VK_COMPARE_OP_ALWAYS;
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
