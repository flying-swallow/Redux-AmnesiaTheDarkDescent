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

#include "graphics/HPLTexture.h"
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

cLuxPostEffect_Insanity::cLuxPostEffect_Insanity(cGraphics *apGraphics, cResources *apResources) : iLuxPostEffect(apGraphics, apResources)
{
	//////////////////////////////
	// Create program — Slang port of dds_insanity_posteffect.frag.fsl, sharing the
	// fullscreen-triangle vertex shader with the other post-effects.
	hpl::LoadSlangGraphics(&RI.device, m_program, apResources,
	                       "posteffect_fullscreen.vert.spv",
	                       "posteffect_insanity.frag.spv");

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
	VkCommandBuffer cmd = ctx.cmd->vk.cmd;

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
	auto resolve = [&](Image *img) -> RIDescriptor_s {
		if (img && img->GetTexture() && img->GetTexture()->binding.texture)
			return img->GetTexture()->binding;
		valid = false;
		return *ctx.inputSrv;
	};
	RIDescriptor_s amp0Desc = (count > 0) ? resolve(mvAmpMaps[i0]) : *ctx.inputSrv;
	RIDescriptor_s amp1Desc = (count > 0) ? resolve(mvAmpMaps[i1]) : *ctx.inputSrv;
	RIDescriptor_s zoomDesc = resolve(mpZoomMap);
	if (count <= 0) valid = false;

	VkRenderingAttachmentInfo colorAttach = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	colorAttach.imageView   = ctx.outputView;
	colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
	renderInfo.renderArea           = {{0, 0}, {ctx.width, ctx.height}};
	renderInfo.layerCount           = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments    = &colorAttach;

	vkCmdBeginRendering(cmd, &renderInfo);

	VkViewport viewport = {0.0f, 0.0f, (float)ctx.width, (float)ctx.height, 0.0f, 1.0f};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = {{0, 0}, {ctx.width, ctx.height}};
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	PostEffectPipelineState state{};
	InitPostEffectPipelineState(state, RIBootstrap::PogoColorFormatVk, false);

	const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
	m_program.bindPipeline(&RI.device, ctx.cmd, pipelineHash, "PostEffect_Insanity",
	                       &state.createInfo);

	RIDescriptor_s *samplerDesc = RI.resolve_filter_descriptor(
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
	vkCmdPushConstants(cmd, m_program.getPipelineLayout(),
	                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
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
