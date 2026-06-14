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
// Explicit per-backend binding tables for cLuxScreenEffect::m_postProgram.
// The program is the shared fullscreen vert + ONE of two frags chosen by
// mEffect; both frags share the same set-0 layout (inputSampler b0, sourceInput
// b1). The blur variant additionally carries a fragment push constant.
// vk = {name, set, binding, RIDescriptorType_e, count}; mtl read off the
// generated entry-point signatures in build-mtl/amnesia/compiled_shaders.
// The fullscreen vert (posteffect_fullscreen.vert) has no descriptors / push
// constant (vs empty for both variants).
namespace {

// Push constant for the blur variant (matches BloomBlurPC / the local BlurPC).
struct ScreenBlurPushConstants { float blurDir[2]; float _pad[2]; };

// Shared fragment binding table for both variants (Metal: sourceInput
// [[texture(0)]], inputSampler [[sampler(0)]]; the blur variant's push constant
// is [[buffer(0)]]). The inventory variant has no push constant.
constexpr RIProgram::RIProgramBinding kPost[] = {
	{"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
	{"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

} // namespace

//-----------------------------------------------------------------------

// Lay down a single whole-image color barrier on the active command buffer.
static void ImageBarrier(RICmd *cmd, RITexture *texture,
		uint32_t before, uint32_t beforeStages,
		uint32_t after, uint32_t afterStages)
{
	RITextureBarrier barrier(texture, before, after, beforeStages, afterStages);
	barrier.mipCount   = 1;
	barrier.layerCount = 1;
	cmd->vk_d3d12_textureBarrier(barrier);
}

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
	const bool bBlur = (mEffect == Effect::Blur);
	const char *frag = bBlur
		? "posteffect_bloom_blur.frag.spv" : "inventory_post.frag.spv";

	// The blur variant carries a fragment push constant; inventory has none.
	const uint16_t pcSize = bBlur ? (uint16_t)sizeof(ScreenBlurPushConstants) : 0;
	const uint32_t pcStages = bBlur ? (uint32_t)RI_SHADER_STAGE_FRAGMENT : 0u;

	LoadSlangGraphics(&RI.device, m_postProgram, gpBase->mpEngine->GetResources(),
	                  vert, frag, "vsMain", "psMain", {},
	                  kPost, pcSize, pcStages);
}

//-----------------------------------------------------------------------

void cLuxScreenEffect::CreateTextures()
{
	const uint32_t w = RI.swapchain.width;
	const uint32_t h = RI.swapchain.height;

	LuxCreateScreenRenderTarget(m_screenBgColor, w, h, RI_FORMAT_RGBA8_UNORM,
	                            "screen.effectBg");

	// The separable blur ping-pongs through a scratch target between the shared
	// sharp copy and the blurred result.
	if (mEffect == Effect::Blur)
		LuxCreateScreenRenderTarget(m_screenScratch, w, h, RI_FORMAT_RGBA8_UNORM,
		                            "screen.effectBlurTmp");

	m_screenBgImage = std::make_shared<Image>(Image::SingleImage{m_screenBgColor});
	mpScreenBgGfx   = mpGui->CreateGfxTexture(m_screenBgImage.get(), false, eGuiMaterial_Alpha);
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

	// The shared_ptr deleter parks the GPU handles on the active frame slot's
	// freelist; they're released when that slot is reused next.
	m_screenBgImage.reset();
	m_screenBgColor.reset();
	m_screenScratch.reset();

	mbApplyPending = false;
	mbApplied      = false;
}

//-----------------------------------------------------------------------

void cLuxScreenEffect::OnPostRender()
{
	if (!mbApplyPending) {
		return;
	}
	if (!m_screenBgColor || !m_postProgram.isValid()) {
		return;
	}

	cLuxScreenCapture *pCapture = gpBase->GetScreenCapture();
	if (!pCapture->IsCaptured()) {
		return; // The shared capture has not landed yet; try again next frame.
	}
	RIDescriptor *pSource = pCapture->PrimaryDescriptor();
	if (pSource == nullptr) {
		return;
	}

	RICmd *cmd = &RI.primary.cmds[0];

	const cVector2l size = pCapture->GetSize();
	const uint32_t w = static_cast<uint32_t>(size.x);
	const uint32_t h = static_cast<uint32_t>(size.y);

	// Keep the effect target alive until the GPU has consumed it.
	auto *frameCtx = RI.GetActiveSet();
	frameCtx->resourceLink.push_back(m_screenBgColor);
	if (m_screenScratch) frameCtx->resourceLink.push_back(m_screenScratch);

	////////////////////////////////////////////////
	// Blurred backdrop (escape menu): separable gaussian ping-pong.
	// Reuses the bloom separable-blur frag; H then V between m_screenScratch
	// and m_screenBgColor, the first H reading the shared sharp copy.
	// After the loop the blurred result lives in m_screenBgColor.
	////////////////////////////////////////////////
	if (mEffect == Effect::Blur)
	{
		RIGraphicsPipelineDesc blurState{};
		blurState.colorCount = 1;
		blurState.colors[0].format = RI_FORMAT_RGBA8_UNORM; // blend disabled (default)
		const hash_t kBlurHash = hash_u32(HASH_INITIAL_VALUE, 0u);

		auto samplerDesc = RI.resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		assert(samplerDesc);

		struct BlurPC { float blurDir[2]; float _pad[2]; }; // matches BloomBlurPC

		RIViewport viewport = {};
		viewport.width = (float)w; viewport.height = (float)h; viewport.depthMax = 1.0f;
		RIRect scissor = {};
		scissor.width = (int16_t)w; scissor.height = (int16_t)h;

		auto blurPass = [&](cTexture *dst, const RIDescriptor &srcDesc,
		                    float dirX, float dirY)
		{
			// dst → color attachment (discard old contents; wait on any prior
			// sampling of dst — covers the WAR hazard on the ping-pong).
			ImageBarrier(&RI.primary.cmds[0], &dst->handle,
					RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
					RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

			RIRenderingAttachment attach = {};
			attach.view    = &dst->view;
			attach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
			attach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

			RIBeginRenderingDesc render = {};
			render.renderArea.width  = (int16_t)w;
			render.renderArea.height = (int16_t)h;
			render.colorCount        = 1;
			render.colors            = &attach;
			cmd->vk_d3d12_beginRendering(&RI.renderer, render);
			cmd->mtl_encoderDraw(render);

			cmd->setViewport(&RI.renderer, viewport);
			cmd->setScissor(&RI.renderer, scissor);

			m_postProgram.bindPipeline(&RI.device, cmd, kBlurHash,
			                           "screen.blur", blurState);

			RIProgram::DescriptorBinding bindings[2] = {};
			bindings[0].descriptor = *samplerDesc;
			bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
			bindings[1].descriptor = srcDesc;
			bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
			m_postProgram.bindDescriptors(&RI.device, cmd,
			                              RI.frameIndex, bindings, 2);

			BlurPC pc = { { dirX, dirY }, { 0.0f, 0.0f } };
			m_postProgram.pushConstants(cmd, &pc, sizeof(pc));

			cmd->draw(&RI.renderer, 3, 1, 0, 0);
			cmd->mtl_encoderEnd();
			cmd->vk_d3d12_endRendering(&RI.renderer);

			// dst → sampleable for the next pass / the GUI.
			ImageBarrier(&RI.primary.cmds[0], &dst->handle,
					RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
					RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
		};

		const float kBlurSize   = 2.0f; // texel multiplier per tap (tunable)
		const int   kIterations = 6;    // H+V pairs; more = heavier blur
		for (int i = 0; i < kIterations; ++i)
		{
			const RIDescriptor &hIn = (i == 0) ? *pSource
			                                     : m_screenBgColor->binding;
			blurPass(m_screenScratch.get(), hIn,                     kBlurSize, 0.0f);
			blurPass(m_screenBgColor.get(), m_screenScratch->binding, 0.0f, kBlurSize);
		}
		// m_screenBgColor ends SHADER_READ_ONLY (last blurPass).

		mbApplyPending = false;
		mbApplied      = true;
		return;
	}

	ImageBarrier(&RI.primary.cmds[0], &m_screenBgColor->handle,
			RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
			RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

	////////////////////////////////////////////////
	// Fullscreen post-effect — sample the shared capture, write m_screenBgColor.
	////////////////////////////////////////////////

	RIRenderingAttachment colorAttach = {};
	colorAttach.view    = &m_screenBgColor->view;
	colorAttach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

	RIBeginRenderingDesc renderInfo = {};
	renderInfo.renderArea.width  = (int16_t)w;
	renderInfo.renderArea.height = (int16_t)h;
	renderInfo.colorCount        = 1;
	renderInfo.colors            = &colorAttach;
	cmd->vk_d3d12_beginRendering(&RI.renderer, renderInfo);
	cmd->mtl_encoderDraw(renderInfo);

	RIViewport viewport = {};
	viewport.width = (float)w; viewport.height = (float)h; viewport.depthMax = 1.0f;
	cmd->setViewport(&RI.renderer, viewport);
	RIRect scissor = {};
	scissor.width = (int16_t)w; scissor.height = (int16_t)h;
	cmd->setScissor(&RI.renderer, scissor);

	// Fullscreen pass: no vertex input (the VS synthesises the triangle from the
	// vertex index), blend disabled, no depth, cull none — all RIGraphicsPipelineDesc
	// defaults. Only the color target format needs setting.
	RIGraphicsPipelineDesc postState{};
	postState.colorCount = 1;
	postState.colors[0].format = RI_FORMAT_RGBA8_UNORM;

	const hash_t pipelineHash =
		hash_u32(HASH_INITIAL_VALUE, (uint32_t)RI_FORMAT_RGBA8_UNORM);
	m_postProgram.bindPipeline(&RI.device, cmd, pipelineHash,
	                           "screen.post", postState);

	auto samplerDesc = RI.resolve_filter_descriptor(
		eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
	assert(samplerDesc);

	RIProgram::DescriptorBinding bindings[2] = {};
	bindings[0].descriptor = *samplerDesc;
	bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
	bindings[1].descriptor = *pSource;
	bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
	m_postProgram.bindDescriptors(&RI.device, cmd,
	                              RI.frameIndex, bindings, 2);

	cmd->draw(&RI.renderer, 3, 1, 0, 0);
	cmd->mtl_encoderEnd();
	cmd->vk_d3d12_endRendering(&RI.renderer);

	// m_screenBgColor → SHADER_READ_ONLY so subsequent GUI draws can sample it.
	ImageBarrier(&RI.primary.cmds[0], &m_screenBgColor->handle,
			RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
			RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

	mbApplyPending = false;
	mbApplied      = true;
}

//-----------------------------------------------------------------------
