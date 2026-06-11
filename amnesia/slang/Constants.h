#pragma once

#include "HostDefinitions.h"

// Surfel GI behavior knobs & runtime-param defaults.
// Bindless buffer capacities, counter-buffer layout, and atlas dimensions
// also live here (moved from SceneTypes.slang).
//
// Sources:
//   SurfelGI/RenderPasses/Surfel/SurfelGI/StaticParams.slang
//   SurfelGI/RenderPasses/Surfel/SurfelGI/SurfelTypes.slang
//   SurfelGI/RenderPasses/Surfel/SurfelGI/SurfelGI.h (StaticParams / RuntimeParams defaults)

// -----------------------------------------------------------------------------
// Standard-BSDF diffuse lobe (Falcor BSDFConfig.slangh analogue). Selects the
// demodulated diffuse weight evaluated by Brdf.slang's diffuseLobeWeight():
//   Lambert   — flat 1 (the old look, modulo the now-explicit 1/π)
//   Disney    — Burley 2012 retro-reflection (fd90 = 0.5 + 2·LdotH²·rough)
//   Frostbite — Disney + Lagarde energy renormalization (Falcor's default)
// Pure preprocessor defines, so they live OUTSIDE the shared-const guard
// below: Brdf.slang's raw #include must see them (an undefined kDiffuseBrdf
// would silently #if-select Lambert).
// -----------------------------------------------------------------------------
#define kDiffuseBrdfLambert   0
#define kDiffuseBrdfDisney    1
#define kDiffuseBrdfFrostbite 2
#ifndef kDiffuseBrdf
#define kDiffuseBrdf kDiffuseBrdfFrostbite
#endif

// The typed constants below are emitted in:
//   - every C++ TU (`static const` at namespace scope = internal linkage, no ODR worry), and
//   - exactly one Slang module: SceneTypes, gated by HPL_DEFINE_SHARED_CONSTS.
// Other Slang modules access them via `import SceneTypes` rather than
// re-emitting copies, which would collide at import sites that pull in
// both SceneTypes and (e.g.) Bindless.resource.
#if defined(__cplusplus) || defined(HPL_DEFINE_SHARED_CONSTS)

HOST_NAMESPACE_BEGIN

// -----------------------------------------------------------------------------
// Engine-owned bindless descriptor set (set 0). Slang accepts a `static const
// uint` reference inside `[vk::binding(N, 0)]` so these can live as typed
// constants rather than preprocessor macros — same name lookup on the C++
// side when populating VkDescriptorSetLayoutBinding.
// -----------------------------------------------------------------------------
SHARED_CONST uint kBindingTextures2D                 = 0u;
SHARED_CONST uint kBindingTexturesCube               = 1u;
SHARED_CONST uint kBindingTextures2DArray           = 2u;
SHARED_CONST uint kBindingOpaquePositionHandles     = 3u;
SHARED_CONST uint kBindingOpaqueTangentHandles      = 4u;
SHARED_CONST uint kBindingOpaqueNormalHandles       = 5u;
SHARED_CONST uint kBindingOpaqueUv0Handles          = 6u;
SHARED_CONST uint kBindingOpaqueColorHandles        = 7u;
SHARED_CONST uint kBindingOpaqueIndexHandles        = 8u;
SHARED_CONST uint kBindingMaterialSampler            = 9u;
SHARED_CONST uint kBindingSurfelCounter              = 10u;
SHARED_CONST uint kBindingSurfelBuffer               = 11u;
SHARED_CONST uint kBindingSurfelGeometry             = 12u;  // cached packed TriangleHit per surfel
SHARED_CONST uint kBindingSurfelValidIndex          = 13u;
SHARED_CONST uint kBindingSurfelDirtyIndex          = 14u;
SHARED_CONST uint kBindingSurfelFreeIndex           = 15u;
SHARED_CONST uint kBindingSurfelRecycle              = 16u;  // SurfelRecycleInfo
SHARED_CONST uint kBindingSurfelRayResult           = 17u;  // SurfelRayResult (kRayBudget)
SHARED_CONST uint kBindingCellInfo                   = 18u;  // CellInfo (kCellCount)
SHARED_CONST uint kBindingCellToSurfel              = 19u;
SHARED_CONST uint kBindingSceneObjects               = 20u;  // UniformObject[]
SHARED_CONST uint kBindingDiffuseMaterial             = 21u;  // DiffuseMaterial[]
SHARED_CONST uint kBindingPointLights                = 22u;
SHARED_CONST uint kBindingTranslucentMaterial        = 24u;
SHARED_CONST uint kBindingWaterMaterial              = 25u;
SHARED_CONST uint kBindingDecalMaterial              = 26u;
SHARED_CONST uint kBindingSurfelRefCounter          = 27u;
SHARED_CONST uint kBindingSurfelReservation          = 28u;
SHARED_CONST uint kBindingSpotLights                 = 29u;
SHARED_CONST uint kBindingPackedHitInfo             = 31u;  // RGBA32UI storage image
SHARED_CONST uint kBindingSurfelDepthMap            = 33u;  // RG32F storage image
SHARED_CONST uint kBindingSurfelDepthSampled        = 34u;  // RG32F sampled image (filtered reads)
SHARED_CONST uint kBindingSurfelDepthSampler        = 35u;  // SamplerState paired with above
SHARED_CONST uint kBindingTlas                        = 36u;  // RaytracingAccelerationStructure for surfel RT scatter
SHARED_CONST uint kBindingSurfelBounds                = 37u;  // SurfelBounds (compact cull record, kTotalSurfelLimit)
SHARED_CONST uint kBindingBindlessSlotGeneration      = 38u;  // per object slot: reuse generation (kObjectSlotCapacity)
SHARED_CONST uint kBindingSurfelSlotGeneration        = 39u;  // per surfel: anchor-slot generation captured at spawn (kTotalSurfelLimit)
SHARED_CONST uint kBindingLightGridCount              = 40u;  // RWStructuredBuffer<uint> (kLightGridCellCount) — world-space light grid
SHARED_CONST uint kBindingLightGridList               = 41u;  // RWStructuredBuffer<uint> (kLightGridCellCount * kLightsPerCellMax)
SHARED_CONST uint kBindingFogAreas                    = 42u;  // RWStructuredBuffer<FogAreaParams> — per-fog-area screen-space iteration
SHARED_CONST uint kBindingPackedRefractionHitInfo     = 43u;  // RGBA32UI storage image — refracted-bounce V-buffer
SHARED_CONST uint kBindingPackedReflectionHitInfo     = 44u;  // RGBA32UI storage image — reflected-bounce V-buffer
SHARED_CONST uint kBindingAttenuationLut              = 45u;  // default light falloff LUT (core_falloff_linear), immutable on set 0
SHARED_CONST uint kBindingDecals                      = 46u;  // RWStructuredBuffer<GpuDecal> (kMaxDecals) — clustered OOB decals
SHARED_CONST uint kBindingObjectDecalIndices          = 47u;  // RWStructuredBuffer<uint> — flat per-object decal-index lists (UniformObject.decalList = offset<<8|count); replaces the decal grid
SHARED_CONST uint kBindingDissolveMap                 = 48u;  // immutable core_dissolve noise (128px), UV-sampled by the shared alphaTest dissolve fade

// -----------------------------------------------------------------------------
// Bindless pool capacities + sentinel.
//   kObjectSlotCapacity:  total scene-object slots the bindless allocator
//                         hands out (set 0, kBindingSceneObjects).
//   k{Texture,Material}SlotCapacity: upper bound on bindless textures /
//                         materials per frame; the host sizes
//                         m_diffuseBindless from kObjectSlotCapacity.
//   k{Point,Spot,Box}SlotLightCapacity: per-frame light SSBO sizing.
//   kInvalidTextureIndex: sentinel returned by the texture slot allocator
//                         when a slot is missing or the pool is exhausted.
// -----------------------------------------------------------------------------
SHARED_CONST uint kObjectSlotCapacity        = 32768u;  // 16384 * 2
SHARED_CONST uint kTextureSlotCapacity       = 16384u;
// Material ids form one continuous, range-partitioned space across three typed
// buffers. Each range's accessor de-biases the id by the range base:
//   solid+decal : [0, kSolidMaterialCapacity)                  -> gDiffuseMaterials[id]
//   translucent : [kTranslucentMaterialBase, kWaterMaterialBase) -> gTranslucentMaterials[id - base]
//   water       : [kWaterMaterialBase, +kWaterMaterialCapacity)   -> gWaterMaterials[id - base]
// isTranslucent / isWater are the range bounds checks; see Scene.slang.
SHARED_CONST uint kSolidMaterialCapacity     = 2048u;   // solid + decal slots (range base 0)
SHARED_CONST uint kTranslucentMaterialCapacity = 256u;  // dedicated translucent/glass table
SHARED_CONST uint kWaterMaterialCapacity     = 64u;     // dedicated compact water table (scenes use <=64)
SHARED_CONST uint kTranslucentMaterialBase   = kSolidMaterialCapacity;
SHARED_CONST uint kWaterMaterialBase         = kSolidMaterialCapacity + kTranslucentMaterialCapacity;
SHARED_CONST uint kPointSlotLightCapacity    = 256u;
SHARED_CONST uint kSpotSlotLightCapacity     = 256u;
SHARED_CONST uint kFogAreaCapacity           = 32u;
SHARED_CONST uint kMaxDecals                  = 4096u;  // gDecals[] capacity (clustered OOB decals)
SHARED_CONST uint kInvalidTextureIndex       = 0xffffffffu;

// -----------------------------------------------------------------------------
// TLAS instance-mask categories. Each TLAS instance picks one (or more) of
// these bits; each TraceRay / TraceRayInline passes a cull mask of the
// categories it wants to hit. instance.mask & ray.cullMask must be non-zero
// for a hit to be considered. Translucents joined the TLAS in the same
// commit that added the mesh translucent / refraction-bounce path, and the
// surfel indirect-lighting path tracer must not bounce off them — sampling
// a translucent's shared DiffuseMaterial slot (no albedo texture, no solid
// scalars) feeds NaN into rayResult.radiance, which poisons surfel.radiance
// via MSME and surfaces as NaN in SurfelGenerationPass gOutput.
SHARED_CONST uint kRayMaskOpaque             = 0x01u;
SHARED_CONST uint kRayMaskTranslucent        = 0x02u;
SHARED_CONST uint kRayMaskAll                = 0xffu;

// -----------------------------------------------------------------------------
// Material-config flag bits packed into DiffuseMaterial.materialConfig by the
// host (CreateMaterailConfigFlags in MaterialResource.cpp). Single source of
// truth shared with C++ via the SHARED_CONST macro — there is no separate
// enum on the host side; MaterialResource.cpp consumes these directly.
// Shaders gate optional shading (e.g. the GGX specular lobe) on these; e.g.
// `(m.materialConfig & kMaterialFlagEnableSpecular) != 0u`.
// -----------------------------------------------------------------------------
SHARED_CONST uint kMaterialFlagEnableDiffuse            = 1u << 0;
SHARED_CONST uint kMaterialFlagEnableNormal             = 1u << 1;
SHARED_CONST uint kMaterialFlagEnableSpecular           = 1u << 2;
SHARED_CONST uint kMaterialFlagEnableAlpha              = 1u << 3;
SHARED_CONST uint kMaterialFlagEnableHeight             = 1u << 4;
SHARED_CONST uint kMaterialFlagEnableIllumination       = 1u << 5;
SHARED_CONST uint kMaterialFlagEnableCubeMap            = 1u << 6;
SHARED_CONST uint kMaterialFlagEnableDissolveAlpha      = 1u << 7;
SHARED_CONST uint kMaterialFlagEnableCubeMapAlpha       = 1u << 8;
SHARED_CONST uint kMaterialFlagIsHeightMapSingleChannel = 1u << 9;   // parallax samples .r vs .a
SHARED_CONST uint kMaterialFlagIsAlphaSingleChannel     = 1u << 10;
// Water surface — drives the SurfelVBuffer.rt wave-animated refraction +
// reflection bounce and the GIRenderPass refraction swap (water also sets
// HasRefraction). Not a texture-presence flag; set host-side per MaterialID.
// Bit 11 is free (12/13 unused, 14/15/16 are the refraction/dissolve bits).
SHARED_CONST uint kMaterialFlagIsWater                  = 1u << 11;
// Bit 14 is solid-diffuse UseDissolveFilter aliased with translucent
// UseRefractionNormals — safe because the two material variants never share
// a draw call. Keep both names so the host populates either flag against the
// same bit per material kind.
SHARED_CONST uint kMaterialFlagUseDissolveFilter        = 1u << 14;
SHARED_CONST uint kMaterialFlagUseRefractionNormals     = 1u << 14;
SHARED_CONST uint kMaterialFlagUseRefractionEdgeCheck   = 1u << 15;
SHARED_CONST uint kMaterialFlagHasRefraction            = 1u << 16;
// Translucent "AffectedByLightLevel" material var: the shader dims the
// translucent/particle color by the surrounding analytic-light level
// (gScene.lightLevelAt) instead of rendering it full-bright.
SHARED_CONST uint kMaterialFlagAffectedByLightLevel     = 1u << 17;

// Perceptual blend-weight exponent for the particle/translucent passes.
// The legacy renderer blended display-space (gamma) values; this renderer
// blends linear HDR, and sRGB-encode-after-lerp reads brighter than the
// base game's lerp-after-encode. Source-side math is computed in display
// space and decoded once at output (exact for products/lerps); the
// blend-with-dst sum gets the power distributed over its weights instead:
// ALPHA goes premultiplied with src·αᵏ and dst·(1−α)ᵏ. 2.2 = pure
// power-law distribution of the sRGB decode; lower biases brighter
// (toward the plain-linear look). Tune against the base game.
SHARED_CONST float kPerceptualBlendExp = 2.2f;

// Blend-mode value families (see BlendModes.slang for the shared math).
// Two DIFFERENT encodings of the same modes — do not mix them up:
//   1. kBlendMode*: the push-constant / pipeline scheme. Mirrors the
//      BlendMode enums in Decal/Particle/TranslucentMeshPipelineDesc.h and
//      the remapBlend writes at the HybridRenderer.cpp draw sites.
//   2. kMaterialBlendMode*: raw hpl::eMaterialBlendMode (GraphicsTypes.h,
//      None first), stored verbatim into GpuDecal.blendMode and consumed by
//      the composite's exact decal blend.
SHARED_CONST uint kBlendModeAdd         = 0u;
SHARED_CONST uint kBlendModeMul         = 1u;
SHARED_CONST uint kBlendModeMulX2       = 2u;
SHARED_CONST uint kBlendModeAlpha       = 3u;
SHARED_CONST uint kBlendModePremulAlpha = 4u;

SHARED_CONST uint kMaterialBlendModeNone        = 0u;
SHARED_CONST uint kMaterialBlendModeAdd         = 1u;
SHARED_CONST uint kMaterialBlendModeMul         = 2u;
SHARED_CONST uint kMaterialBlendModeMulX2       = 3u;
SHARED_CONST uint kMaterialBlendModeAlpha       = 4u;
SHARED_CONST uint kMaterialBlendModePremulAlpha = 5u;

// -----------------------------------------------------------------------------
// Surfel-GI capacities + cell-grid sizing.
//   kTotalSurfelLimit:    surfel buffer head count.
//   kRayBudget:           per-frame ray slots in gSurfelRayResultBuffer.
//   kCellDimension:       linear cell count along each axis of the uniform
//                         world-space hash grid.
//   kCellCount:           total cells (= kCellDimension^3).
//   kCellToSurfelCapacity: backing array for the cell→surfel index lists.
//   kCellUnit:            world-space side length of one cell.
//   kSurfelTargetArea:    surfel-radius target area used by collectCellInfo.
//   kPerCellSurfelLimit:  per-cell surfel cap (debug + sanity bound).
//   k{Ref,Max,SleepingMax}*Life / kRefCountThreshold: surfel lifecycle.
//   kSurfelDepth{Width,Height,Unit}:   depth atlas geometry (4096×4096, 7×7 tile).
// -----------------------------------------------------------------------------
SHARED_CONST uint  kTotalSurfelLimit    = 150000u;
SHARED_CONST uint  kRayBudget           = kTotalSurfelLimit * 64u;
SHARED_CONST uint  kCellDimension       = 250u;
SHARED_CONST uint  kCellCount           = kCellDimension * kCellDimension * kCellDimension;
SHARED_CONST uint  kCellToSurfelCapacity = kTotalSurfelLimit * 125u;
SHARED_CONST float kCellUnit            = 0.5f;
SHARED_CONST float kSurfelTargetArea    = 50000.0f;
SHARED_CONST uint  kPerCellSurfelLimit  = 1024u;
SHARED_CONST uint  kRefCountThreshold   = 32u;
SHARED_CONST uint  kMaxLife             = 240u;
SHARED_CONST uint  kSleepingMaxLife     = kMaxLife / 4u;
SHARED_CONST uint2 kTileSize            = uint2(16, 16);
// uint2 atlas geometry — surfel utility math uses `.x` / `.y` accessors.
SHARED_CONST uint2 kSurfelDepthTextureRes  = uint2(4096, 4096);
SHARED_CONST uint2 kSurfelDepthTextureUnit = uint2(7, 7);
// Half-extent of one depth-atlas tile (Falcor SurfelTypes.slang:
// kSurfelDepthTextureUnit / 2). Used by SurfelIntegratePass's octahedral
// depth-write offset math.
SHARED_CONST uint2 kSurfelDepthTextureHalfUnit = uint2(3, 3);

// World-space light grid (coarse, camera-centered) for surfel NEE importance
// sampling. Covers the same extent as the surfel grid (kCellDimension·kCellUnit)
// but at a coarse resolution so per-cell light lists stay short. Built each
// frame by LightGridBuildPass; read by SurfelRayTrace.rt evalAnalyticLight.
SHARED_CONST uint  kLightGridDim       = 128u;
SHARED_CONST float kLightGridUnit      = (float(kCellDimension) * kCellUnit) / float(kLightGridDim);
SHARED_CONST uint  kLightGridCellCount = kLightGridDim * kLightGridDim * kLightGridDim;  // 32768
SHARED_CONST uint  kLightsPerCellMax   = 128u;                                           // per-cell light-list cap (list buffer = kLightGridCellCount·this·4B); lights past it are dropped by atomic order

// Decals use a precomputed per-object decal list (UniformObject.decalList →
// gObjectDecalIndices), not a spatial grid — see SceneTypes.UniformObject and
// World::Compile. kMaxObjectDecalIndices caps the flat association pool.
SHARED_CONST uint  kMaxObjectDecalIndices = 1u << 20;   // 1M (offset is 24-bit; cap well under that)

// -----------------------------------------------------------------------------
// GI bounce-albedo boost — art-directed, applied in the surfel RAY PATH only
// (SurfelRayTrace NEE + bounce throughput); the direct pass and the composite's
// albedo multiply stay physically correct. Amnesia's diffuse sets are dark
// (measured mean GI-hit albedo ≈ 0.04 linear), so a physically-correct bounce
// keeps ~4% of the light's energy and indirect reads as black. Bounce albedo
// becomes pow(albedo, exp): 1.0 = physically correct, 0.5 (sqrt) lifts 0.04 →
// 0.2 (~5× indirect on this content) — the same knob as Lumen's Diffuse Color
// Boost. Per-channel pow slightly desaturates dark hues; expected.
// -----------------------------------------------------------------------------
SHARED_CONST float kSurfelGIAlbedoBoostExp = 1.0f;

// -----------------------------------------------------------------------------
// gSurfelCounter slot indices. The counter is a flat
// RWStructuredBuffer<uint> sized to kSurfelCounterSlotCount. Slots 0..5
// mirror the SurfelCounterOffset enum; SurfelPreparePass zeroes them each
// frame and HybridRenderer's stats-log block reads them for the per-second
// log line.
// -----------------------------------------------------------------------------
SHARED_CONST uint kSurfelCounterValid          = 0u;
SHARED_CONST uint kSurfelCounterDirty          = 1u;
SHARED_CONST uint kSurfelCounterFree           = 2u;
SHARED_CONST uint kSurfelCounterCell           = 3u;
SHARED_CONST uint kSurfelCounterRequestedRay   = 4u;
SHARED_CONST uint kSurfelCounterMissBounce     = 5u;
SHARED_CONST uint kSurfelCounterSlotCount      = 6u;

// -----------------------------------------------------------------------------
// Runtime param defaults (originally SurfelGI::RuntimeParams in SurfelGI.h).
// Kept here so host and UI code share one source of truth.
// -----------------------------------------------------------------------------
SHARED_CONST uint  kMaxSurfelForStep            = 10u;

// Surfel generation.
SHARED_CONST float kDefaultChanceMultiply       = 0.3f;
SHARED_CONST uint  kDefaultChancePower          = 1u;
SHARED_CONST float kDefaultPlacementThreshold   = 2.f;
SHARED_CONST float kDefaultRemovalThreshold     = 4.f;
SHARED_CONST float kDefaultThresholdGap         = 2.f;
// Frames a freshly-spawned surfel ramps in over before it contributes fully
// to the indirect output (smoothstep(0, delay, frame) in SurfelGenerationPass).
// The Falcor reference uses 240 (~4s @ 60fps); on this camera-relative grid a
// surfel must survive that long before it shows up, so a moving camera / churn
// can keep indirect at ~0. Reduced to ramp in under ~0.5s while debugging.
SHARED_CONST uint  kDefaultBlendingDelay        = 30u;

// Ray tracing.
SHARED_CONST float kDefaultVarianceSensitivity  = 40.f;
SHARED_CONST uint  kDefaultMinRayCount          = 4u;
SHARED_CONST uint  kDefaultMaxRayCount          = 64u;
SHARED_CONST uint  kDefaultRayStep              = 3u;
SHARED_CONST uint  kDefaultMaxStep              = 6u;

// Integrate.
SHARED_CONST float kDefaultShortMeanWindow      = 0.03f;

// determins the strenght of the wave intensity
SHARED_CONST float kWaterRefractionIntensity = 0.6f;

// Brightness multiplier for the water refraction/reflection bounce radiance.
// The RT bounces are re-shaded (NEE direct + surfel indirect + emission), which
// reads dimmer than the base game's fully-lit framebuffer/cubemap sample; this
// lifts them back toward that brightness. 1.0 = raw re-shade.
SHARED_CONST float kWaterReflectionExposure = 1.0f;
SHARED_CONST float kWaterRefractionExposure = 2.0f; //0.5f;

// How much wave turbulence the REFLECTION bounce normal keeps (the refraction
// bounce always uses the full wave normal). The RT reflection is a sharp mirror
// trace; blending its normal back toward the flat surface normal calms the
// ripples so it reads more like a mirror and less turbulent. 0 = perfect
// mirror, 1 = full wave distortion.
SHARED_CONST float kWaterReflectionTurbulence = 0.3f;

// Evaluation overlay (SurfelEvaluationPass). 0 = normal indirect lighting.
// Non-zero values render diagnostic visualizations:
//   1 = variance, 2 = ray count, 3 = ref count, 4 = life, 5 = coverage.
// Flip in source and recompile shaders to enable a debug overlay; no
// runtime UI hook exists yet.
SHARED_CONST uint  kDefaultOverlayMode          = 0u;

// MainCompositePass per-component toggles. 0 = skip that term, 1 = add
// it. The host used to drive these via a per-pass CB (SurfelGIRenderCB);
// they were always set to 1 there, so they live here as static defaults
// alongside the rest of the SurfelGI knobs. Flip in source + recompile
// to A/B direct vs. indirect.
SHARED_CONST uint  kDefaultRenderDirectLighting    = 1u;
SHARED_CONST uint  kDefaultRenderIndirectLighting  = 1u;
// Direct-lighting temporal accumulation (SVGF temporal stage, in the resolve
// pass). Running mean weighted by history length: alpha = max(1/count,
// kDirectTemporalAlpha), count capped at kDirectMaxAccum. The 1/count term gives
// an exact average for the first ~1/floor frames; the floor then keeps it
// adaptive so changing lighting isn't permanently lagged.
//
// Steady-state residual noise ≈ sqrt(alpha/(2-alpha)) × per-frame noise, so the
// FLOOR is the main grain knob on a static view: 0.05 → ~0.16×, 0.02 → ~0.10×,
// 0.01 → ~0.07×. Lower floor / higher cap = smoother but laggier on lighting
// changes (disocclusion still resets count=1, so newly-revealed areas recover
// cleanly regardless). Tuned down from 0.05/16 to settle a grainy static image;
// drop the floor further (toward 0.01) if it's still grainy, raise it (toward
// 0.05) if dynamic lights start to smear.
SHARED_CONST float kDirectTemporalAlpha            = 0.05f;
SHARED_CONST float kDirectMaxAccum                 = 32.0f;
// Disocclusion rejection for the direct pass: reproject the surface key (view
// depth + normal) and reject history (restart accumulation) when the reprojected
// surface differs — kills camera-motion smear at silhouettes.
SHARED_CONST float kReprojZRelTol                  = 0.05f;   // ≤5% view-depth difference accepted
SHARED_CONST float kReprojNormalCos                = 0.9f;    // ≈25° normal tolerance
// Disocclusion seed: a fresh pixel borrows already-converged direct from
// same-surface neighbors in the previous accumulation, searching a
// (2R+1)² window (no shadow rays). Larger = more reuse, more taps.
SHARED_CONST int   kDisoccSearchRadius             = 3;       // 7×7 spatial reuse window
// SVGF-lite spatial denoise for the direct pass. After temporal accumulation, an
// edge-aware à-trous wavelet filter (5×5, step doubling each iteration) shares the
// 1-spp estimate across same-surface neighbors — the spatial half the temporal-only
// accumulation was missing, and the thing that hides the per-frame speckle / the
// disocclusion noise spike. Edge-stopping weights gate on view depth (σZ), world
// normal (σN exponent), and luminance (σL, scaled by a local variance estimate).
// kDirectVarianceBoost widens the luminance gate while the history is short
// (count low) so fresh pixels blur harder before they've converged.
SHARED_CONST int   kAtrousIterations               = 3;       // edge-aware à-trous passes (host loop count)
SHARED_CONST float kSvgfSigmaZ                      = 4.0f;    // depth edge-stop
SHARED_CONST float kSvgfSigmaN                      = 64.0f;   // normal edge-stop exponent
SHARED_CONST float kSvgfSigmaL                      = 4.0f;    // luminance edge-stop
SHARED_CONST float kDirectVarianceBoost            = 4.0f;    // early-frame (low count) variance scale
// ReSTIR DI (Bitterli 2020, biased reuse). The direct pass keeps a per-pixel
// light-selection reservoir instead of a radiance running-mean: a streaming RIS
// build over the cell lights, then temporal reuse (reproject + merge previous
// frame's reservoir) and spatial reuse (merge a few same-surface neighbours).
// Effective candidate count grows into the hundreds at one shadow ray/pixel/
// frame, which is the root-cause fix for many-light noise.
//   kReservoirMClamp — cap the reprojected previous M at N× the current build M
//     so stale lighting can't dominate (responsiveness vs. variance).
//   kSpatialSamples / kSpatialRadius — neighbour count + pixel search radius for
//     the spatial-reuse pass; neighbours are geometry-rejected on the depth/
//     normal key (kReproj* tolerances) to stop light leaking across surfaces.
SHARED_CONST float kReservoirMClamp                = 20.0f;   // temporal staleness cap (× current M)
SHARED_CONST int   kSpatialSamples                 = 4;       // spatial neighbours resampled
SHARED_CONST float kSpatialRadius                  = 16.0f;   // spatial search radius (px)
// Guard band / overscan: the whole frame renders at (1 + 2·kGuardBandFraction)
// the display size with a correspondingly wider FOV, and the present blit crops
// the center back to the display. Gives temporal reprojection valid history just
// off the visible edge so camera pans don't show edge artifacts. Fill cost scales
// ~(1+2f)². Crop inset per side (UV) = f/(1+2f).
//
// DISABLED (set to 0): the overscan render misaligns everything that assumes
// the authored projection — editor DebugDraw overlays (selection wireframes,
// gizmos, grid) and UI picking (mouse-ray unproject). Re-enabling requires
// compensating both the overlay flush (DebugDraw) and the picking path.
// Original value: 0.06 (≈6% per side).
SHARED_CONST float kGuardBandFraction              = 0.0f;

// PBR point/spot-light falloff — Falcor LightData semantics, no scale knobs:
// `intensity` IS the light's radiance (the host uploads the authored engine
// radius verbatim for both point and spot — one unit for every analytic light,
// so direct and the surfel NEE agree by construction). The shader emits
// radiance = color · intensity · 1/(d² + sourceRadius²) — a softened
// inverse-square that goes HDR (>1) near the source so lights cross the bloom
// white point. Brightness tuning is an AUTHORING concern (light radius /
// color), not a constant.
//
// LightGridBuildPass bins each light out to the distance where its brightest
// channel's radiance dims to kLightRadianceFloor:
//   reach² = maxChannel(color) · intensity / kLightRadianceFloor − sourceRadius²
// reach² ≤ 0 (peak below the floor) drops the light entirely. NOTE: this bin reach
// also governs indirect/GI spread — the surfel NEE only samples lights binned into a
// surfel's cell, so lowering the floor widens and brightens GI (and crowds the
// per-cell light cap); it is not purely a grid-cost knob.
SHARED_CONST float kLightRadianceFloor         = 0.005f;    // min per-channel radiance (linear) worth binning; reach² = maxC(color)·authoredRadius/floor − sourceRadiusSq (scale-independent)
SHARED_CONST float kPointLightSourceRadiusSq   = 0.25f;  // soft source radius² (0.5m) — near-field softening + on-source peak cap (caps on-lamp radiance at color·intensity/this instead of 1/d²→∞; raise to soften lamp hotspots further)
// Default soft-shadow source radius when a light authors none (GetSourceRadius()==0):
// a fraction of the authored reach radius, so the penumbra scales with the lamp and
// lights are soft by default instead of hard. Host-only fallback in the point-light
// upload; an explicitly authored sourceRadius always wins.
SHARED_CONST float kPointLightDefaultSourceRadiusFrac = 0.05f;  // 5% of authored radius (e.g. 5m reach ⇒ 0.25m penumbra disk)

SHARED_CONST float kParalaxScale = 0.4f;


// Diffuse-lobe roughness for materials without a specular map (no gloss
// channel to decode): fully rough = matte with the strongest grazing
// retro-reflection. Materials with a spec map decode their roughness from
// tex[3].y (Brdf.slang decodeMaterialRoughness).
SHARED_CONST float kDefaultDiffuseRoughness = 1.0f;


HOST_NAMESPACE_END

#endif // __cplusplus || HPL_DEFINE_SHARED_CONSTS
