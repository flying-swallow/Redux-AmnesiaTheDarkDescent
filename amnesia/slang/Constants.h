#pragma once

#include "HostDefinitions.h"

// Bindless object slots a single SurfelClearByInstance *dispatch* carries (the
// per-dispatch gClearParams UBO holds a uint[] of this size). A plain macro so
// it works as an array bound in both the host C struct and the shader struct
// without depending on cross-module const propagation.
//
// This is a per-dispatch BATCH size, not a frame cap: cHybridRenderer::Draw
// loops the dispatch in chunks of this size until the entire retired-slot list
// is drained in the SAME frame. Retired slots reference freed vertex buffers,
// so deferring any of them to a later frame would let collectCellInfo deref a
// freed buffer-device-address (GPUVM fault) — they must all clear now.
//
// 4096 uints == 16384 B == the Vulkan-guaranteed maxUniformBufferRange, so the
// UBO is portable to any device. The valid count for the final (partial) batch
// is delivered via push constant (see SurfelClearByInstance.cs.slang).
#define HPL_RETIRED_SLOTS_PER_DISPATCH 4096

// Sentinel written into a bindless slot's vertex stream handles
// (gOpaque*Handles) when its VertexBuffer is freed (see cHybridRenderer::Draw).
// 0 is a legitimate handle value (zero-init / "no optional stream"), so retired
// slots are marked with an address that can never be a valid VkDeviceAddress
// rather than 0. SurfelUpdatePass::collectCellInfo recycles any surfel whose
// cached slot reads this value instead of dereferencing the freed
// buffer-device-address (GPUVM fault).
#define HPL_RETIRED_GEOMETRY_HANDLE 0xFFFFFFFFFFFFFFFFull

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
SHARED_CONST float kCellUnit            = 6.0f;
SHARED_CONST float kSurfelTargetArea    = 40000.0f;
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

// Debug counters for SurfelUpdatePass::collectCellInfo. Populated only
// inside that entry point; surfaced in HybridRenderer's per-second
// [SurfelGI] log line. Cheap (atomic-add per dirty surfel × 125 neighbor
// slots in the worst case), kept always-on while the surfel→cell binding
// path stabilises. Remove (and drop the slot count back to 6) once the
// pipeline is settled.
//   DbgAlive       : dirty surfels that passed the radius>0 && life>0
//                    gate — i.e. that reach the cell-binding branch.
//                    Should be == Dirty when surfels are healthy; if
//                    much smaller, surfels are dying before they bind.
//   DbgCellHits    : total (surfel, neighbor-cell) intersections — the
//                    number of InterlockedAdd calls that actually
//                    incremented gCellInfoBuffer.surfelCount. Should be
//                    on the order of DbgAlive × (a few). Zero means
//                    isSurfelIntersectCell rejected every neighbor.
//   DbgCellInvalid : neighbor cells rejected by isCellValid (off the
//                    grid). A scene where the camera + surfels are far
//                    from origin can push the camera-relative cellPos
//                    past ±kCellDimension/2 and silently lose binds.
SHARED_CONST uint kSurfelCounterDbgAlive        = 6u;
SHARED_CONST uint kSurfelCounterDbgCellHits     = 7u;
SHARED_CONST uint kSurfelCounterDbgCellInvalid  = 8u;
// Finer-grained splits of the alive gate (counted on dirty surfels only,
// i.e. one per dispatched thread that survived the dispatch-bounds check).
//   DbgRadiusPos : dirty surfels whose prior-frame radius > 0
//   DbgLifePos   : dirty surfels whose decremented life > 0
// If DbgAlive == 0 but Dirty > 0, comparing these tells you which arm of
// the gate is killing the surfels.
SHARED_CONST uint kSurfelCounterDbgRadiusPos    = 9u;
SHARED_CONST uint kSurfelCounterDbgLifePos      = 10u;

// Scatter-path counters (SurfelGI/SurfelRayTrace.rt.slang). Tells you why
// `surfel.radiance` is staying at zero after the integrate pass:
//   DbgScatHit       : scatterCloseHit invocations — primary scatter-ray
//                      hits. Compares against DbgScatMiss; if Hit≈0 the
//                      TraceRay isn't reaching geometry at all.
//   DbgScatMiss      : scatterMiss invocations.
//   DbgScatNeeNonZero: handleHit calls where evalAnalyticLight returned
//                      a strictly positive direct-lighting contribution.
//                      Hit > 0 but NeeNonZero ≈ 0 means NEE is always
//                      shadowed / out-of-range / NdotL≤0; no surfel
//                      luminance because nothing's feeding it.
//   DbgFinalizeHit   : finalize() calls that found ≥1 surfel in the cell
//                      and added cache radiance. While the cache is cold
//                      this stays at 0 — the surfels boot from NEE alone.
SHARED_CONST uint kSurfelCounterDbgScatHit       = 11u;
SHARED_CONST uint kSurfelCounterDbgScatMiss      = 12u;
SHARED_CONST uint kSurfelCounterDbgScatNeeNonZero = 13u;
SHARED_CONST uint kSurfelCounterDbgFinalizeHit   = 14u;
// Largest per-cell surfelCount the generation pass walked this frame
// (InterlockedMax, one global atomic per 16x16 tile from the elected
// thread). Surfaced as an avg-of-per-frame-max in the [SurfelGI] log line;
// used to size the Stage-2 cooperative-LDS budget. Remove once tuned.
SHARED_CONST uint kSurfelCounterDbgGenSurfelMax  = 15u;
SHARED_CONST uint kSurfelCounterSlotCount        = 16u;

// Static toggle defaults (set/cleared from the host via addDefine in SurfelGI.cpp).
// USE_SURFEL_RADIANCE      - default ON
// LIMIT_SURFEL_SEARCH      - default OFF
// USE_RAY_GUIDING          - default OFF
// USE_SURFEL_DEPTH         - default ON
// USE_IRRADIANCE_SHARING   - default ON

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
SHARED_CONST uint  kDefaultBlendingDelay        = 240u;

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
