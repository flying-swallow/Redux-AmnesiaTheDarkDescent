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
SHARED_CONST uint kBindingOpaqueMaterial             = 21u;  // DiffuseMaterial[]
SHARED_CONST uint kBindingPointLights                = 22u;
SHARED_CONST uint kBindingTranslucentMaterial        = 24u;
SHARED_CONST uint kBindingWaterMaterial              = 25u;
SHARED_CONST uint kBindingDecalMaterial              = 26u;
SHARED_CONST uint kBindingSurfelRefCounter          = 27u;
SHARED_CONST uint kBindingSurfelReservation          = 28u;
SHARED_CONST uint kBindingSpotLights                 = 29u;
SHARED_CONST uint kBindingBoxLights                  = 30u;
SHARED_CONST uint kBindingPackedHitInfo             = 31u;  // RGBA32UI storage image
SHARED_CONST uint kBindingIrradianceMap              = 32u;  // R32F storage image
SHARED_CONST uint kBindingSurfelDepthMap            = 33u;  // RG32F storage image
SHARED_CONST uint kBindingSurfelDepthSampled        = 34u;  // RG32F sampled image (filtered reads)
SHARED_CONST uint kBindingSurfelDepthSampler        = 35u;  // SamplerState paired with above
SHARED_CONST uint kBindingTlas                        = 36u;  // RaytracingAccelerationStructure for surfel RT scatter
SHARED_CONST uint kBindingSurfelBounds                = 37u;  // SurfelBounds (compact cull record, kTotalSurfelLimit)
SHARED_CONST uint kBindingBindlessSlotGeneration      = 38u;  // per object slot: reuse generation (kObjectSlotCapacity)
SHARED_CONST uint kBindingSurfelSlotGeneration        = 39u;  // per surfel: anchor-slot generation captured at spawn (kTotalSurfelLimit)
SHARED_CONST uint kBindingLightGridCount              = 40u;  // RWStructuredBuffer<uint> (kLightGridCellCount) — world-space light grid
SHARED_CONST uint kBindingLightGridList               = 41u;  // RWStructuredBuffer<uint> (kLightGridCellCount * kLightsPerCellMax)

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
SHARED_CONST uint kMaterialSlotCapacity      = 16384u;
SHARED_CONST uint kPointSlotLightCapacity    = 256u;
SHARED_CONST uint kSpotSlotLightCapacity     = 256u;
SHARED_CONST uint kBoxSlotLightCapacity      = 256u;
SHARED_CONST uint kInvalidTextureIndex       = 0xffffffffu;

// -----------------------------------------------------------------------------
// Material-config flag bits packed into DiffuseMaterial.materialConfig by the
// host (CreateMaterailConfigFlags). MIRROR of the TextureConfigFlags enum in
// HPL2/core/include/graphics/MaterialResource.h — keep the two in sync. Shaders
// gate optional shading (e.g. the GGX specular lobe) on these; e.g.
// `(m.materialConfig & kMaterialFlagEnableSpecular) != 0u`.
// -----------------------------------------------------------------------------
SHARED_CONST uint kMaterialFlagEnableNormal   = 1u << 1;
SHARED_CONST uint kMaterialFlagEnableSpecular = 1u << 2;
SHARED_CONST uint kMaterialFlagEnableHeight   = 1u << 4;
SHARED_CONST uint kMaterialFlagEnableCubeMap  = 1u << 6;

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
//   kIrradianceMap{Width,Height,Unit}: atlas geometry (4096×4096, 7×7 tile).
//   kSurfelDepth{Width,Height,Unit}:   depth atlas (same geometry).
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
SHARED_CONST uint2 kIrradianceMapRes       = uint2(4096, 4096);
SHARED_CONST uint2 kIrradianceMapUnit      = uint2(7, 7);
SHARED_CONST uint2 kSurfelDepthTextureRes  = uint2(4096, 4096);
SHARED_CONST uint2 kSurfelDepthTextureUnit = uint2(7, 7);

// World-space light grid (coarse, camera-centered) for surfel NEE importance
// sampling. Covers the same extent as the surfel grid (kCellDimension·kCellUnit)
// but at a coarse resolution so per-cell light lists stay short. Built each
// frame by LightGridBuildPass; read by SurfelRayTrace.rt evalAnalyticLight.
SHARED_CONST uint  kLightGridDim       = 32u;
SHARED_CONST float kLightGridUnit      = (float(kCellDimension) * kCellUnit) / float(kLightGridDim);
SHARED_CONST uint  kLightGridCellCount = kLightGridDim * kLightGridDim * kLightGridDim;  // 32768
SHARED_CONST uint  kLightsPerCellMax   = 32u;                                            // per-cell cap

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

// Static feature toggles. The Falcor reference set these via host addDefine;
// here we #define them directly (Constants.h is included by every SurfelGI
// shader, so the #ifdef blocks see them). Comment a line out to disable.
//   USE_SURFEL_RADIANCE   - multi-bounce surfel feedback in the ray trace.
//                           (the port's `finalize` runs unconditionally, so
//                           this define is informational — kept for parity.)
//   USE_IRRADIANCE_SHARING- SurfelIntegratePass blends each surfel's radiance
//                           with its cell neighbours' — smooths the splotchy
//                           look of sparse coverage.
//   USE_SURFEL_DEPTH      - Chebyshev visibility weighting (reduces light leak)
//                           + writes the surfel-depth atlas. Reference default
//                           ON; left OFF here until enabled deliberately (its
//                           weight suppresses contributions until the depth
//                           atlas converges over the first ~100 frames).
//   USE_RAY_GUIDING       - reference default OFF; leave off.
#define USE_SURFEL_RADIANCE    1
#define USE_IRRADIANCE_SHARING 1
#define USE_SURFEL_DEPTH       1
// #define USE_RAY_GUIDING     1

// -----------------------------------------------------------------------------
// Runtime param defaults (originally SurfelGI::RuntimeParams in SurfelGI.h).
// Kept here so host and UI code share one source of truth.
// -----------------------------------------------------------------------------
SHARED_CONST uint  kMaxSurfelForStep            = 10u;

// Non-physical boost on the light radiance the surfel NEE integrates — cheats
// the indirect/GI term brighter without touching direct. The surfel NEE keeps
// its 1/π Lambert, so π (≈3.14) cancels it → indirect uses the same no-1/π
// convention as the (base-matched) direct in SurfelGIRenderPass.frag; raise
// above π to over-cheat the GI fill.
SHARED_CONST float kIndirectLightScale          = 40.0f;

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

// Evaluation overlay (SurfelEvaluationPass). 0 = normal indirect lighting.
// Non-zero values render diagnostic visualizations:
//   1 = variance, 2 = ray count, 3 = ref count, 4 = life, 5 = coverage.
// Flip in source and recompile shaders to enable a debug overlay; no
// runtime UI hook exists yet.
SHARED_CONST uint  kDefaultOverlayMode          = 0u;

// SurfelUpdate "lock surfel" debug toggle. 0 = normal (surfels reposition
// + reallocate rays each frame), 1 = freeze placement + ray allocation
// for snapshot debugging. Flip in source and recompile to use.
SHARED_CONST uint  kDefaultLockSurfel           = 0u;

// SurfelGIRenderPass per-component toggles. 0 = skip that term, 1 = add
// it. The host used to drive these via a per-pass CB (SurfelGIRenderCB);
// they were always set to 1 there, so they live here as static defaults
// alongside the rest of the SurfelGI knobs. Flip in source + recompile
// to A/B direct vs. indirect.
SHARED_CONST uint  kDefaultRenderDirectLighting    = 1u;
SHARED_CONST uint  kDefaultRenderIndirectLighting  = 1u;

HOST_NAMESPACE_END

#endif // __cplusplus || HPL_DEFINE_SHARED_CONSTS
