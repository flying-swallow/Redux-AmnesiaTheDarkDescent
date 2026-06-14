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

#include "LuxPostEffects.h"

#include "LuxMapHandler.h"

#include "graphics/Texture.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgramHelpers.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// INSANITY
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

namespace {
// Push constant block for posteffect_insanity.frag (matches the local
// InsanityPushConstants defined below at the call site).
struct InsanityPC { float time; float amplitude; float waveAlpha; float zoomAlpha; };

// Explicit per-backend binding tables for cLuxPostEffect_Insanity::m_program.
// Not bindless: every [vk::binding] is program-managed. The shared fullscreen
// vert (posteffect_fullscreen.vert) has no descriptors / push constant (vs
// empty).
//
// posteffect_insanity.frag.slang (set 0):
//   b0 SamplerState inputSampler, b1..b4 Texture2D sourceInput/ampMap0/ampMap1/
//   zoomMap, [vk::push_constant] InsanityPC pc.
// posteffect_insanity.frag.metal:
//   sourceInput [[texture(0)]], ampMap0 [[texture(1)]], inputSampler [[sampler(0)]],
//   ampMap1 [[texture(2)]], pc [[buffer(0)]], zoomMap [[texture(3)]].
constexpr hpl::RIProgram::RIProgramBinding kInsanity[] = {
	{"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
	{"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
	{"ampMap0", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {1}},
	{"ampMap1", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 3}, {}, {2}},
	{"zoomMap", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 4}, {}, {3}},
};
} // namespace

cLuxPostEffect_Insanity::cLuxPostEffect_Insanity(cGraphics *apGraphics, cResources *apResources) : iLuxPostEffect(apGraphics, apResources)
{
	//////////////////////////////
	// Create program — Slang port of dds_insanity_posteffect.frag.fsl, sharing the
	// fullscreen-triangle vertex shader with the other post-effects.
	hpl::LoadSlangGraphics(&RI.device, m_program, apResources,
	                       "posteffect_fullscreen.vert.spv",
	                       "posteffect_insanity.frag.spv",
	                       "vsMain", "psMain", {},
	                       kInsanity, sizeof(InsanityPC),
	                       RI_SHADER_STAGE_FRAGMENT);

	//////////////////////////////
	// Textures
	mvAmpMaps.resize(3);
	
	for(size_t i=0; i<mvAmpMaps.size(); ++i)
		mvAmpMaps[i] = mpResources->GetTextureManager()->Create2DImage("posteffect_insanity_ampmap"+cString::ToString((int)i), false);

	mpZoomMap = mpResources->GetTextureManager()->Create2DImage("posteffect_insanity_zoom.jpg", false);

	//////////////////////////////
	// Init vars
	mfT =0;
	mfAnimCount =0;
	mfWaveAlpha = 0.0f;
	mfZoomAlpha = 0.0f;
	mfWaveSpeed =0.0f;
}

//-----------------------------------------------------------------------

cLuxPostEffect_Insanity::~cLuxPostEffect_Insanity()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffect_Insanity::Update(float afTimeStep)
{
	mfT += afTimeStep * mfWaveSpeed;
	
	mfAnimCount += afTimeStep * 0.15f;

	float fMaxAnim = (float)mvAmpMaps.size();
	if(mfAnimCount >= fMaxAnim) mfAnimCount = mfAnimCount-fMaxAnim;
}

//-----------------------------------------------------------------------


namespace {
struct InsanityPushConstants
{
	float time;
	float amplitude;
	float waveAlpha;
	float zoomAlpha;
};
} // namespace

void cLuxPostEffect_Insanity::RenderEffect(const hpl::PostEffectRenderCtx &ctx)
{
	using namespace hpl;

	// Animated amp-map pair: ampMap0->1->2->0 as mfAnimCount sweeps [0,3), blended
	// by its fractional part (matches the legacy afAmpT animation).
	const int count = (int)mvAmpMaps.size();
	const int i0 = count > 0 ? ((int)mfAnimCount) % count : 0;
	const int i1 = count > 0 ? (i0 + 1) % count : 0;
	float amplitude = mfAnimCount - (float)((int)mfAnimCount); // frac; mfAnimCount >= 0

	// Resolve texture bindings. If any insanity asset is missing, substitute the
	// scene texture and zero the distortion so this pass stays a valid passthrough:
	// it MUST write its pogo half every frame, or the composite's post-effect toggle
	// desyncs and presents a stale, pre-tonemap buffer (the brightness bug).
	bool valid = (count >= 2);
	auto resolve = [&](Image *img) -> RIDescriptor {
		if (img && img->GetTexture() && img->GetTexture()->binding.texture)
			return img->GetTexture()->binding;
		valid = false;
		return *ctx.inputSrv;
	};
	RIDescriptor amp0Desc = (count > 0) ? resolve(mvAmpMaps[i0]) : *ctx.inputSrv;
	RIDescriptor amp1Desc = (count > 0) ? resolve(mvAmpMaps[i1]) : *ctx.inputSrv;
	RIDescriptor zoomDesc = resolve(mpZoomMap);
	if (count <= 0) valid = false;

	RIRenderingAttachment colorAttach = {};
	colorAttach.view    = ctx.outputView;
	colorAttach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

	RIBeginRenderingDesc renderInfo = {};
	renderInfo.renderArea.width  = (int16_t)ctx.width;
	renderInfo.renderArea.height = (int16_t)ctx.height;
	renderInfo.colorCount        = 1;
	renderInfo.colors            = &colorAttach;
	ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, renderInfo);
	ctx.cmd->mtl_encoderDraw(renderInfo);

	RIViewport viewport = {};
	viewport.width    = static_cast<float>(ctx.width);
	viewport.height   = static_cast<float>(ctx.height);
	viewport.depthMax = 1.0f;
	ctx.cmd->setViewport(&RI.renderer, viewport);
	RIRect scissor = {};
	scissor.width  = (int16_t)ctx.width;
	scissor.height = (int16_t)ctx.height;
	ctx.cmd->setScissor(&RI.renderer, scissor);

	RIGraphicsPipelineDesc state{};
	state.colorCount = 1;
	state.colors[0].format = RIBootstrap::PogoColorFormat; // blend disabled (default)

	const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
	m_program.bindPipeline(&RI.device, ctx.cmd, pipelineHash, "PostEffect_Insanity",
	                       state);

	auto samplerDesc = RI.resolve_filter_descriptor(
	    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
	    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	RIProgram::DescriptorBinding bindings[5] = {};
	bindings[0].descriptor = *samplerDesc;
	bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
	bindings[1].descriptor = *ctx.inputSrv;
	bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
	bindings[2].descriptor = amp0Desc;
	bindings[2].handle     = DescriptorBindingID::Create("ampMap0");
	bindings[3].descriptor = amp1Desc;
	bindings[3].handle     = DescriptorBindingID::Create("ampMap1");
	bindings[4].descriptor = zoomDesc;
	bindings[4].handle     = DescriptorBindingID::Create("zoomMap");
	m_program.bindDescriptors(&RI.device, ctx.cmd, ctx.frameIndex, bindings, 5);

	InsanityPushConstants pc{};
	pc.time      = mfT;
	pc.amplitude = amplitude;
	pc.waveAlpha = valid ? mfWaveAlpha : 0.0f;
	pc.zoomAlpha = valid ? mfZoomAlpha : 0.0f;
	m_program.pushConstants(ctx.cmd, &pc, sizeof(pc));

	ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
	ctx.cmd->mtl_encoderEnd();
	ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
}


//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// POST EFFECT HANDLER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffectHandler::cLuxPostEffectHandler() : iLuxUpdateable("LuxPostEffectHandler")
{
	cGraphics *pGraphics = gpBase->mpEngine->GetGraphics();
	cResources *pResources = gpBase->mpEngine->GetResources();

	///////////////////////
	// Create post effects
	mpInsanity = hplNew(cLuxPostEffect_Insanity, (pGraphics, pResources) );
	AddEffect(mpInsanity, 25);
	mpInsanity->SetActive(true);
}

//-----------------------------------------------------------------------

cLuxPostEffectHandler::~cLuxPostEffectHandler()
{
	STLDeleteAll(mvPostEffects);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::OnStart()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::Update(float afTimeStep)
{
	for(size_t i=0; i<mvPostEffects.size(); ++i)
	{
		iLuxPostEffect *pPostEffect = mvPostEffects[i];

        if(pPostEffect->IsActive()) pPostEffect->Update(afTimeStep);
	}
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::Reset()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::LoadMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	mpInsanity->SetDisabled(pMainCfg->GetBool("Graphics", "PostEffectInsanity", true)==false);

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::SaveMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	pMainCfg->SetBool("Graphics", "PostEffectInsanity", mpInsanity->IsDisabled()==false);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::AddEffect(iLuxPostEffect *apPostEffect, int alPrio)
{
	mvPostEffects.push_back(apPostEffect);
	apPostEffect->SetActive(false);
	gpBase->mpMapHandler->GetViewport()->GetPostEffectComposite()->AddPostEffect(apPostEffect, alPrio);
}

//-----------------------------------------------------------------------
