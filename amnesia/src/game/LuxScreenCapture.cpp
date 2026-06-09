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

#include "LuxScreenCapture.h"

#include "LuxBase.h"

#include "gui/Gui.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/RIPogoBuffer.h"

using namespace hpl;

//-----------------------------------------------------------------------

// Allocate `outTex` as a screen-sized RGBA8 color attachment that is also
// sampleable as a 2D texture, and wire up the descriptor binding used by
// cGui::CreateGfxTexture / RIProgram::bindDescriptors. The HPLTexture_Delete
// deleter expects `handle.vk.image` (+ its VMA allocation) and `binding.vk.image.imageView`
// to be set — see HPLTexture.cpp:19 — so we fill all three.
static bool CreateScreenRenderTarget(
		std::shared_ptr<hpl::HPLTexture> &outTex,
		uint32_t width, uint32_t height, const char *debugName)
{
	auto tex = std::shared_ptr<HPLTexture>(new HPLTexture{}, HPLTexture::HPLTexture_Delete);
	tex->width  = static_cast<uint16_t>(width);
	tex->height = static_cast<uint16_t>(height);
	tex->depth  = 1;
	tex->mipNum = 1;

	const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType   = VK_IMAGE_TYPE_2D;
	imageInfo.format      = format;
	imageInfo.extent      = { width, height, 1 };
	imageInfo.mipLevels   = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples     = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
	                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo memReqs = {};
	memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	if (!VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imageInfo, &memReqs,
	                                  &tex->handle.vk.image, &tex->handle.vk.allocation, NULL))) {
		return false;
	}

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image    = tex->handle.vk.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = format;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	tex->binding = {};
	tex->binding.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
	tex->binding.texture = &tex->handle;
	tex->binding.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	tex->binding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (!VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
	                                     &tex->binding.vk.image.imageView))) {
		return false;
	}
	tex->binding.finalize(&RI.device);
	tex->setDebugName(debugName);

	outTex = std::move(tex);
	return true;
}

//-----------------------------------------------------------------------

// Lay down a single image barrier on the active command buffer.
static void ImageBarrier(RICmd_s *cmd, RITexture_s *texture,
		uint32_t before, uint32_t beforeStages,
		uint32_t after, uint32_t afterStages)
{
	RITextureBarrier_s barrier(texture, before, after, beforeStages, afterStages);
	barrier.mipCount = 1;
	barrier.layerCount = 1;
	cmd->textureBarrier(barrier);
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::Init(hpl::cGui *apGui, Effect effect)
{
	mpGui   = apGui;
	mEffect = effect;

	// Both effects sample the (upright) m_screenColor copy, so both use the
	// non-flipping shared fullscreen vert. inventory_post.vert's Y-flip would
	// invert the backdrop (it went unnoticed only while the capture source was
	// garbage; the pogo scene-color shows the flip plainly). vsMain/psMain.
	//   DesaturateDarken: inventory_post.frag (single pass).
	//   Blur: posteffect_bloom_blur.frag (H/V ping-pong in OnPostRender).
	const char *vert = "posteffect_fullscreen.vert.spv";
	const char *frag = (mEffect == Effect::Blur)
		? "posteffect_bloom_blur.frag.spv" : "inventory_post.frag.spv";

	LoadSlangGraphics(&RI.device, m_postProgram, gpBase->mpEngine->GetResources(),
	                  vert, frag);

	// Passthrough used to copy the pogo scene-color into m_screenColor — the
	// same trivial blit the renderer's tail pass uses.
	LoadSlangGraphics(&RI.device, m_copyProgram, gpBase->mpEngine->GetResources(),
	                  "posteffect_fullscreen.vert.spv", "posteffect_blit.frag.spv");
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::CreateTextures()
{
	const uint32_t w = RI.swapchain.width;
	const uint32_t h = RI.swapchain.height;

	CreateScreenRenderTarget(m_screenColor,   w, h, "screen.capture");
	CreateScreenRenderTarget(m_screenBgColor, w, h, "screen.captureBg");

	// The separable blur ping-pongs through a scratch target between the
	// sharp copy and the blurred result.
	if (mEffect == Effect::Blur)
		CreateScreenRenderTarget(m_screenScratch, w, h, "screen.captureBlurTmp");

	m_screenImage   = std::make_shared<Image>(Image::SingleImage{m_screenColor});
	m_screenBgImage = std::make_shared<Image>(Image::SingleImage{m_screenBgColor});

	mpScreenGfx   = mpGui->CreateGfxTexture(m_screenImage.get(),   false, eGuiMaterial_Diffuse);
	mpScreenBgGfx = mpGui->CreateGfxTexture(m_screenBgImage.get(), false, eGuiMaterial_Alpha);
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::RequestCapture()
{
	// The actual GPU capture has to happen while a command buffer is in
	// recording state (between RI::BeginActiveSet and CloseAndSubmitActiveSet)
	// and after the scene has rendered. The owning state requests the capture
	// during input/container handling, so we defer the work to the next
	// OnPostRender — that's the nearest hook that satisfies both constraints.
	// See Engine.cpp:528-540.
	mbPending  = true;
	mbCaptured = false;
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::Destroy()
{
	if(mpScreenGfx)   mpGui->DestroyGfx(mpScreenGfx);
	if(mpScreenBgGfx) mpGui->DestroyGfx(mpScreenBgGfx);
	mpScreenGfx   = nullptr;
	mpScreenBgGfx = nullptr;

	// The shared_ptr deleter (HPLTexture::HPLTexture_Delete) pushes the
	// VkImage / VkImageView / VmaAllocation onto the active frame slot's
	// freelist; they're released when that slot is reused next, by which
	// time the ring fence has signaled. So no explicit queue wait here.
	m_screenImage.reset();
	m_screenBgImage.reset();
	m_screenColor.reset();
	m_screenBgColor.reset();
	m_screenScratch.reset();

	mbPending  = false;
	mbCaptured = false;
}

//-----------------------------------------------------------------------

void cLuxScreenCapture::OnPostRender()
{
	if (!mbPending) {
		return;
	}
	if (!m_screenColor || !m_screenBgColor ||
	    m_postProgram.getPipelineLayout() == VK_NULL_HANDLE ||
	    m_copyProgram.getPipelineLayout() == VK_NULL_HANDLE) {
		return;
	}

	VkCommandBuffer cmd = RI.primary.cmds[0].vk.cmd;
	if (cmd == VK_NULL_HANDLE) {
		return; // No active frame to ride on; try again next post-render.
	}

	const uint32_t w = RI.swapchain.width;
	const uint32_t h = RI.swapchain.height;

	// The clean game scene (world + post-effects, NO GUI) lives in the primary
	// viewport's pogo "read" half — the same image cScene's tail blit samples
	// to write the swapchain. The GUI is composited onto the swapchain only
	// after that blit, so the swapchain is the wrong source. The
	// menu/inventory viewport draws no world, so the pogo still holds the
	// last gameplay frame. Sample it in a fullscreen pass.
	auto *frameCtx = RI.GetActiveSet();
	cViewport *pViewport = gpBase->mpEngine->GetScene()->GetPrimaryViewport();
	RI_PogoBuffer *pPogo = pViewport ? pViewport->PogoBuffer() : NULL;
	if (pPogo == NULL) {
		return; // No world rendered yet; try again next post-render.
	}
	RIDescriptor_s *sceneColor = RI_PogoBufferShaderResource(pPogo);

	// Keep the textures alive until the GPU has consumed them — BeginActiveSet
	// drains resourceLink only after the frame slot's fence signals.
	frameCtx->resourceLink.push_back(m_screenColor);
	frameCtx->resourceLink.push_back(m_screenBgColor);
	if (m_screenScratch) frameCtx->resourceLink.push_back(m_screenScratch);

	////////////////////////////////////////////////
	// Pass 0: copy the pogo scene-color into m_screenColor (the sharp copy).
	////////////////////////////////////////////////
	{
		PostEffectPipelineState copyState{};
		InitPostEffectPipelineState(copyState, VK_FORMAT_R8G8B8A8_UNORM, false);
		const hash_t kCopyHash = hash_u32(HASH_INITIAL_VALUE, 0u);

		auto samplerDesc = RI.resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		assert(samplerDesc);

		ImageBarrier(&RI.primary.cmds[0], &m_screenColor->handle,
				RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
				RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

		VkRenderingAttachmentInfo attach = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		attach.imageView   = m_screenColor->binding.vk.image.imageView;
		attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

		VkRenderingInfo render = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		render.renderArea           = { { 0, 0 }, { w, h } };
		render.layerCount           = 1;
		render.colorAttachmentCount = 1;
		render.pColorAttachments    = &attach;

		vkCmdBeginRendering(cmd, &render);

		VkViewport viewport = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
		VkRect2D   scissor   = { { 0, 0 }, { w, h } };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		m_copyProgram.bindPipeline(&RI.device, &RI.primary.cmds[0], kCopyHash,
		                           "screen.copy", &copyState.createInfo);

		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *samplerDesc;
		bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = *sceneColor;
		bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
		m_copyProgram.bindDescriptors(&RI.device, &RI.primary.cmds[0],
		                              RI.frameIndex, bindings, 2);

		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);

		// m_screenColor → sampleable for the effect passes and the GUI.
		ImageBarrier(&RI.primary.cmds[0], &m_screenColor->handle,
				RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
				RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
	}

	////////////////////////////////////////////////
	// Blurred backdrop (escape menu): separable gaussian ping-pong.
	// Reuses the bloom separable-blur frag; H then V between m_screenScratch
	// and m_screenBgColor, the first H reading the sharp m_screenColor copy.
	// After the loop the blurred result lives in m_screenBgColor.
	////////////////////////////////////////////////
	if (mEffect == Effect::Blur)
	{
		PostEffectPipelineState blurState{};
		InitPostEffectPipelineState(blurState, VK_FORMAT_R8G8B8A8_UNORM, false);
		const hash_t kBlurHash = hash_u32(HASH_INITIAL_VALUE, 0u);

		auto samplerDesc = RI.resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		assert(samplerDesc);

		struct BlurPC { float blurDir[2]; float _pad[2]; }; // matches BloomBlurPC

		VkViewport viewport = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
		VkRect2D   scissor   = { { 0, 0 }, { w, h } };

		auto blurPass = [&](HPLTexture *dst, const RIDescriptor_s &srcDesc,
		                    float dirX, float dirY)
		{
			// dst → color attachment (discard old contents; wait on any
			// prior sampling of dst — covers the WAR hazard on the ping-pong).
			ImageBarrier(&RI.primary.cmds[0], &dst->handle,
					RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
					RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

			VkRenderingAttachmentInfo attach = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			attach.imageView   = dst->binding.vk.image.imageView;
			attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

			VkRenderingInfo render = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			render.renderArea           = { { 0, 0 }, { w, h } };
			render.layerCount           = 1;
			render.colorAttachmentCount = 1;
			render.pColorAttachments    = &attach;

			vkCmdBeginRendering(cmd, &render);

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
			vkCmdEndRendering(cmd);

			// dst → sampleable for the next pass / the GUI.
			ImageBarrier(&RI.primary.cmds[0], &dst->handle,
					RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
					RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
		};

		const float kBlurSize   = 2.0f; // texel multiplier per tap (tunable)
		const int   kIterations = 6;    // H+V pairs; more = heavier blur
		for (int i = 0; i < kIterations; ++i)
		{
			const RIDescriptor_s &hIn = (i == 0) ? m_screenColor->binding
			                                     : m_screenBgColor->binding;
			blurPass(m_screenScratch.get(), hIn,                     kBlurSize, 0.0f);
			blurPass(m_screenBgColor.get(), m_screenScratch->binding, 0.0f, kBlurSize);
		}
		// m_screenBgColor ends SHADER_READ_ONLY (last blurPass); m_screenColor
		// is still SHADER_READ_ONLY for the sharp-copy quad.

		mbPending  = false;
		mbCaptured = true;
		return;
	}

	ImageBarrier(&RI.primary.cmds[0], &m_screenBgColor->handle,
			RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
			RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

	////////////////////////////////////////////////
	// Pass 2: fullscreen post-effect — sample m_screenColor, write m_screenBgColor.
	////////////////////////////////////////////////

	VkRenderingAttachmentInfo colorAttach = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	colorAttach.imageView   = m_screenBgColor->binding.vk.image.imageView;
	colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	renderInfo.renderArea = { { 0, 0 }, { w, h } };
	renderInfo.layerCount = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments = &colorAttach;

	vkCmdBeginRendering(cmd, &renderInfo);

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
	bindings[1].descriptor = m_screenColor->binding;
	bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
	m_postProgram.bindDescriptors(&RI.device, &RI.primary.cmds[0],
	                              RI.frameIndex, bindings, 2);

	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);

	// m_screenBgColor → SHADER_READ_ONLY so subsequent GUI draws can sample it.
	ImageBarrier(&RI.primary.cmds[0], &m_screenBgColor->handle,
			RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
			RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

	mbPending  = false;
	mbCaptured = true;
}

//-----------------------------------------------------------------------
