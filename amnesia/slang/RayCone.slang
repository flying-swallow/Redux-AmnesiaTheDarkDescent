#pragma once

// RayCone.slang — texture-footprint helpers for ray-traced material lookup.
//
// A ray has a finite footprint even when it is represented by a single
// mathematical line.  Ray cones model that footprint as a width at a hit and
// a spread angle from the camera.  This is the Akenine-Möller et al. method
// from the JCGT 2021 paper "Improved Shader and Texture Level of Detail Using
// Ray Cones".  Keeping the cone math in this small module lets every
// ray-traced material path use the same estimate without importing Scene or
// bindless (which would introduce an import cycle).
//
// The triangle term is deliberately expressed as a ratio of UV area to world
// area.  World-space positions are required: applying an instance scale must
// change the footprint-to-texture ratio.  The texture-dimension term is also
// deliberately factored out, so one lod base can be reused for every texture
// sampled at a hit.  Consumers turn it into a full texture LOD as follows:
//
//   full lod = base + 0.5 * log2(texWidth * texHeight) + materialMipBias
//
// `materialMipBias` is applied by the bindless sampler helper, where the
// concrete texture dimensions are available.

import PerFrame.resource;

public struct RayCone
{
    public float width;
    public float spreadAngle;
};

// Explicit texture-size-independent base for the top mip. Even the largest
// plausible texture contributes only about +14 through 0.5*log2(w*h), so this
// always resolves below 0 and the sampler clamps to mip 0. Call sites without
// a ray cone must use this named value instead of a misleading literal.
public static const float kTexLodTopMipBase = -64.0f;

// At the primary camera plane the cone has zero width.  The angular spread is
// derived from the vertical field of view and the actual render extent rather
// than the display extent.  A reduced temporal-upscaling extent therefore
// produces the appropriately wider cone.
public RayCone rayConePrimary()
{
    RayCone c;
    c.width       = 0.0f;
    c.spreadAngle = atan(2.0f * tan(0.5f * gPerFrame.cameraFov)
                         / max(gPerFrame.viewportSize.y, 1.0f));
    return c;
}

// Propagate the cone to a surface hit.  `hitT` is the distance along the ray
// in world units; the width grows linearly with the cone's angular spread.
public RayCone rayConePropagate(RayCone c, float hitT)
{
    c.width += c.spreadAngle * hitT;
    return c;
}

// Maximum accumulated cone spread angle. Beyond a hemisphere the linear
// `width += spreadAngle * hitT` propagation has no meaning, so the accumulated
// spread saturates here instead of growing without bound over several rough
// bounces. An unbounded spread on a rough first bounce would drive every
// subsequent hit to the smallest mip.
public static const float kRayConeMaxSpreadAngle = 1.5707963267948966f; // pi/2

// Extra angular spread a scattering event adds to the cone. This is the
// surface-spread term the JCGT 2021 paper derives from curvature; the
// roughness-driven form used here needs no curvature estimate.
//
// `ggxAlpha` is SurfaceBRDF::rough. A GGX lobe of alpha `a` perturbs the
// half-vector by about atan(a), and reflecting about that half-vector doubles
// the deviation, so the specular surface spread is 2*atan(a): zero for a
// perfect mirror, pi/2 at a = 1.
//
// A diffuse bounce instead scatters over the whole hemisphere. For a
// cosine-weighted hemisphere the mean polar angle is pi/4, so the full cone
// angle is pi/2 — which is exactly the a = 1 limit of the specular form. The
// two lobes therefore meet at the fully rough end, and the term is monotonic
// in roughness across both.
public float rayConeSurfaceSpreadAngle(float ggxAlpha, bool sampledSpecular)
{
    if (!sampledSpecular)
        return kRayConeMaxSpreadAngle;
    return 2.0f * atan(clamp(ggxAlpha, 0.0f, 1.0f));
}

// Widen a cone's spread by one scattering event's surface spread. Monotonic
// and bounded, and the max() means a degenerate input cannot shrink a cone
// that has already widened.
public RayCone rayConeWiden(RayCone c, float surfaceSpreadAngle)
{
    c.spreadAngle = min(c.spreadAngle + max(surfaceSpreadAngle, 0.0f),
                        kRayConeMaxSpreadAngle);
    return c;
}

// Return the signed 2D cross product (the doubled area of a UV triangle).
float cross2(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

// Return the texture-size-independent triangle LOD constant.  `p0W`/`p1W`/
// `p2W` MUST be world-space positions so instance scaling affects the ratio.
// Degenerate UV or world triangles return zero; this preserves the current
// top-mip-ish behavior without producing an infinity or NaN.
public float triangleTexLodConstant(float2 uv0, float2 uv1, float2 uv2,
                                    float3 p0W, float3 p1W, float3 p2W)
{
    float uvArea    = abs(cross2(uv1 - uv0, uv2 - uv0));
    float worldArea = length(cross(p1W - p0W, p2W - p0W));
    if (uvArea <= 0.0f || worldArea <= 0.0f)
        return 0.0f;
    return 0.5f * log2(uvArea / worldArea);
}

// Minimum cosine for the grazing-angle correction.  The floor caps the
// silhouette boost at about 4.3 mips instead of allowing it to diverge when
// the ray is perpendicular to the surface normal.
static const float kRayConeMinCosine = 0.05f;

// Return the texture-size-independent LOD base.  The full LOD is
// `base + 0.5*log2(texWidth*texHeight) + materialMipBias`; the bindless
// sampler adds those texture-specific terms when it resolves a material map.
public float rayConeTexLodBase(RayCone c, float triLodConstant,
                               float3 normalW, float3 rayDir)
{
    return triLodConstant
         + log2(max(abs(c.width), 1e-8f))
         - log2(max(abs(dot(normalize(rayDir), normalize(normalW))),
                    kRayConeMinCosine));
}

// Return the world-space DIAMETER of the cone footprint in the surface tangent
// plane. This is the same elongation term that rayConeTexLodBase folds in,
// exposed on its own because a light-projected texture needs the world footprint
// before its own UV mapping is applied.
public float rayConeSurfaceFootprint(RayCone c, float3 normalW, float3 rayDir)
{
    return abs(c.width) / max(abs(dot(normalize(rayDir), normalize(normalW))),
                              kRayConeMinCosine);
}

// Return the texture-size-independent LOD base for an affine projected decal.
// projectDecal builds its UV from box-local x and z, so the UV-per-world rates
// are rows 0 and 2 of invModelMat divided by their cell counts (Slang mul(M,v)
// is row-major-by-row, as projectedTexLodBase already documents). The transform
// is affine, so the rate is constant over the box and there is no 1/w term as
// in the gobo case; the V flip does not change the magnitude.
// The decal atlas maps one box cell to 1/sx by 1/sy of the texture, so a mip
// whose texel exceeds a cell can pull in a neighboring cell. Clamp the base to
// -log2(max(sx,sy)) to prevent that atlas bleed. The sampler's completing term
// is isotropic, so this bound is texture-size-independent; a 1x1 grid clamps
// at 0, the 1x1 mip, and therefore imposes no restriction. The max of the two
// axis rates is conservative for the same reason as projectedTexLodBase.
public float decalTexLodBase(float4x4 invModelMat, uint subDivX, uint subDivY,
                             float surfaceFootprintW)
{
    uint sx = max(subDivX, 1u);
    uint sy = max(subDivY, 1u);
    float uvPerWorld = max(length(invModelMat[0].xyz) / float(sx),
                           length(invModelMat[2].xyz) / float(sy));
    if (!(surfaceFootprintW > 0.0f) || !(uvPerWorld > 0.0f))
        return kTexLodTopMipBase;

    float maxBase = -log2(float(max(sx, sy)));
    return min(log2(surfaceFootprintW * uvPerWorld), maxBase);
}

// Convert a world-space footprint diameter into the texture-size-independent LOD
// base for a projected light texture. `clip` is the already-computed
// mul(viewProjection, float4(posW, 1)) result, so callers do not transform twice.
// Slang `mul(M, v)` is row-major-by-row: viewProjection[0] produces clip.x and
// viewProjection[3] produces clip.w. jx/jy are the projective Jacobian rows
// d(uv)/d(worldPos). The max-of-lengths is the conservative isotropic rate for a
// footprint disk with no preferred direction. The 1/clip.w carries distance from
// the light and uv*rowW carries lateral obliquity, so no separate distance or FOV
// term is needed.
public float projectedTexLodBase(float4x4 viewProjection, float4 clip,
                                 float surfaceFootprintW)
{
    if (!(clip.w > 0.0f) || !(surfaceFootprintW > 0.0f))
        return kTexLodTopMipBase;

    float2 uv = clip.xy / clip.w;
    float3 jx = (viewProjection[0].xyz - uv.x * viewProjection[3].xyz) / clip.w;
    float3 jy = (viewProjection[1].xyz - uv.y * viewProjection[3].xyz) / clip.w;
    float uvPerWorld = max(length(jx), length(jy));
    if (!(uvPerWorld > 0.0f))
        return kTexLodTopMipBase;
    return log2(surfaceFootprintW * uvPerWorld);
}

// Return the texture-size-independent LOD base for the closest-point sample of
// an area-light source texture. The sampled point lies on the LIGHT, not on the
// shading surface: two footprints compete and the larger governs. The shading
// point's footprint slides the closest point across the emitter, while the
// emitter extent the point effectively subtends is also a footprint. The
// closest-point model replaces an area integral by one sample, and emitter
// points farther than about distToLight tangentially are already suppressed by
// inverse-square falloff, so the texture should average over roughly
// distToLight world units of emitter. This makes a distant rect converge to the
// mean of its source texture instead of an arbitrary texel. min(width,height) is
// the divisor because the sampler's completing term assumes one isotropic rate.
public float areaLightSourceTexLodBase(float surfaceFootprintW, float distToLight,
                                       float width, float height)
{
    float minExtent = min(width, height);
    if (!(minExtent > 0.0f))
        return kTexLodTopMipBase;
    float f = max(max(surfaceFootprintW, 0.0f), max(distToLight, 0.0f)) / minExtent;
    if (!(f > 0.0f))
        return kTexLodTopMipBase;
    return log2(f);
}

// Return the texture-size-independent LOD base for a point-light cube gobo. The
// surface footprint diameter at distance distToLight subtends about
// surfaceFootprintW / distToLight radians as seen from the light. This ignores
// foreshortening of the surface patch relative to the light direction, so the
// angle is conservative (larger angle => blurrier mip).
//
// Cube faces use a gnomonic projection. The angular, rather than projective,
// face-UV rate is 0.5 face-UV per radian at the face centre and grows toward
// the corners: with face coordinates in [-1,1], u = tan(theta), and the
// conservative radial Jacobian is 1/cos^2(c), versus 1/cos(c) tangentially,
// so the radial scale is the larger one. A face spans 90 degrees over 1.0 UV,
// but tan() makes the centre rate lower than the average 1/(pi/2), hence 0.5
// rather than 1/(pi/2). For direction d, the larger radial scale gives
// uvPerRadian = 0.5 * dot(d,d) / (m*m), where m is the largest absolute
// component; this also works for unnormalised directions. The rate is 0.5 at a
// face centre and 0.5*3 = 1.5 at a face corner.
public float cubeGoboTexLodBase(float surfaceFootprintW, float distToLight,
                                float3 lightLocalDir)
{
    float m = max(abs(lightLocalDir.x),
                  max(abs(lightLocalDir.y), abs(lightLocalDir.z)));
    if (!(surfaceFootprintW > 0.0f) || !(distToLight > 0.0f) || !(m > 0.0f))
        return kTexLodTopMipBase;

    float uvPerRadian = 0.5f * dot(lightLocalDir, lightLocalDir) / (m * m);
    float uvFootprint = (surfaceFootprintW / distToLight) * uvPerRadian;
    if (!(uvFootprint > 0.0f))
        return kTexLodTopMipBase;
    return log2(uvFootprint);
}
