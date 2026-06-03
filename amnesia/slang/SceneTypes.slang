#pragma once
#include "HostDefinitions.h"
// SceneTypes is the canonical Slang home for the SHARED_CONST capacity /
// surfel-grid / counter-slot constants declared in Constants.h. Setting
// HPL_DEFINE_SHARED_CONSTS makes Constants.h emit them here; other Slang
// modules that need them `import SceneTypes` instead of re-#including.
#define HPL_DEFINE_SHARED_CONSTS
#include "Constants.h"
#undef HPL_DEFINE_SHARED_CONSTS

HOST_NAMESPACE_BEGIN

// -----------------------------------------------------------------------------
// Type discriminators. Every Light/Material struct stores its discriminator
// in `type` at offset 0, so any consumer can read the first 4 bytes of a
// record to identify its concrete kind without knowing which buffer it came
// from. This is the contract the ILight / IMaterial interfaces and the
// surfel GI iteration loops rely on.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Scalar-layout point light. `type` discriminator at offset 0 (ILight contract).
// `intensity` is the light's luminous power (CPU-derived in HybridRenderer's
// point-light loop as authoredRadius²·kPointLightIntensityScale; color already
// carries brightness, so no luminance term). The radiance falloff
// (interface.slang) is a plain WINDOWLESS inverse-square, so intensity only sets
// brightness there; the reach cutoff (reach² = maxChannel(color)·intensity /
// kLightRadianceFloor − kPointLightSourceRadiusSq) lives solely in
// LightGridBuildPass binning. `color` is the linear diffuse
// colour. `goboTextureIndex` points into
// textures_cube[] (set 0, binding 1); when set, sample the cube in light-local
// space using the worldToLight* rows — the rows of the light's world rotation (R),
// which reconstruct R^T via `float3x3(worldToLightX, worldToLightY, worldToLightZ)`.
// -----------------------------------------------------------------------------
SLANG_PUBLIC struct PointLight {
    SLANG_PUBLIC uint  type;                         // LIGHT_TYPE_POINT
    SLANG_PUBLIC uint  goboTextureIndex;
    SLANG_PUBLIC float3  position;
    SLANG_PUBLIC float intensity;                    // luminous power (CPU-derived)
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC float3  worldToLightX;
    SLANG_PUBLIC float3  worldToLightY;
    SLANG_PUBLIC float3  worldToLightZ;
    SLANG_PUBLIC float radius;                       // CPU-precomputed light-grid bin reach (world units); 0 ⇒ skip
};

// Scalar-layout spot light. `intensity` = luminous power (same convention as
// PointLight; bin reach derived from the radiance floor in-shader — see PointLight
// above). The shader uses the windowless inverse-square radial falloff plus an
// angular cone term. Carries
// the cone forward + pre-baked cos(angle/2), an optional projected gobo, a
// shadow bit, and the light's view-projection matrix (gobo / future shadow-map
// UVs).
SLANG_PUBLIC struct SpotLight {
    SLANG_PUBLIC uint  type;                         // LIGHT_TYPE_SPOT
    SLANG_PUBLIC uint  goboTextureIndex;
    SLANG_PUBLIC float3  position;
    SLANG_PUBLIC float intensity;                    // luminous power (CPU-derived)
    SLANG_PUBLIC float3  direction;                    // world-space outward forward
    SLANG_PUBLIC float cosOuterAngle;                // cos(GetFOV() * 0.5)
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC uint  shadowEnabled;                // 0 or 1
    SLANG_PUBLIC float4x4  viewProjection;               // light-space ViewProj for gobo UVs
    SLANG_PUBLIC float radius;                       // CPU-precomputed light-grid bin reach (world units); 0 ⇒ skip
};

// Per-fog-area params for the screen-space fog iteration. One entry per
// active cFogArea each frame, capped at kFogAreaCapacity. The shader walks
// the SSBO per-pixel, intersects the eye→surface ray with each box, and
// accumulates a visibility scalar + fog colour contribution.
//
// `invModelMat` is the inverse of cFogArea::GetModelMatrix — it sends a
// world-space point into the area's unit-box local space ([-0.5, 0.5]^3).
// `flags`: bit 0 = ShowBacksideWhenInside, bit 1 = ShowBacksideWhenOutside.
// `color.rgb` is the linear fog colour; `colorA` is the legacy
// GetColor().a (fog alpha — 1.0 means fully opaque fog at full distance).
SLANG_PUBLIC struct FogAreaParams {
    SLANG_PUBLIC float4x4 invModelMat;     // 64 B
    SLANG_PUBLIC float3   color;           // 12 B (linear)
    SLANG_PUBLIC float    colorA;          //  4 B
    SLANG_PUBLIC float    start;           //  4
    SLANG_PUBLIC float    end;             //  4
    SLANG_PUBLIC float    falloffExp;      //  4
    SLANG_PUBLIC uint     flags;           //  4
};

// Texture-slot indices into the bindless `textures_2d[]` array (set=0,
// binding=0). One uint32 per slot — see per_frame.resource.glsl for the
// `DiffuseMaterial_*Texture_ID` accessors that index this array.
// Slot layout: 0=Diffuse, 1=NMap, 2=Alpha, 3=Specular,
//              4=Height, 5=Illumination, 6=DissolveAlpha, 7=CubeMapAlpha.
// `cubeMapTextureIndex` is the parallel reflection-cubemap slot (textures_cube[]).
SLANG_PUBLIC struct DiffuseMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_DIFFUSE
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  cubeMapTextureIndex;          // kInvalidTextureIndex when absent
    SLANG_PUBLIC uint  tex[8];
    SLANG_PUBLIC float heightMapScale;               // SolidDiffuse-only
    SLANG_PUBLIC float heightMapBias;                // SolidDiffuse-only
    SLANG_PUBLIC float frenselBias;                  // SolidDiffuse
    SLANG_PUBLIC float frenselPow;                   // SolidDiffuse
};

// GPU twin of MaterialTranslucent (Material.h). All bool flags
// (isAffectedByLightLevel, hasRefraction, refractionEdgeCheck,
// refractionNormals) and the eMaterialBlendMode pack into materialConfig.
SLANG_PUBLIC struct TranslucentMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_TRANSLUCENT
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  cubeMapTextureIndex;          // reflection cubemap; kInvalidTextureIndex when absent
    SLANG_PUBLIC uint  tex[8];
    SLANG_PUBLIC float refractionScale;
    SLANG_PUBLIC float frenselBias;
    SLANG_PUBLIC float frenselPow;
    SLANG_PUBLIC float rimLightMul;
    SLANG_PUBLIC float rimLightPow;
};

// GPU twin of MaterialWater (Material.h). Bool flags (hasReflection,
// isLargeSurface, worldReflectionOcclusionTest) pack into materialConfig.
SLANG_PUBLIC struct WaterMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_WATER
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  tex[8];
    SLANG_PUBLIC float refractionScale;
    SLANG_PUBLIC float frenselBias;
    SLANG_PUBLIC float frenselPow;
    SLANG_PUBLIC float reflectionFadeStart;
    SLANG_PUBLIC float reflectionFadeEnd;
    SLANG_PUBLIC float waveSpeed;
    SLANG_PUBLIC float waveAmplitude;
    SLANG_PUBLIC float waveFreq;
};

// GPU twin of MaterialDecal (Material.h). Only carries texture slots and the
// blend mode (encoded in materialConfig); no per-instance scalars.
SLANG_PUBLIC struct DecalMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_DECAL
    SLANG_PUBLIC uint  materialConfig;
    SLANG_PUBLIC uint  tex[8];
};

SLANG_PUBLIC struct UniformObject {
    SLANG_PUBLIC float dissolveAmount;
    SLANG_PUBLIC uint  materialID;
    SLANG_PUBLIC float lightLevel;
    SLANG_PUBLIC float illuminationAmount;
    SLANG_PUBLIC float4x4  modelMat;
    SLANG_PUBLIC float4x4  invModelMat;
    SLANG_PUBLIC float4x4  uvMat;
    // Precomputed per-object decal list, packed (offset << 8) | count, indexing
    // gObjectDecalIndices[] (stable gDecals[] indices). Built once at load by
    // World::Compile from the static decal↔object intersection (the editor
    // baked decals as static meshes clipped to specific geometry). count==0 for
    // dynamic/movable objects, so they never receive decals. compositeDecals
    // iterates only this list — no spatial grid, no receiver-category bits.
    SLANG_PUBLIC uint  decalList;
};

// Clustered OOB decal. No geometry: invModelMat maps a world position into the
// decal's unit-cube box [-0.5,0.5]^3; the projection pass clips to the box and
// samples the decal texture (DiffuseMaterial slot `materialID`). receiverMask is
// the eDecalReceiver filter; blendMode is eMaterialBlendMode. Packed each frame
// from cDecal into gDecals[] (binding kBindingDecals).
SLANG_PUBLIC struct GpuDecal {
    SLANG_PUBLIC float4x4 invModelMat;
    SLANG_PUBLIC float4   color;
    SLANG_PUBLIC uint     materialID;
    SLANG_PUBLIC uint     receiverMask;
    SLANG_PUBLIC uint     blendMode;
    SLANG_PUBLIC uint     subDivX;        // texture-atlas grid (editor SubDiv)
    SLANG_PUBLIC uint     subDivY;
    SLANG_PUBLIC uint     subDivIndex;    // selected atlas cell (editor CurrentSubDiv)
    // World-space projection axis (decal local +Y). projectDecal rejects surfaces
    // facing away from it (matches DecalCreator's per-triangle backface cull).
    SLANG_PUBLIC float3   projAxisWS;
    SLANG_PUBLIC float    _pad;
};

// -----------------------------------------------------------------------------
// Surfel-GI shared types (HitInfo / TriangleHit / VertexData / LightSample).
// Used by the ray-tracing surfel passes for hit packing and NEE sampling.
// -----------------------------------------------------------------------------

// Result of sampling any ILight from a shading point. `dir` points from the
// shading point toward the light; `distance` is finite for analytic lights
// and used to terminate the shadow ray exactly at the source.
SLANG_PUBLIC struct LightSample {
    SLANG_PUBLIC float3  Li;              // incident radiance (color * intensity * falloff)
    SLANG_PUBLIC float pdf;             // sampling pdf (1.0 for analytic)
    SLANG_PUBLIC float3  dir;             // unit dir from shading point to light
    SLANG_PUBLIC float distance;        // shadow-ray tMax
};

// Packed primary-ray hit. Mirrors the layout of `gPackedHitInfo` (a uint4
// V-buffer image) used across the surfel V-buffer / G-buffer port:
//   .x : geometry instance id
//   .y : primitive (triangle) index inside the instance
//   .z : barycentrics packed as two float16s (low=b1, high=b2; b0 = 1-b1-b2)
//   .w : 0 (reserved). All-zero packed = "no hit", which `isValid()` tests.
// Wraps a single uint4 so shader code can do `HitInfo h = HitInfo(packed)`,
// matching Falcor's source-compatible constructor pattern.
SLANG_PUBLIC struct HitInfo {
    SLANG_PUBLIC uint4 packed;
};

// Closer-to-Falcor TriangleHit alias; same logical content as HitInfo but
// unpacked. Kept distinct so future hit kinds (procedural, displaced) can
// grow into HitInfo without disturbing the surfel triangle-only paths.
SLANG_PUBLIC struct TriangleHit {
    SLANG_PUBLIC uint  instanceID;
    SLANG_PUBLIC uint  primitiveIndex;
    SLANG_PUBLIC float2  barycentrics;    // (b1, b2); b0 = 1 - b1 - b2
};

// Per-hit interpolated vertex data. World-space because every consumer of it
// (surfel placement, cell hashing, NEE shadow origin) wants world-space.
SLANG_PUBLIC struct VertexData {
    SLANG_PUBLIC float3  posW;
    SLANG_PUBLIC float3  normalW;
    SLANG_PUBLIC float3  tangentW;
    SLANG_PUBLIC float2  texC;
};

// -----------------------------------------------------------------------------
// Per-frame scene UBO contents (scalar block layout). Bound on the GPU at
// set=1, binding=0 — see PerFrame.resource.slang. Populated by
// cHybridRenderer::Draw() each frame. Scalar layout means float3 packs at
// 12 B with 4-byte alignment, matching the host C++ struct exactly.
// -----------------------------------------------------------------------------
SLANG_PUBLIC struct SceneConstants {
    SLANG_PUBLIC float4x4  invViewRotationMat;
    SLANG_PUBLIC float4x4  viewMat;
    SLANG_PUBLIC float4x4  invViewMat;
    SLANG_PUBLIC float4x4  projMat;
    SLANG_PUBLIC float4x4  invProjMat;                 // for WorldPosFromDepth

    SLANG_PUBLIC float worldFogStart;
    SLANG_PUBLIC float worldFogLength;
    SLANG_PUBLIC float oneMinusFogAlpha;
    SLANG_PUBLIC float fogFalloffExp;
    SLANG_PUBLIC float4  worldFogColor;

    SLANG_PUBLIC float2  viewTexel;
    SLANG_PUBLIC float2  viewportSize;
    SLANG_PUBLIC float afT;
    SLANG_PUBLIC uint  totalFrames;                // monotonic frame counter
    SLANG_PUBLIC float cameraFov;                  // main camera vertical FOV (radians)
    SLANG_PUBLIC uint  pointLightCount;            // active entries in gPointLights[]
    SLANG_PUBLIC float fireflyClampThreshold;      // luminance cap in surfel raytrace + integrate
    SLANG_PUBLIC float zNear;                      // view-space, positive
    SLANG_PUBLIC float zFar;                       // view-space, positive
    SLANG_PUBLIC uint  spotLightCount;             // active entries in gSpotLights[]
    SLANG_PUBLIC uint  fogAreaCount;               // active entries in gFogAreas[]
    SLANG_PUBLIC uint  decalCount;                 // active entries in gDecals[]

    SLANG_PUBLIC float3 posW;
    SLANG_PUBLIC float jitterX;
    SLANG_PUBLIC float3 cameraU;
    SLANG_PUBLIC float jitterY;
    SLANG_PUBLIC float3 cameraV;
    SLANG_PUBLIC float3 cameraW;
};
HOST_NAMESPACE_END
