/*
 * Copyright © 2009-2020 Frictional Games
 * Copyright 2026 Michael Pollind
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

#include "LuxEffectRenderer.h"

#include "graphics/Color.h"
#include "graphics/Texture.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIFormat.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"
#include "scene/Viewport.h"
#include "system/Hasher.h"

#include <algorithm>
#include <cstring>

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// LOCAL TYPES / HELPERS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

namespace {

// Per-frame UBO ("pass", set 0 binding 0) — view/viewProj for the geometry
// passes. Matches OutlinePass in outline_geom.vert.slang.
struct OutlinePassUBO {
	float viewProj[16];
	float view[16];
};

// Push constant — per-object model matrix + glow color. Matches OutlinePC
// (outline_geom).
struct OutlinePushConstants {
	float model[16];
	float color[4];
};

// Push constant for the alpha-tested outline variant — adds params.x (alpha
// texture single-channel flag). Matches OutlineAlphaPC (outline_alpha).
struct OutlineAlphaPushConstants {
	float model[16];
	float color[4];
	float params[4];
};

struct BlurPushConstants {
	float blurDir[2];
	float _pad[2];
};

//-----------------------------------------------------------------------
// Explicit per-backend binding tables for the five cLuxEffectRenderer programs.
// None of these are bindless, so every [vk::binding] is program-managed.
// vk = {name, set, binding, RIDescriptorType_e, count}; mtl = {name, kind,
// per-stage [[*(N)]] index} read off the generated entry-point signatures in
// build-mtl/amnesia/compiled_shaders/<basename>.{vert,frag}.metal.
//-----------------------------------------------------------------------

// Per-program unified binding tables. The push constant's size/visibility are
// passed at the call site (LoadGraphics below); its per-stage Metal [[buffer(N)]]
// slot is handed in separately because slangc assigns it different indices per
// stage (VS buffer1 when the `pass` UBO occupies buffer0, FS buffer0 when no
// buffer precedes it). The `pass` UBO is declared but unused in these fragment
// stages, so slangc strips it -> only the vertex `pass` survives.

// --- outline_geom (mGeomProgram): solid silhouette/rim. PC: VS+FS, both buffer1.
constexpr RIProgram::RIProgramBinding kGeom[] = {
	{"pass", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX, {0, 0}, {}, {0}},
};

// --- outline_alpha (mAlphaProgram): alpha-tested cutout. PC: VS buffer1, FS buffer1.
// (The hand-written .metal port keeps the FS sampler+map in set0's argument buffer at
// buffer0, so the FS push constant moves to buffer1 — unlike the old slangc loose path.)
constexpr RIProgram::RIProgramBinding kAlpha[] = {
	{"pass", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX, {0, 0}, {}, {0}},
	{"alphaSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
	{"alphaMap", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {0}},
};

// --- glow_object (mGlowProgram): diffuse-modulated fill. PC: VS buffer1, FS buffer1
// (FS sampler+map live in set0's argument buffer at buffer0; see outline_alpha note).
constexpr RIProgram::RIProgramBinding kGlow[] = {
	{"pass", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX, {0, 0}, {}, {0}},
	{"diffuseSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
	{"diffuseMap", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {0}},
};

// --- posteffect_bloom_blur (mBlurProgram): fullscreen vert + bloom blur. PC: FS buffer0.
constexpr RIProgram::RIProgramBinding kBlur[] = {
	{"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
	{"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

// --- outline_composite (mCompositeProgram): fullscreen vert + composite. No PC.
constexpr RIProgram::RIProgramBinding kComposite[] = {
	{"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
	{"blurInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

// Outline glow colour is authored in DISPLAY (sRGB) space and converted with
// hpl::sRGBToLinear before the composite (the pogo target is linear HDR).
const float kOutlineGlow[3] = {0.0f, 0.0f, 0.5f};
const float kOutlineScaleAdd = 0.02f;

// Flash / enemy glow TINTS — applied as LINEAR multipliers into the HDR scene,
// matching the original dds_flash / dds_enemy_glow fragment maths (which tint
// in-shader with no sRGB decode). The glow is modulated by the object's own
// diffuse texture and a view rim, so these are gentle bluish tints, not a flat
// fill. Enemy folds the original's extra 0.7 scalar into the tint.
const float kFlashGlow[3] = {0.5f, 0.5f, 1.0f};
const float kEnemyGlow[3] = {0.6f * 0.7f, 0.6f * 0.7f, 1.0f * 0.7f};

enum eGeomPassMode {
	eGeomPassMode_OutlineMark = 0, // stencil ALWAYS -> REPLACE 1, no color
	eGeomPassMode_OutlineRim,      // stencil NOT_EQUAL 1, write glow color
	eGeomPassMode_Glow,            // flash / enemy: diffuse-modulated interior
	                               // fill, additive RGB, cull BACK, pos/uv
};

void EmitImageBarrier(RICmd *apCmd, RITexture *apTex,
					  RIResourceState_e aBefore, uint32_t alBeforeStages,
					  RIResourceState_e aAfter, uint32_t alAfterStages) {
	RITextureBarrier barrier = {};
	barrier.texture = apTex;
	barrier.before = aBefore;
	barrier.beforeStages = alBeforeStages;
	barrier.after = aAfter;
	barrier.afterStages = alAfterStages;
	apCmd->vk_d3d12_textureBarrier(barrier);
}

// Build (and cache on the program) a geometry pipeline for the given pass.
// abUvLayout switches binding 1 from normal (float3) to texcoord (float2) —
// used by the alpha-tested cutout outline AND the Glow pass (both sample a UV
// instead of a normal). abNormalPresent only applies to the normal layout.
void BindGeomPipeline(RIProgram &aProgram, RICmd *apCmd, eGeomPassMode aMode,
					  bool abNormalPresent, bool abUvLayout, const char *asDebugName) {
	const bool bGlow = (aMode == eGeomPassMode_Glow);
	const bool bHasStencil = (aMode == eGeomPassMode_OutlineMark ||
							  aMode == eGeomPassMode_OutlineRim);

	RIGraphicsPipelineDesc desc{};
	desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
	// Cull BACK everywhere — the glow draws the visible FRONT faces so the mesh
	// interior fills with the additive glow (no silhouette-only border).
	desc.cullMode = RI_CULL_MODE_BACK;
	desc.frontCounterClockwise = false; // engine winding (clockwise)

	// binding 0: position (float3 in a float4 stride of 16). binding 1: texcoord
	// (float2) or normal (float3); stride 0 substitutes the fallback normal.
	desc.vertexStreamCount = 2;
	desc.vertexStreams[0] = {0, 16, false};
	desc.vertexStreams[1] = {1, abUvLayout ? 12u : (abNormalPresent ? 12u : 0u),
							 false};
	desc.vertexAttributeCount = 2;
	desc.vertexAttributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT, 0};
	desc.vertexAttributes[1] = {
		1, 1,
		abUvLayout ? (uint32_t)RI_FORMAT_RG32_SFLOAT
				   : (uint32_t)RI_FORMAT_RGB32_SFLOAT,
		0};

	// Read-only depth test against the scene depth (never writes).
	desc.depthTestEnable = true;
	desc.depthWriteEnable = false;
	desc.depthCompareOp = RI_COMPARE_LESS_EQUAL;
	desc.depthStencilFormat = RIBootstrap::DepthFormat;

	if (bHasStencil) {
		desc.stencilTestEnable = true;
		desc.stencilReference = 1;
		RIStencilOpDesc op{};
		op.compareMask = 0xFF;
		if (aMode == eGeomPassMode_OutlineMark) {
			op.failOp = RI_STENCIL_OP_KEEP;
			op.passOp = RI_STENCIL_OP_REPLACE;
			op.depthFailOp = RI_STENCIL_OP_KEEP;
			op.compareOp = RI_COMPARE_ALWAYS;
			op.writeMask = 0xFF;
		} else { // rim
			op.failOp = RI_STENCIL_OP_KEEP;
			op.passOp = RI_STENCIL_OP_KEEP;
			op.depthFailOp = RI_STENCIL_OP_KEEP;
			op.compareOp = RI_COMPARE_NOT_EQUAL;
			op.writeMask = 0x00;
		}
		desc.stencilFront = op;
		desc.stencilBack = op;
	}

	desc.colorCount = 1;
	RIColorAttachmentDesc &c = desc.colors[0];
	c.format = RIBootstrap::PogoColorFormat;
	if (bGlow) {
		// TRUE additive (ONE/ONE), RGB only — drawn into the linear-HDR
		// backbuffer before the post chain so bloom+tonemap compress it.
		c.blendEnabled = true;
		c.colorBlendOp = RI_BLEND_OP_ADD;
		c.alphaBlendOp = RI_BLEND_OP_ADD;
		c.srcColor = RI_BLEND_ONE;
		c.dstColor = RI_BLEND_ONE;
		c.srcAlpha = RI_BLEND_ONE;
		c.dstAlpha = RI_BLEND_ONE;
		c.writeMask = RI_COLOR_WRITE_RGB;
	} else if (aMode == eGeomPassMode_OutlineRim) {
		// Overwrite the glow color into the cleared offscreen target.
		c.blendEnabled = false;
		c.writeMask = RI_COLOR_WRITE_RGBA;
	} else { // mark — no color
		c.blendEnabled = false;
		c.writeMask = RI_COLOR_WRITE_NONE;
	}

	hash_t hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)aMode);
	hash = hash_u32(hash, abNormalPresent ? 1u : 0u);
	hash = hash_u32(hash, abUvLayout ? 1u : 0u);
	aProgram.bindPipeline(&RI.device, apCmd, hash, asDebugName, desc);
}

// Fullscreen-triangle pipeline (no vertex input, no depth, cull NONE). When
// abAdditive, SCREEN-blends (dst + src*(1-dst)) RGB-only into the attachment.
void BindFullscreenPipeline(RIProgram &aProgram, RICmd *apCmd, bool abAdditive,
							hash_t aSeed, const char *asDebugName) {
	RIGraphicsPipelineDesc desc{};
	desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
	desc.cullMode = RI_CULL_MODE_NONE;
	desc.colorCount = 1;
	RIColorAttachmentDesc &c = desc.colors[0];
	c.format = RIBootstrap::PogoColorFormat;
	if (abAdditive) {
		// SCREEN — dst + src*(1-dst) — brightens toward white but never past it
		// (composites the glow after the tone-map/post chain has run).
		c.blendEnabled = true;
		c.colorBlendOp = RI_BLEND_OP_ADD;
		c.alphaBlendOp = RI_BLEND_OP_ADD;
		c.srcColor = RI_BLEND_ONE_MINUS_DST_COLOR;
		c.dstColor = RI_BLEND_ONE;
		c.srcAlpha = RI_BLEND_ONE_MINUS_DST_COLOR;
		c.dstAlpha = RI_BLEND_ONE;
		c.writeMask = RI_COLOR_WRITE_RGB;
	} else {
		c.blendEnabled = false;
		c.writeMask = RI_COLOR_WRITE_RGBA;
	}
	aProgram.bindPipeline(&RI.device, apCmd, aSeed, asDebugName, desc);
}

// Bind a renderable's position + normal streams (bindings 0/1). Substitutes
// the global single-vertex fallback normal when the mesh lacks one; the caller
// zeroes that binding's stride in the pipeline (abNormalPresent=false).
bool BindGeomStreams(RICmd *apCmd, cVertexBuffer *apVB, bool *abNormalPresent) {
	auto *vbri = static_cast<cVertexBuffer *>(apVB);
	auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
		const auto *element = vbri->GetElement(type);
		return (element && element->buffer) ? element->buffer.get() : nullptr;
	};
	RIBuffer *pos = bufOf(eVertexBufferElement_Position);
	const auto &idxRI = vbri->GetIndexRIBuffer();
	RIBuffer *idx = idxRI ? idxRI.get() : nullptr;
	if (!pos || !idx)
		return false;
	RIBuffer *nrm = bufOf(eVertexBufferElement_Normal);
	if (abNormalPresent)
		*abNormalPresent = (nrm != nullptr);
	RIBuffer *vertBufs[2] = {pos, nrm ? nrm : &RI.fallbackNormalVertex};
	apCmd->bindVertexBuffers<2>(0, 2, vertBufs);
	apCmd->bindIndexBuffer(&RI.renderer, idx, 0, RI_INDEX_TYPE_32);
	return true;
}

// Bind position (binding 0) + texcoord (binding 1) for the alpha-tested
// cutout outline. Returns false if the mesh has no UVs (caller falls back to
// the solid path).
bool BindGeomStreamsUv(RICmd *apCmd, cVertexBuffer *apVB) {
	auto *vbri = static_cast<cVertexBuffer *>(apVB);
	auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
		const auto *element = vbri->GetElement(type);
		return (element && element->buffer) ? element->buffer.get() : nullptr;
	};
	RIBuffer *pos = bufOf(eVertexBufferElement_Position);
	RIBuffer *uv = bufOf(eVertexBufferElement_Texture0);
	const auto &idxRI = vbri->GetIndexRIBuffer();
	RIBuffer *idx = idxRI ? idxRI.get() : nullptr;
	if (!pos || !uv || !idx)
		return false;
	RIBuffer *vertBufs[2] = {pos, uv};
	apCmd->bindVertexBuffers<2>(0, 2, vertBufs);
	apCmd->bindIndexBuffer(&RI.renderer, idx, 0, RI_INDEX_TYPE_32);
	return true;
}

} // namespace

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxEffectRenderer::cLuxEffectRenderer() : iLuxUpdateable("LuxEffectRenderer")
{
	mpViewport = NULL;
	mbProgramsLoaded = false;
	mbOutlineColorInit = false;
	mbBlurInit = false;

	mFlashOscill.SetUp(0, 1, 0, 1, 1);

	Reset();
}

//-----------------------------------------------------------------------

cLuxEffectRenderer::~cLuxEffectRenderer()
{
	DestroyPostEffectColorTarget(m_outlineColor);
	DestroyPostEffectColorTarget(m_blur[0]);
	DestroyPostEffectColorTarget(m_blur[1]);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxEffectRenderer::Reset()
{
	mvOutlineObjects.clear();
	ClearRenderLists();
}

void cLuxEffectRenderer::ClearRenderLists()
{
	mvFlashObjects.clear();
	mvEnemyGlowObjects.clear();
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::Update(float afTimeStep)
{
	mFlashOscill.Update(afTimeStep);
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::AttachViewport(cViewport* apViewport)
{
	mpViewport = apViewport;
	mTranslucenceDrawHandler = EventHandler<const PostTranslucenceDrawCtx&>(
		[this](const PostTranslucenceDrawCtx& ctx){ OnPostTranslucenceDraw(ctx); });
	mPostWorldDrawHandler = EventHandler<const PostWorldDrawCtx&>(
		[this](const PostWorldDrawCtx& ctx){ OnPostWorldDraw(ctx); });
	if (mpViewport) {
		mTranslucenceDrawHandler.Connect(mpViewport->OnPostTranslucenceDraw());
		mPostWorldDrawHandler.Connect(mpViewport->OnPostWorldDraw());
	}
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::AddOutlineObject(iRenderable *apObject)
{
	mvOutlineObjects.push_back(apObject);
}

void cLuxEffectRenderer::ClearOutlineObjects()
{
	mvOutlineObjects.clear();
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::AddFlashObject(iRenderable *apObject, float afAlpha)
{
	mvFlashObjects.push_back(cGlowObject(apObject, afAlpha) );
}

void cLuxEffectRenderer::AddEnemyGlow(iRenderable *apObject, float afAlpha)
{
	mvEnemyGlowObjects.push_back(cGlowObject(apObject, afAlpha) );
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxEffectRenderer::EnsurePrograms()
{
	if (mbProgramsLoaded) return;
	cResources *pResources = gpBase->mpEngine->GetResources();

	// outline_alpha / glow_object read the push constant from buffer1 in both
	// stages: the VS `pass` UBO occupies buffer0, and the FS sampler+texture live
	// in set0's argument buffer at buffer0 (hand-written .metal port) — so the FS
	// push constant also lands at buffer1. A per-stage Metal slot the
	// LoadSlangGraphics helper can't express, so build the descriptors directly here.
	auto LoadGraphics = [&](RIProgram &prog, const char *vs, const char *fs,
	                        std::span<const RIProgram::RIProgramBinding> bindings,
	                        uint16_t pcSize, uint32_t pcStages,
	                        uint16_t pcMtlVert, uint16_t pcMtlFrag) {
		auto vsBin = RIProgram::loadShaderStage(pResources->GetFileSearcher(), vs);
		auto fsBin = RIProgram::loadShaderStage(pResources->GetFileSearcher(), fs);
		std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vsBin, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fsBin, "psMain"}};
		stages[0].pushConstantMtlIndex = pcMtlVert;
		stages[1].pushConstantMtlIndex = pcMtlFrag;
		RIProgram::RIProgramDescriptor desc = {};
		desc.stages = stages;
		desc.bindings = bindings;
		desc.pushConstantSize = pcSize;
		desc.pushConstantStages = pcStages;
		prog.initialize(&RI.device, desc);
	};
	constexpr uint32_t kVF = RI_SHADER_STAGE_VERTEX | RI_SHADER_STAGE_FRAGMENT;
	LoadGraphics(mGeomProgram, "outline_geom.vert.spv", "outline_geom.frag.spv",
	             kGeom, sizeof(OutlinePushConstants), kVF, 1, 1);
	LoadGraphics(mAlphaProgram, "outline_alpha.vert.spv", "outline_alpha.frag.spv",
	             kAlpha, sizeof(OutlineAlphaPushConstants), kVF, 1, 1);
	LoadGraphics(mGlowProgram, "glow_object.vert.spv", "glow_object.frag.spv",
	             kGlow, sizeof(OutlineAlphaPushConstants), kVF, 1, 1);
	LoadGraphics(mBlurProgram, "posteffect_fullscreen.vert.spv",
	             "posteffect_bloom_blur.frag.spv", kBlur,
	             sizeof(BlurPushConstants), RI_SHADER_STAGE_FRAGMENT, 0, 0);
	LoadGraphics(mCompositeProgram, "posteffect_fullscreen.vert.spv",
	             "outline_composite.frag.spv", kComposite, 0, 0, 0, 0);
	mbProgramsLoaded = true;
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::EnsureTargets(uint32_t alWidth, uint32_t alHeight)
{
	if (!m_outlineColor.valid || m_outlineColor.width != alWidth ||
		m_outlineColor.height != alHeight) {
		DestroyPostEffectColorTarget(m_outlineColor);
		CreatePostEffectColorTarget(m_outlineColor, alWidth, alHeight,
									RIBootstrap::PogoColorFormat, 0u,
									"LuxOutline.color");
		mbOutlineColorInit = true;
	}

	const uint32_t blurW = std::max(alWidth / 4u, 1u);
	const uint32_t blurH = std::max(alHeight / 4u, 1u);
	if (!m_blur[0].valid || m_blur[0].width != blurW ||
		m_blur[0].height != blurH) {
		DestroyPostEffectColorTarget(m_blur[0]);
		DestroyPostEffectColorTarget(m_blur[1]);
		CreatePostEffectColorTarget(m_blur[0], blurW, blurH,
									RIBootstrap::PogoColorFormat, 0u,
									"LuxOutline.blur0");
		CreatePostEffectColorTarget(m_blur[1], blurW, blurH,
									RIBootstrap::PogoColorFormat, 0u,
									"LuxOutline.blur1");
		mbBlurInit = true;
	}
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::OnPostWorldDraw(const PostWorldDrawCtx &ctx)
{
	// Only the hover outline composites over the FINAL tone-mapped image here;
	// flash + enemy glow draw additively pre-tonemap (OnPostTranslucenceDraw).
	if (mvOutlineObjects.empty()) return;

	// Everything per-viewport / per-frame arrives through the context — no
	// captured mpViewport, no reach into the RI global for the command buffer.
	RI_PogoBuffer *pPogo = ctx.pogo;
	if (pPogo == NULL) return;

	cFrustum *pFrustum = ctx.frustum;
	if (pFrustum == NULL) return;

	RITextureView *pDepthView = ctx.depthView;
	if (pDepthView == NULL) return;

	const uint32_t w = ctx.width;
	const uint32_t h = ctx.height;
	if (w == 0 || h == 0) return;

	EnsurePrograms();
	EnsureTargets(w, h);

	RICmd *pCmd = ctx.cmd;

	/////////////////////////////
	// Per-frame view/viewProj UBO
	OutlinePassUBO ubo = {};
	{
		const ml::float4x4 viewProj =
			pFrustum->GetProjectionMat() * pFrustum->GetViewMat();
		const ml::float4x4 view = pFrustum->GetViewMat();
		std::memcpy(ubo.viewProj, viewProj.a, sizeof(ubo.viewProj));
		std::memcpy(ubo.view, view.a, sizeof(ubo.view));
	}
	RIDescriptor passDesc = {};
	RI.UpdateFrameUBO(&passDesc, (void *)&ubo, sizeof(ubo));

	/////////////////////////////
	// Outline silhouette + blur into the offscreen scratch (untouched pogo)
	RenderOutline(ctx, passDesc);

	/////////////////////////////
	// Composite the blurred outline into the pogo read half (post-tonemap).
	const uint32_t readIdx = (pPogo->attachmentIndex + 1u) % 2u;
	RITexture *pReadTex = &pPogo->textures[readIdx];
	RITextureView *pReadView = &pPogo->views[readIdx];

	// Read half: SHADER_RESOURCE -> RENDER_TARGET (composite appends).
	pCmd->vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(pReadTex, /*initial=*/false));

	RIRect scissor = {};
	scissor.width = (int16_t)w; scissor.height = (int16_t)h;

	// (a) Composite the blurred outline — fullscreen, no depth.
	{
		RIRenderingAttachment colorAttach = {};
		colorAttach.view = pReadView;
		colorAttach.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		RIBeginRenderingDesc render = {};
		render.renderArea.width = (int16_t)w;
		render.renderArea.height = (int16_t)h;
		render.colorCount = 1;
		render.colors = &colorAttach;
		pCmd->vk_d3d12_beginRendering(&RI.renderer, render);
		pCmd->mtl_encoderDraw(render);

		RIViewport fsViewport = {};
		fsViewport.width = (float)w; fsViewport.height = (float)h;
		fsViewport.depthMax = 1.0f;
		pCmd->setViewport(&RI.renderer, fsViewport);
		pCmd->setScissor(&RI.renderer, scissor);

		BindFullscreenPipeline(mCompositeProgram, pCmd, /*additive=*/true,
							   hash_u32(HASH_INITIAL_VALUE, 0u),
							   "LuxOutline.composite");
		auto pSampler = RI.resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *pSampler;
		bindings[0].handle = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = m_blur[1].descriptor;
		bindings[1].handle = DescriptorBindingID::Create("blurInput");
		mCompositeProgram.bindDescriptors(ctx.device, pCmd, ctx.frameIndex, bindings, 2);
		pCmd->draw(&RI.renderer, 3, 1, 0, 0);

		pCmd->mtl_encoderEnd();
		pCmd->vk_d3d12_endRendering(&RI.renderer);

		// Restore blur[1]'s rest state (RENDER_TARGET) for the next frame's
		// ping-pong — matches the Bloom blur convention.
		EmitImageBarrier(pCmd, &m_blur[1].texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
						 RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
	}

	// Read half back to SHADER_RESOURCE for delivery.
	pCmd->vk_d3d12_textureBarrier(RI_PogoShaderBarrier(pReadTex, /*initial=*/false));
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::OnPostTranslucenceDraw(const PostTranslucenceDrawCtx &ctx)
{
	const bool bHasFlash = !mvFlashObjects.empty();
	const bool bHasEnemy = !mvEnemyGlowObjects.empty();
	if (!bHasFlash && !bHasEnemy) return;

	cFrustum *pFrustum = ctx.frustum;
	if (pFrustum == NULL || ctx.viewport == NULL) return;

	RITextureView *pDepthView = ctx.depthView;
	if (pDepthView == NULL) return;

	const uint32_t w = ctx.width;
	const uint32_t h = ctx.height;
	if (w == 0 || h == 0) return;

	EnsurePrograms();

	RICmd *pCmd = ctx.cmd;

	// The renderer left the linear-HDR scene in the viewport BackBuffer. NOTE:
	// valid only while the hybrid guard band is disabled (kGuardBandFraction ==
	// 0), where the overscan extent equals the target size — so the plain
	// frustum projection + a target-sized full-image viewport line up exactly.
	// If the guard band is ever enabled, this pass must switch to the
	// renderer's widened projection and the full overscan extent.
	cViewport::BackBuffer bb = ctx.viewport->GetBackBuffer();
	if (bb.colorView == nullptr) return;

	/////////////////////////////
	// Per-frame view/viewProj UBO (HDR-space, plain frustum)
	OutlinePassUBO ubo = {};
	{
		const ml::float4x4 viewProj =
			pFrustum->GetProjectionMat() * pFrustum->GetViewMat();
		const ml::float4x4 view = pFrustum->GetViewMat();
		std::memcpy(ubo.viewProj, viewProj.a, sizeof(ubo.viewProj));
		std::memcpy(ubo.view, view.a, sizeof(ubo.view));
	}
	RIDescriptor passDesc = {};
	RI.UpdateFrameUBO(&passDesc, (void *)&ubo, sizeof(ubo));

	// Borrow the BackBuffer: SHADER_RESOURCE -> RENDER_TARGET for the additive
	// draws, then back to SHADER_RESOURCE so the feed blit finds it as the
	// renderer left it.
	EmitImageBarrier(pCmd, &bb.renderTarget, RI_RESOURCE_STATE_SHADER_RESOURCE,
					 RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

	RIViewport flippedViewport = {};
	flippedViewport.y = (float)h; flippedViewport.width = (float)w;
	flippedViewport.height = -(float)h; flippedViewport.depthMax = 1.0f;
	RIRect scissor = {};
	scissor.width = (int16_t)w; scissor.height = (int16_t)h;

	{
		RIRenderingAttachment colorAttach = {};
		colorAttach.view = bb.colorView;
		colorAttach.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		// Scene depth, read-only test (left in a depth-attachment layout by Draw).
		RIRenderingAttachment depthAttach = {};
		depthAttach.view = pDepthView;
		depthAttach.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		depthAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		RIBeginRenderingDesc render = {};
		render.renderArea.width = (int16_t)w;
		render.renderArea.height = (int16_t)h;
		render.colorCount = 1;
		render.colors = &colorAttach;
		render.depthStencil = &depthAttach;
		pCmd->vk_d3d12_beginRendering(&RI.renderer, render);
		pCmd->mtl_encoderDraw(render);

		pCmd->setViewport(&RI.renderer, flippedViewport);
		pCmd->setScissor(&RI.renderer, scissor);

		auto pDiffSampler = RI.resolve_filter_descriptor(
			eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
			eTextureFilter_Bilinear);

		// Port of dds_flash / dds_enemy_glow: an additive glow that FILLS the
		// object's visible surface, modulated by its own diffuse texture and a
		// bluish tint. The tint is a LINEAR multiplier into the HDR scene (no sRGB
		// decode — matches the original in-shader maths). Flash alpha-tests and
		// draws twice; enemy draws once. Both pulse with the shared oscillation.
		auto drawObjects = [&](std::vector<cGlowObject> &avObjects, const float aTint[3],
							   float afGlobalAlpha, bool abAlphaTest, int alDrawCount) {
			for (size_t i = 0; i < avObjects.size(); ++i) {
				iRenderable *pObject = avObjects[i].mpObject;
				if (pObject == NULL) continue;
				if (pObject->CollidesWithFrustum(pFrustum) == false) continue;
				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if (pVB == NULL) continue;

				// The glow samples the object's diffuse texture, so it needs both
				// UVs and a diffuse image; skip the object if either is missing.
				cMaterial *pMat = pObject->GetMaterial();
				Image *pDiffImage = pMat ? pMat->GetImage(eMaterialTexture_Diffuse) : NULL;
				std::shared_ptr<cTexture> diffTex =
					pDiffImage ? pDiffImage->GetTexture() : nullptr;
				if (!diffTex) continue;

				if (!BindGeomStreamsUv(pCmd, pVB)) continue;
				BindGeomPipeline(mGlowProgram, pCmd, eGeomPassMode_Glow,
								 /*normalPresent=*/false, /*uvLayout=*/true, "LuxEffect.glow");

				OutlineAlphaPushConstants pc = {};
				cMatrixf *pMtx = pObject->GetModelMatrix(pFrustum);
				const ml::float4x4 model =
					cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
				std::memcpy(pc.model, model.a, sizeof(pc.model));
				pc.color[0] = hpl::sRGBToLinear(aTint[0]);
				pc.color[1] = hpl::sRGBToLinear(aTint[1]);
				pc.color[2] = hpl::sRGBToLinear(aTint[2]);
				pc.color[3] = avObjects[i].mfAlpha * afGlobalAlpha;
				pc.params[0] = abAlphaTest ? 1.0f : 0.0f;
				pc.params[1] =
					(RIFormatChannelCount(diffTex->format) == 1) ? 1.0f : 0.0f;
				mGlowProgram.pushConstants(pCmd, &pc, sizeof(pc));

				RIProgram::DescriptorBinding bindings[3] = {};
				bindings[0].descriptor = passDesc;
				bindings[0].handle = DescriptorBindingID::Create("pass");
				bindings[1].descriptor = *pDiffSampler;
				bindings[1].handle = DescriptorBindingID::Create("diffuseSampler");
				bindings[2].descriptor = diffTex->binding;
				bindings[2].handle = DescriptorBindingID::Create("diffuseMap");
				mGlowProgram.bindDescriptors(ctx.device, pCmd, ctx.frameIndex, bindings, 3);

				for (int d = 0; d < alDrawCount; ++d)
					pCmd->drawIndexed(&RI.renderer, (uint32_t)pVB->GetIndexNum(), 1, 0, 0, 0);
			}
		};

		// Shared pulse (0.5..1.0), matching the original's single fGlobalAlpha.
		const float fGlobalAlpha = (0.5f + mFlashOscill.val * 0.5f) * 0.1;
		if (bHasFlash)
			drawObjects(mvFlashObjects, kFlashGlow, fGlobalAlpha,
						/*alphaTest=*/true, /*drawCount=*/2);
		if (bHasEnemy)
			drawObjects(mvEnemyGlowObjects, kEnemyGlow, fGlobalAlpha,
						/*alphaTest=*/false, /*drawCount=*/1);

		pCmd->mtl_encoderEnd();
		pCmd->vk_d3d12_endRendering(&RI.renderer);
	}

	// Return the BackBuffer to SHADER_RESOURCE for the feed blit.
	EmitImageBarrier(pCmd, &bb.renderTarget, RI_RESOURCE_STATE_RENDER_TARGET,
					 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::RenderOutline(const PostWorldDrawCtx &ctx,
									   const RIDescriptor &aPassDesc)
{
	RICmd *apCmd = ctx.cmd;
	cFrustum *apFrustum = ctx.frustum;
	const uint32_t alWidth = ctx.width;
	const uint32_t alHeight = ctx.height;

	// Depth view comes from the context; the depth *texture* (for the stencil-
	// aspect barrier) is reached through the context's viewport — both belong to
	// the firing viewport, delivered rather than captured.
	RITexture *pDepthTex = ctx.viewport ? ctx.viewport->GetDepthTexture() : NULL;
	RITextureView *pDepthView = ctx.depthView;
	if (pDepthTex == NULL || pDepthView == NULL) return;

	// Offscreen color: (UNDEFINED on first use) -> RENDER_TARGET.
	EmitImageBarrier(apCmd, &m_outlineColor.texture,
					 mbOutlineColorInit ? RI_RESOURCE_STATE_UNDEFINED
										: RI_RESOURCE_STATE_SHADER_RESOURCE,
					 mbOutlineColorInit ? RI_STAGE_NONE : RI_STAGE_FRAGMENT,
					 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
	mbOutlineColorInit = false;

	// Stencil aspect UNDEFINED -> STENCIL_ATTACHMENT_OPTIMAL (the depth aspect
	// is already DEPTH_ATTACHMENT_OPTIMAL, left by HybridRenderer::Draw). Raw,
	// Vulkan-only barrier — the RI helper can't address a single aspect to an
	// attachment layout, and Metal has no explicit image layouts (no-op there).
#if (DEVICE_IMPL_VULKAN)
	{
		VkImageMemoryBarrier2 sb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		sb.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		sb.srcAccessMask = 0;
		sb.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
						  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		sb.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
						   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		sb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		sb.newLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
		sb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		sb.image = pDepthTex->vk.image;
		sb.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
		VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers = &sb;
		vkCmdPipelineBarrier2(apCmd->vk.cmd, &dep);
	}
#endif

	// Cull list once.
	std::vector<iRenderable *> lstObjects;
	lstObjects.reserve(mvOutlineObjects.size());
	for (size_t i = 0; i < mvOutlineObjects.size(); ++i) {
		iRenderable *pObject = mvOutlineObjects[i];
		if (pObject && pObject->CollidesWithFrustum(apFrustum))
			lstObjects.push_back(pObject);
	}

	{
		RIRenderingAttachment colorAttach = {};
		colorAttach.view = &m_outlineColor.view;
		colorAttach.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		// clearValue.color defaults to {0,0,0,0}.

		// Combined depth+stencil view: load/keep depth (read-only test), clear
		// stencil. hasStencil binds the stencil aspect with its own load/store.
		RIRenderingAttachment depthStencil = {};
		depthStencil.view = pDepthView;
		depthStencil.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		depthStencil.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		depthStencil.hasStencil = true;
		depthStencil.stencilLoadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
		depthStencil.stencilStoreOp = RI_ATTACHMENT_STORE_OP_DONT_CARE;
		depthStencil.clearValue.stencil = 0;

		RIBeginRenderingDesc render = {};
		render.renderArea.width = (int16_t)alWidth;
		render.renderArea.height = (int16_t)alHeight;
		render.colorCount = 1;
		render.colors = &colorAttach;
		render.depthStencil = &depthStencil;
		apCmd->vk_d3d12_beginRendering(&RI.renderer, render);
		apCmd->mtl_encoderDraw(render);

		RIViewport flippedViewport = {};
		flippedViewport.y = (float)alHeight; flippedViewport.width = (float)alWidth;
		flippedViewport.height = -(float)alHeight; flippedViewport.depthMax = 1.0f;
		RIRect scissor = {};
		scissor.width = (int16_t)alWidth; scissor.height = (int16_t)alHeight;
		apCmd->setViewport(&RI.renderer, flippedViewport);
		apCmd->setScissor(&RI.renderer, scissor);

		// Two passes: (1) mark stencil from the original silhouette, (2) draw
		// the glow color from the scaled-out mesh where stencil != 1 (the rim).
		for (int pass = 0; pass < 2; ++pass) {
			const eGeomPassMode mode =
				(pass == 0) ? eGeomPassMode_OutlineMark : eGeomPassMode_OutlineRim;
			for (size_t i = 0; i < lstObjects.size(); ++i) {
				iRenderable *pObject = lstObjects[i];
				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if (pVB == NULL) continue;

				// Model matrix; pass 1 scales the mesh out ~2% about its local
				// centre so the rim sits just outside the silhouette.
				cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
				cMatrixf mtxWorld = pMtx ? *pMtx : cMatrixf::Identity;
				if (pass == 1) {
					cBoundingVolume *pBV = pObject->GetBoundingVolume();
					cVector3f vLocalSize = pBV->GetLocalMax() - pBV->GetLocalMin();
					cVector3f vScale = (cVector3f(1.0f) / vLocalSize) * kOutlineScaleAdd +
									   cVector3f(1.0f);
					cMatrixf mtxScale = cMath::MatrixMul(
						cMath::MatrixScale(vScale),
						cMath::MatrixTranslate(pBV->GetLocalCenter() * -1));
					mtxScale.SetTranslation(mtxScale.GetTranslation() +
											pBV->GetLocalCenter());
					mtxWorld = cMath::MatrixMul(mtxWorld, mtxScale);
				}
				const ml::float4x4 model = cMath::ToFloatTranspose4x4(mtxWorld);

				// Alpha-tested (cutout) materials: sample the Alpha texture and
				// discard so the mark + rim follow the visible silhouette. Falls
				// back to the solid path if the mesh has no UVs.
				cMaterial *pMat = pObject->GetMaterial();
				Image *pAlphaImage = pMat ? pMat->GetImage(eMaterialTexture_Alpha) : NULL;
				std::shared_ptr<cTexture> alphaTex =
					pAlphaImage ? pAlphaImage->GetTexture() : nullptr;

				bool bDrewAlpha = false;
				if (alphaTex && BindGeomStreamsUv(apCmd, pVB)) {
					BindGeomPipeline(mAlphaProgram, apCmd, mode, /*normalPresent=*/false,
									 /*uvLayout=*/true, "LuxOutline.alpha");

					OutlineAlphaPushConstants pc = {};
					std::memcpy(pc.model, model.a, sizeof(pc.model));
					pc.color[0] = sRGBToLinear(kOutlineGlow[0]);
					pc.color[1] = sRGBToLinear(kOutlineGlow[1]);
					pc.color[2] = sRGBToLinear(kOutlineGlow[2]);
					pc.color[3] = 1.0f;
					pc.params[0] =
						(RIFormatChannelCount(alphaTex->format) == 1) ? 1.0f : 0.0f;
					mAlphaProgram.pushConstants(apCmd, &pc, sizeof(pc));

					auto pSampler = RI.resolve_filter_descriptor(
						eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
						eTextureFilter_Bilinear);
					RIProgram::DescriptorBinding bindings[3] = {};
					bindings[0].descriptor = aPassDesc;
					bindings[0].handle = DescriptorBindingID::Create("pass");
					bindings[1].descriptor = *pSampler;
					bindings[1].handle = DescriptorBindingID::Create("alphaSampler");
					bindings[2].descriptor = alphaTex->binding;
					bindings[2].handle = DescriptorBindingID::Create("alphaMap");
					mAlphaProgram.bindDescriptors(ctx.device, apCmd, ctx.frameIndex, bindings, 3);

					apCmd->drawIndexed(&RI.renderer, (uint32_t)pVB->GetIndexNum(), 1, 0, 0, 0);
					bDrewAlpha = true;
				}
				if (bDrewAlpha) continue;

				// Solid path.
				bool bNormalPresent = false;
				if (!BindGeomStreams(apCmd, pVB, &bNormalPresent)) continue;
				BindGeomPipeline(mGeomProgram, apCmd, mode, bNormalPresent,
								 /*uvLayout=*/false, "LuxOutline.geom");

				OutlinePushConstants pc = {};
				std::memcpy(pc.model, model.a, sizeof(pc.model));
				pc.color[0] = sRGBToLinear(kOutlineGlow[0]);
				pc.color[1] = sRGBToLinear(kOutlineGlow[1]);
				pc.color[2] = sRGBToLinear(kOutlineGlow[2]);
				pc.color[3] = 1.0f;
				mGeomProgram.pushConstants(apCmd, &pc, sizeof(pc));

				RIProgram::DescriptorBinding b = {};
				b.descriptor = aPassDesc;
				b.handle = DescriptorBindingID::Create("pass");
				mGeomProgram.bindDescriptors(ctx.device, apCmd, ctx.frameIndex, &b, 1);

				apCmd->drawIndexed(&RI.renderer, (uint32_t)pVB->GetIndexNum(), 1, 0, 0, 0);
			}
		}

		apCmd->mtl_encoderEnd();
		apCmd->vk_d3d12_endRendering(&RI.renderer);
	}

	// Offscreen color -> SHADER_RESOURCE for the blur to sample.
	EmitImageBarrier(apCmd, &m_outlineColor.texture, RI_RESOURCE_STATE_RENDER_TARGET,
					 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

	BlurOutline(apCmd, m_blur[0].width, m_blur[0].height);
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::BlurOutline(RICmd *apCmd, uint32_t alBlurW, uint32_t alBlurH)
{
	// First-use rest states (Bloom pattern): blur[0] SHADER_RESOURCE, blur[1]
	// RENDER_TARGET.
	if (mbBlurInit) {
		EmitImageBarrier(apCmd, &m_blur[0].texture, RI_RESOURCE_STATE_UNDEFINED,
						 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
		EmitImageBarrier(apCmd, &m_blur[1].texture, RI_RESOURCE_STATE_UNDEFINED,
						 RI_STAGE_NONE, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
		mbBlurInit = false;
	}

	auto pSampler = RI.resolve_filter_descriptor(
		eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	RIViewport viewport = {};
	viewport.width = (float)alBlurW; viewport.height = (float)alBlurH;
	viewport.depthMax = 1.0f;
	RIRect scissor = {};
	scissor.width = (int16_t)alBlurW; scissor.height = (int16_t)alBlurH;

	auto blurPass = [&](RITextureView *destView, RITexture *destTexture,
						RITexture *prevDestTexture, const RIDescriptor &inputDesc,
						float dirX, float dirY) {
		EmitImageBarrier(apCmd, destTexture, RI_RESOURCE_STATE_SHADER_RESOURCE,
						 RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
		EmitImageBarrier(apCmd, prevDestTexture, RI_RESOURCE_STATE_RENDER_TARGET,
						 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

		RIRenderingAttachment attach = {};
		attach.view = destView;
		attach.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
		attach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		RIBeginRenderingDesc render = {};
		render.renderArea.width = (int16_t)alBlurW;
		render.renderArea.height = (int16_t)alBlurH;
		render.colorCount = 1;
		render.colors = &attach;
		apCmd->vk_d3d12_beginRendering(&RI.renderer, render);
		apCmd->mtl_encoderDraw(render);

		apCmd->setViewport(&RI.renderer, viewport);
		apCmd->setScissor(&RI.renderer, scissor);

		BindFullscreenPipeline(mBlurProgram, apCmd, /*additive=*/false,
							   hash_u32(HASH_INITIAL_VALUE, 1u), "LuxOutline.blur");
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *pSampler;
		bindings[0].handle = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = inputDesc;
		bindings[1].handle = DescriptorBindingID::Create("sourceInput");
		mBlurProgram.bindDescriptors(&RI.device, apCmd, RI.frameIndex, bindings, 2);

		BlurPushConstants pc = {};
		pc.blurDir[0] = dirX;
		pc.blurDir[1] = dirY;
		mBlurProgram.pushConstants(apCmd, &pc, sizeof(pc));
		apCmd->draw(&RI.renderer, 3, 1, 0, 0);
		apCmd->mtl_encoderEnd();
		apCmd->vk_d3d12_endRendering(&RI.renderer);
	};

	const float fBlurSize = 1.0f;
	for (int iter = 0; iter < 2; ++iter) {
		const RIDescriptor &firstInput =
			(iter == 0) ? m_outlineColor.descriptor : m_blur[1].descriptor;
		// H: dest blur[0], read firstInput, prevDest blur[1].
		blurPass(&m_blur[0].view, &m_blur[0].texture,
				 &m_blur[1].texture, firstInput, fBlurSize, 0.0f);
		// V: dest blur[1], read blur[0], prevDest blur[0].
		blurPass(&m_blur[1].view, &m_blur[1].texture,
				 &m_blur[0].texture, m_blur[0].descriptor, 0.0f, fBlurSize);
	}

	// blur[1] COLOR_ATTACH -> SHADER_RESOURCE for the composite to sample.
	EmitImageBarrier(apCmd, &m_blur[1].texture, RI_RESOURCE_STATE_RENDER_TARGET,
					 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
}

//-----------------------------------------------------------------------
