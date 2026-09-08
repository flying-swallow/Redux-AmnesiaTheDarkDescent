#ifndef HPL_RAY_CONE_LOD_H
#define HPL_RAY_CONE_LOD_H

#include <cstdint>

namespace hpl {

// Primary ray-cone spread from the vertical field of view and render height.
float RayConePrimarySpreadAngle(float verticalFovRadians,
                                uint32_t renderHeight);

// Host mirror of RayCone.slang::kRayConeMaxSpreadAngle. Keep this value in
// sync with the shader; beyond a hemisphere the linear width propagation has
// no physical meaning, and an unbounded rough-bounce spread would force every
// later hit to the smallest mip.
constexpr float kRayConeMaxSpreadAngle = 1.5707963267948966f;

// ggxAlpha is SurfaceBRDF::rough, a GGX alpha already floored to [0.04, 1]
// by Brdf.slang. A specular lobe with alpha a spreads by 2*atan(a) after
// reflection about its half-vector: the half-vector deviation is about
// atan(a), and reflection doubles it. A diffuse bounce scatters over the full
// hemisphere; its cosine-weighted mean polar angle is pi/4, giving the same
// pi/2 full cone angle as alpha 1. The two lobes therefore meet at the fully
// rough end, and the term is monotonic in roughness across both.
// The non-specular branch returns the full diffuse spread,
// independently of ggxAlpha; out-of-range alpha is clamped before the
// specular form is evaluated.
float RayConeSurfaceSpreadAngle(float ggxAlpha, bool sampledSpecular);

// Measured GGX-equivalent alpha for the water reflection-normal variation
// inside one half-resolution guide texel. This is a measurement rather than
// a proxy constant: it derives the spread from the mean resultant length of
// the valid full-resolution unit reflection normals in that texel. It can
// only see variation across those raster samples, however, so it is a lower
// bound on the true sub-texel wave-normal variation.
float WaterGuideNormalSpreadAlpha(const float normalSum[3], uint32_t count);

// The guide stores a GGX alpha, while NRD's front-end packer takes LINEAR
// roughness. Use linearRoughness = sqrt(alpha), matching the convention
// already used by the opaque path in PathTracer/NrdPack.cs.slang. Non-finite
// input is clamped to zero and out-of-range alpha is clamped to [0, 1], so a
// malformed guide packs roughness 0 (a mirror) rather than garbage.
float WaterNrdLinearRoughness(float spreadAlpha);

// Ray-cone width at a hit distance, including the initial footprint width.
float RayConeWidthAt(float spreadAngle, float startWidth, float hitDistance);

// Add the angular spread contributed by a surface bounce to the propagated
// cone. Negative surface spreads are treated as zero so malformed inputs do
// not shrink an existing cone; the result is capped at the largest meaningful
// hemisphere-sized spread above.
float RayConeWidenSpreadAngle(float spreadAngle, float surfaceSpreadAngle);

// The triangle's UV-to-world-area contribution to the texture LOD.
float RayConeTriangleLodConstant(float uvArea, float worldArea);

// Complete ray-cone texture LOD. The cosine is floored because the grazing
// boost would otherwise grow without bound at silhouettes; kRayConeMinCosine
// caps it at about 4.3 mips. The +0.5*log2(textureWidth*textureHeight) term
// is the part split between rayConeTexLodBase and sampleBindless2DCone on the
// Slang side.
//
// The result is intentionally not clamped to zero. Negative LODs are clamped
// by sampler hardware, which preserves the intended materialMipBias behavior
// at the top mip.
float RayConeTextureLod(float triangleLodConstant, float coneWidth,
                        float normalDotRayDir, float textureWidth,
                        float textureHeight, float materialMipBias);

// Host mirror of RayCone.slang::kTexLodTopMipBase. Keep this value in sync
// with the shader; return it when no texture footprint can be derived. This
// named escape hatch is not the same as a zero LOD base.
constexpr float kTexLodTopMipBase = -64.0f;

// World-space diameter of the cone footprint on the surface it hits. The
// cosine floor elongates the cone disk into the surface tangent plane, matching
// the grazing term folded into RayConeTextureLod.
float RayConeSurfaceFootprint(float coneWidth, float normalDotRayDir);

// Build the row-major 2x2 Jacobian of one sequential water-wave UV update.
// The inputs mirror the shader directly; modifiedUvX is needed by the second
// sine because the y update uses the already-modified x coordinate.
void WaterWaveUvJacobian(float effectiveAmplitude, float effectiveFrequency,
                         float phase, float v, float modifiedUvX,
                         float outJacobian[4]);

// Apply a water-wave UV Jacobian to one screen-space UV gradient pair. Call
// this once for each of ddx(baseUv) and ddy(baseUv) before SampleGrad.
void WaterWaveUvGradient(const float jacobian[4],
                         const float baseUvGradient[2],
                         float outSampleUvGradient[2]);

// UV units per world unit for a projected gobo at a world-space position.
// rowX, rowY and rowW are rows 0, 1 and 3 of the world-to-gobo-UV matrix;
// clip.xy/clip.w is already in [0,1] gobo UV, with no 0.5 remap.
// The maximum row length is conservative: the footprint is a disk with no
// preferred direction in this frame, so selecting the larger axis rate picks
// the blurrier mip instead of aliasing on the tighter axis. The projective
// derivative includes both distance through 1/clipW and lateral obliquity.
float ProjectedGoboUvPerWorldUnit(const float rowX[4], const float rowY[4],
                                  const float rowW[4], const float posW[3]);

// Convert a surface footprint and projected UV rate into a texture-size-
// independent gobo LOD base. The product is the gobo-UV diameter of the
// surface patch; the degenerate return is the named top-mip escape hatch,
// not zero.
float ProjectedGoboTexLodBase(float surfaceFootprintW, float uvPerWorldUnit);

// [0,1] cube-face UV units per radian of subtended angle, for a direction
// into the cube map. dir need not be normalised. A cube face is a gnomonic
// projection, so the rate is 0.5 at the face centre and grows toward the
// corners; the conservative radial 1/cos^2 rate is used, matching the
// max-of-Jacobian-lengths choice above. The Slang side computes this inline
// inside cubeGoboTexLodBase.
float CubeFaceUvPerRadian(const float dir[3]);

// Pure host mirror of RayCone.slang::cubeGoboTexLodBase. The surface footprint
// is a world-space DIAMETER. Its subtended-angle approximation conservatively
// ignores foreshortening of the surface patch relative to the light direction;
// the degenerate return is the named top-mip escape hatch, not zero.
float CubeGoboTexLodBase(float surfaceFootprintW, float distanceToLight,
                         const float dir[3]);

// UV units per world unit for a projected decal atlas cell. rowX and rowZ are
// rows 0 and 2 of the affine world-to-box transform; only their xyz parts
// participate in the derivative. The maximum row rate is conservative because
// the footprint is a disk with no preferred direction, so the blurrier axis
// avoids aliasing on the tighter axis. The atlas subdivision scales each row
// by its cell count, and the V flip does not change the rate magnitude.
float DecalUvPerWorldUnit(const float rowX[4], const float rowZ[4],
                          uint32_t subDivX, uint32_t subDivY);

// Texture-size-independent atlas-bleed cap for a projected decal. This cannot
// reuse the surface cone: the decal samples projected atlas coordinates, and
// a mip larger than one atlas cell averages neighbouring cells. The bound is
// -log2(max(subDivX, subDivY)); a 1x1 grid clamps at 0.0f, which is the 1x1 mip
// under this lodBase convention and therefore is not a restriction.
float DecalAtlasMaxTexLodBase(uint32_t subDivX, uint32_t subDivY);

// Convert a surface footprint and projected decal UV rate into a
// texture-size-independent LOD base. The product is the decal-UV diameter of
// the shading patch; the atlas cap prevents filtering across neighbouring
// cells. The degenerate return is the named top-mip escape hatch, not zero.
float DecalTexLodBase(float surfaceFootprintW, float uvPerWorldUnit,
                      uint32_t subDivX, uint32_t subDivY);

// Texture-size-independent LOD base for a rect area light's source texture.
// The sampled point is on the light, so the surface footprint slides the
// closest point across the emitter while the emitter's subtended extent also
// governs the source footprint. The closest-point model replaces an area
// integral with one sample; emitter points farther than about the light
// distance tangentially are already dominated by inverse-square falloff, so
// averaging over that distance lets distant lights converge to the mean of
// their source texture. The larger footprint is conservative.
//
// The smaller rect extent is the divisor because the completing sampler term
// is 0.5*log2(width*height), which assumes one isotropic UV rate; using the
// smaller world extent is the conservative axis. Values above one are
// intentional and resolve past the last mip, where the sampler clamps.
float AreaLightSourceTexLodBase(float surfaceFootprintW, float distanceToLight,
                                float rectWidth, float rectHeight);

} // namespace hpl

#endif // HPL_RAY_CONE_LOD_H
