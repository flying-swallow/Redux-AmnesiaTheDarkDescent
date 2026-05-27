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
// Scalar-layout point light. `type` discriminator at offset 0; remaining
// fields match the legacy host upload (HybridRenderer.cpp's point-light loop).
// `attenuationTextureIndex` points into textures_2d[] at the light's 1D-as-2D
// falloff LUT keyed on (d/r)²; kInvalidTextureIndex triggers the analytic
// saturate(1-(d/r)²) fallback in sampleAttenuation (light_falloff.glsl).
// `intensity` mirrors the base-game `lightColor.w` specular flag/multiplier
// (cColor.a) — used only on the specular term, not on diffuse.
// `goboTextureIndex` points into textures_cube[] (set 0, binding 1); when
// set, sample the cube in light-local space using the worldToLight* rows.
// worldToLight{X,Y,Z} are the rows of the light's world rotation (R), which
// reconstruct R^T via `float3x3(worldToLightX, worldToLightY, worldToLightZ)`.
// -----------------------------------------------------------------------------
SLANG_PUBLIC struct PointLight {
    SLANG_PUBLIC uint  type;                         // LIGHT_TYPE_POINT
    SLANG_PUBLIC uint  attenuationTextureIndex;
    SLANG_PUBLIC uint  goboTextureIndex;
    SLANG_PUBLIC float intensity;
    SLANG_PUBLIC float3  position;
    SLANG_PUBLIC float radius;
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC float3  worldToLightX;
    SLANG_PUBLIC float3  worldToLightY;
    SLANG_PUBLIC float3  worldToLightZ;
};

// Scalar-layout spot light. Adds cone forward + pre-baked cos(angle/2),
// optional gobo + cone-falloff LUT slots, shadow bit, and the light's
// view-projection matrix (gobo / future shadow-map UVs). attenuationTextureIndex
// is the legacy radial LUT (d/r)²; coneFalloffTextureIndex is the legacy
// 1D cone-falloff LUT keyed on (1-cosTheta)/(1-cosOuterAngle). Both LUT slots
// follow the kInvalidTextureIndex → analytic fallback convention.
SLANG_PUBLIC struct SpotLight {
    SLANG_PUBLIC uint  type;                         // LIGHT_TYPE_SPOT
    SLANG_PUBLIC uint  attenuationTextureIndex;
    SLANG_PUBLIC uint  goboTextureIndex;
    SLANG_PUBLIC uint  coneFalloffTextureIndex;
    SLANG_PUBLIC float3  position;
    SLANG_PUBLIC float radius;
    SLANG_PUBLIC float3  direction;                    // world-space outward forward
    SLANG_PUBLIC float cosOuterAngle;                // cos(GetFOV() * 0.5)
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC float intensity;
    SLANG_PUBLIC uint  shadowEnabled;                // 0 or 1
    SLANG_PUBLIC float4x4  viewProjection;               // light-space ViewProj for gobo UVs
};

// Scalar-layout box (volumetric) light. World-space AABB centered on the
// light's world position, matching RendererDeferred.cpp's box-light proxy
// (Scale × Translate, no rotation). Pixel is "inside" when
// abs(worldPos - center) <= halfSize component-wise.
// Color is the legacy `cColor.rgb`; alpha is intentionally not stored because
// the legacy box shader discards it (deferred_light_box.frag.fsl). Add-blend
// only for now (eLightBoxBlendFunc_Replace is treated as Add on the C++ side).
SLANG_PUBLIC struct BoxLight {
    SLANG_PUBLIC uint  type;                         // LIGHT_TYPE_BOX
    SLANG_PUBLIC uint  blendFunc;                    // 0 = Replace, 1 = Add
    SLANG_PUBLIC float3  center;                       // world-space box center
    SLANG_PUBLIC float3  halfSize;                     // world-space AABB half-extents
    SLANG_PUBLIC float3  color;
    SLANG_PUBLIC float3  worldToLightX;
    SLANG_PUBLIC float3  worldToLightY;
    SLANG_PUBLIC float3  worldToLightZ;
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
    SLANG_PUBLIC float heightMapScale;
    SLANG_PUBLIC float heightMapBias;
    SLANG_PUBLIC float frenselBias;
    SLANG_PUBLIC float frenselPow;
};

// GPU twin of MaterialTranslucent (Material.h). All bool flags
// (isAffectedByLightLevel, hasRefraction, refractionEdgeCheck,
// refractionNormals) and the eMaterialBlendMode pack into materialConfig.
SLANG_PUBLIC struct TranslucentMaterial {
    SLANG_PUBLIC uint  type;                         // MATERIAL_TYPE_TRANSLUCENT
    SLANG_PUBLIC uint  materialConfig;
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
    SLANG_PUBLIC uint  boxLightCount;              // active entries in gBoxLights[]
    SLANG_PUBLIC uint  diffuseMaterialCount;       // active entries in gDiffuseMaterials[]
    SLANG_PUBLIC uint  translucentMaterialCount;   // active entries in gTranslucentMaterials[]
    SLANG_PUBLIC uint  waterMaterialCount;         // active entries in gWaterMaterials[]
    SLANG_PUBLIC uint  decalMaterialCount;         // active entries in gDecalMaterials[]

    SLANG_PUBLIC float3 posW;
    SLANG_PUBLIC float jitterX;
    SLANG_PUBLIC float3 cameraU;
    SLANG_PUBLIC float jitterY;
    SLANG_PUBLIC float3 cameraV;
    SLANG_PUBLIC float3 cameraW;
};
HOST_NAMESPACE_END
