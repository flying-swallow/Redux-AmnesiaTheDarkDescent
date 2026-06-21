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

#include "LuxScreenEffect.h"

#include "LuxBase.h"
#include "LuxScreenCapture.h"

#include "gui/Gui.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgramHelpers.h"

using namespace hpl;

//-----------------------------------------------------------------------

void cLuxScreenEffect::Init(hpl::cGui *apGui, Effect effect)
{
	mpGui   = apGui;
	mEffect = effect;

	// Both effects sample the (upright) shared capture, so both use the
	// non-flipping shared fullscreen vert. vsMain/psMain.
	//   DesaturateDarken: inventory_post.frag (single pass).
	//   Blur: posteffect_bloom_blur.frag (H/V ping-pong in OnPostRender).
	const char *vert = "posteffect_fullscreen.vert.spv";
	const char *frag = (mEffect == Effect::Blur)
		? "posteffect_bloom_blur.frag.spv" : "inventory_post.frag.spv";

	LoadSlangGraphics(&RI.device, m_postProgram, gpBase->mpEngine->GetResources(),
	                  vert, frag);
}

//-----------------------------------------------------------------------

void cLuxScreenEffect::CreateTextures()
{
	const uint32_t w = RI.swapchain.width;
	const uint32_t h = RI.swapchain.height;

	m_screenBgImage = LuxCreateScreenRenderTarget(w, h, RI_FORMAT_RGBA8_UNORM,
	                            "screen.effectBg");

	// The separable blur ping-pongs through a scratch target between the shared
	// sharp copy and the blurred result.
	if (mEffect == Effect::Blur)
		m_screenScratchImage = LuxCreateScreenRenderTarget(w, h, RI_FORMAT_RGBA8_UNORM,
		                            "screen.effectBlurTmp");

	mpScreenBgGfx   = mpGui->CreateGfxTexture(m_screenBgImage.Get(), false, eGuiMaterial_Alpha);
}

//-----------------------------------------------------------------------

void cLuxScreenEffect::RequestCapture()
{
	gpBase->GetScreenCapture()->RequestCapture();
	mbApplyPending = true;
	mbApplied      = false;
}

//-----------------------------------------------------------------------

hpl::cGuiGfxElement* cLuxScreenEffect::GetScreenGfx()
{
	return gpBase->GetScreenCapture()->GetScreenGfx();
}

//-----------------------------------------------------------------------

void cLuxScreenEffect::Destroy()
{
	if (mpScreenBgGfx) mpGui->DestroyGfx(mpScreenBgGfx);
	mpScreenBgGfx = nullptr;

	// Dropping the handles frees the Images (and their cTextures) once any
	// in-flight per-frame keep-alive pins on the active slot have been released.
	m_screenBgImage = {};
	m_screenScratchImage = {};

	mbApplyPending = false;
	mbApplied      = false;
}

//-----------------------------------------------------------------------

void cLuxScreenEffect::OnPostRender()
{
	if (!mbApplyPending) {
		return;
	}
	if (!m_screenBgImage || m_postProgram.getPipelineLayout() == VK_NULL_HANDLE) {
		return;
	}
	cTexture *bgColor = m_screenBgImage->GetTexture();
	cTexture *scratch = m_screenScratchImage ? m_screenScratchImage->GetTexture() : nullptr;

	cLuxScreenCapture *pCapture = gpBase->GetScreenCapture();
	if (!pCapture->IsCaptured()) {
		return; // The shared capture has not landed yet; try again next frame.
	}
	RIDescriptor source = pCapture->PrimaryDescriptor();
	if (source.vkImageView() == VK_NULL_HANDLE) {
		return;
	}

	VkCommandBuffer cmd = RI.primary.cmds[0].vk.cmd;
	if (cmd == VK_NULL_HANDLE) {
		return; // No active frame to ride on; try again next post-render.
	}

	const cVector2l size = pCapture->GetSize();
	const uint32_t w = static_cast<uint32_t>(size.x);
	const uint32_t h = static_cast<uint32_t>(size.y);

	// Keep the effect target alive until the GPU has consumed it.
	RI.graphicsDefer.push(PinResource(m_screenBgImage.Get()));
	if (m_screenScratchImage) RI.graphicsDefer.push(PinResource(m_screenScratchImage.Get()));

	////////////////////////////////////////////////
	// Blurred backdrop (escape menu): separable gaussian ping-pong.
	// Reuses the bloom separable-blur frag; H then V between m_screenScratch
	// and m_screenBgColor, the first H reading the shared sharp copy.
	// After the loop the blurred result lives in m_screenBgColor.
	////////////////////////////////////////////////
	if (mEffect == Effect::Blur)
	{
		PostEffectPipelineState blurState{};
		InitPostEffectPipelineState(blurState, RI_FORMAT_RGBA8_UNORM, false);
		const hash_t kBlurHash = hash_u32(HASH_INITIAL_VALUE, 0u);

		auto samplerDesc = RI.resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		assert(samplerDesc);

		struct BlurPC { float blurDir[2]; float _pad[2]; }; // matches BloomBlurPC

		VkViewport viewport = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
		VkRect2D   scissor   = { { 0, 0 }, { w, h } };

		auto blurPass = [&](cTexture *dst, const RIDescriptor &srcDesc,
		                    float dirX, float dirY)
		{
			// dst → color attachment (discard old contents; wait on any prior
			// sampling of dst — covers the WAR hazard on the ping-pong).
			RITextureBarrier toTarget(&dst->handle,
					RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
					RI_STAGE_FRAGMENT, RI_STAGE_NONE);
			RI.primary.cmds[0].vk_d3d12_textureBarrier(toTarget);

			RITextureView dstView = dst->view;
			RIRenderingAttachment color = {};
			color.view    = dstView;
			color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
			color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

			RIBeginRenderingDesc beginDesc = {};
			beginDesc.renderArea.width  = (int16_t)w;
			beginDesc.renderArea.height = (int16_t)h;
			beginDesc.colorCount = 1;
			beginDesc.colors     = &color;
			RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdSetScissor(cmd, 0, 1, &scissor);

			m_postProgram.bindPipeline(&RI.device, &RI.primary.cmds[0], kBlurHash,
			                           "screen.blur", &blurState.createInfo);

			RIProgram::DescriptorBinding bindings[2] = {};
			bindings[0].descriptor = *samplerDesc;
			bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
			bindings[1].descriptor = srcDesc;
			bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
			m_postProgram.bindDescriptors(&RI.device, &RI.primary.cmds[0],
			                              RI.frameIndex, bindings, 2);

			BlurPC pc = { { dirX, dirY }, { 0.0f, 0.0f } };
			vkCmdPushConstants(cmd, m_postProgram.getPipelineLayout(),
			                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

			vkCmdDraw(cmd, 3, 1, 0, 0);
			RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);

			// dst → sampleable for the next pass / the GUI.
			RITextureBarrier toSampled(&dst->handle,
					RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
					RI_STAGE_NONE, RI_STAGE_FRAGMENT);
			RI.primary.cmds[0].vk_d3d12_textureBarrier(toSampled);
		};

		const float kBlurSize   = 2.0f; // texel multiplier per tap (tunable)
		const int   kIterations = 6;    // H+V pairs; more = heavier blur
		for (int i = 0; i < kIterations; ++i)
		{
			const RIDescriptor hIn = (i == 0) ? source
			                                  : bgColor->descriptor();
			blurPass(scratch, hIn,                  kBlurSize, 0.0f);
			blurPass(bgColor, scratch->descriptor(), 0.0f, kBlurSize);
		}
		// bgColor ends SHADER_READ_ONLY (last blurPass).

		mbApplyPending = false;
		mbApplied      = true;
		return;
	}

	RITextureBarrier bgToTarget(&bgColor->handle,
			RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
			RI_STAGE_NONE, RI_STAGE_NONE);
	RI.primary.cmds[0].vk_d3d12_textureBarrier(bgToTarget);

	////////////////////////////////////////////////
	// Fullscreen post-effect — sample the shared capture, write bgColor.
	////////////////////////////////////////////////

	RITextureView bgColorView = bgColor->view;
	RIRenderingAttachment color = {};
	color.view    = bgColorView;
	color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

	RIBeginRenderingDesc beginDesc = {};
	beginDesc.renderArea.width  = (int16_t)w;
	beginDesc.renderArea.height = (int16_t)h;
	beginDesc.colorCount = 1;
	beginDesc.colors     = &color;
	RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

	VkViewport viewport = {};
	viewport.x = 0.0f; viewport.y = 0.0f;
	viewport.width = (float)w; viewport.height = (float)h;
	viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, { w, h } };
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	// Build the (cached) pipeline. No vertex input — vertex shader synthesises
	// the fullscreen triangle from gl_VertexIndex.
	const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfo pipelineRendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	pipelineRendering.colorAttachmentCount    = 1;
	pipelineRendering.pColorAttachmentFormats = &colorFormat;

	VkPipelineVertexInputStateCreateInfo vertexInputState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

	VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineRasterizationStateCreateInfo rasterizationState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizationState.cullMode    = VK_CULL_MODE_NONE;
	rasterizationState.lineWidth   = 1.0f;

	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount  = 1;

	VkPipelineMultisampleStateCreateInfo multisampleState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencilState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo colorBlendState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	colorBlendState.attachmentCount = 1;
	colorBlendState.pAttachments    = &blendAttachment;

	VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
	dynamicState.pDynamicStates    = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineCreateInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipelineCreateInfo.pNext               = &pipelineRendering;
	pipelineCreateInfo.pVertexInputState   = &vertexInputState;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
	pipelineCreateInfo.pRasterizationState = &rasterizationState;
	pipelineCreateInfo.pViewportState      = &viewportState;
	pipelineCreateInfo.pMultisampleState   = &multisampleState;
	pipelineCreateInfo.pDepthStencilState  = &depthStencilState;
	pipelineCreateInfo.pColorBlendState    = &colorBlendState;
	pipelineCreateInfo.pDynamicState       = &dynamicState;

	const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)colorFormat);
	m_postProgram.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineHash,
	                           "screen.post", &pipelineCreateInfo);

	auto samplerDesc = RI.resolve_filter_descriptor(
		eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
	assert(samplerDesc);

	RIProgram::DescriptorBinding bindings[2] = {};
	bindings[0].descriptor = *samplerDesc;
	bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
	bindings[1].descriptor = source;
	bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
	m_postProgram.bindDescriptors(&RI.device, &RI.primary.cmds[0],
	                              RI.frameIndex, bindings, 2);

	vkCmdDraw(cmd, 3, 1, 0, 0);
	RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);

	// bgColor → SHADER_READ_ONLY so subsequent GUI draws can sample it.
	RITextureBarrier bgToSampled(&bgColor->handle,
			RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
			RI_STAGE_NONE, RI_STAGE_FRAGMENT);
	RI.primary.cmds[0].vk_d3d12_textureBarrier(bgToSampled);

	mbApplyPending = false;
	mbApplied      = true;
}

//-----------------------------------------------------------------------
