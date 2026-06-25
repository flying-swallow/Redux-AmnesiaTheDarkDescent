#pragma once

// Slang-side ILight interface over the host/device-shared light structs declared
// in SceneTypes.slang. Conformances are added via `extension` so the shared
// header stays a pure data-layout file (consumed by both C++ and shader).
//
// Light type is determined by which SSBO an entry lives in (point/spot/area) and
// the per-type count boundaries in gPerFrame — see Scene.slang getLight() — so
// the structs carry no explicit type discriminator.
//
// Usage example:
//   void shadeAny<L : ILight>(L light, float3 worldPos) {
//       LightSample ls;
//       if (light.sample(worldPos, ls)) { ... }
//   }

#include "HostDefinitions.h"
import SceneTypes;            // light struct fields + kInvalidTextureIndex sentinel
import bindless;             // gTextures2D, gTexturesCube, gMaterialSampler
import Random;               // concentricDiskSample (soft-shadow source sampling)

// -----------------------------------------------------------------------------
// ILight — common shape for the point/spot/box bindless light SSBO entries.
// `getPosition()` returns the source for point/spot, the AABB
// center for box. `sample()` is the NEE entry point used by the surfel ray
// pipeline and any future direct-lighting code.
// -----------------------------------------------------------------------------
public interface ILight
{
    public float3 getColor();
    public float3 getPosition();
    public float getRadius();         // light-grid bin reach (world units); 0 => skip
    public bool sample(float3 shadingPosW, out LightSample ls);

    // Whether this light casts shadows (GetCastShadows). When false the NEE
    // shadow ray is skipped — the light fully lights its targets, no occlusion.
    public bool castsShadow();

    // Jittered point on the emitter for soft-shadow rays. `dirToLight` is the unit
    // direction from the shading point to the light (used to orient a perpendicular
    // disk for spherical point/spot sources; area lights ignore it and sample their
    // own plane). `u` is a uniform float2 in [0,1). Returns the light center when
    // the source is degenerate (point/spot with sourceRadius 0) => hard shadow.
    public float3 sampleSourcePoint(float3 dirToLight, float2 u);
};

// Jittered point on a spherical source of `radius` at `center`, on a disk facing
// the shading point (perpendicular to `dirToLight`). radius <= 0 => the center
// (hard shadow). Shared by point and spot.
float3 diskSourcePoint(float3 center, float radius, float3 dirToLight, float2 u)
{
    if (radius <= 0.0) return center;
    float3 t = normalize(abs(dirToLight.y) < 0.99f
                         ? cross(float3(0.f, 1.f, 0.f), dirToLight)
                         : cross(float3(1.f, 0.f, 0.f), dirToLight));
    float3 b = cross(dirToLight, t);
    float2 d = concentricDiskSample(u) * radius;
    return center + d.x * t + d.y * b;
}

// One-sided barn-door occlusion for a RectLight, evaluated per tangential axis.
// `coord` is the shading point's rect-local tangential coordinate (lx or ly),
// `halfExt` the rect half-extent on that axis, `lz` the depth along the emission
// normal (> 0). The flaps hinge on the rect edges, tilt from the normal by the
// barn-door angle θ (sinA/cosA), and extend `L` world units. Their tips form a
// smaller exit aperture; light is confined to the frustum from the source rect
// through that aperture. Returns 1 fully lit … 0 fully shadowed.
//   xin  = near-source-edge graze (depends only on θ): everything inside is lit.
//   xout = far-source-edge graze over the flap tip (depends on L): full shadow beyond.
// L = 0 or θ → 90° (cosA → 0) means the flaps are open → no-op (returns 1).
float barnAxis(float coord, float halfExt, float lz, float sinA, float cosA, float L)
{
    float a = abs(coord);
    if (a <= halfExt) return 1.0;                          // over the rect → lit
    if (L <= 0.0 || cosA <= 1.0e-4) return 1.0;            // no flap / fully open
    float xin  = halfExt + lz * (sinA / cosA);             // hw + lz·tanθ
    float xout = -halfExt + lz * (2.0 * halfExt + L * sinA) / (L * cosA);
    return saturate((xout - a) / max(xout - xin, 1.0e-4));
}

public extension PointLight : ILight
{
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }
    public float getRadius()       { return radius; }
    public bool castsShadow()   { return shadowEnabled != 0u; }
    public float3 sampleSourcePoint(float3 dirToLight, float2 u)
    {
        return diskSourcePoint(position, sourceRadius, dirToLight, u);
    }

    // Radiance = color · intensity · softened inverse-square. The attenuation is a
    // plain inverse-square (no reach window) so radiance goes HDR (>1) near the
    // source and blooms; the reach cutoff lives only in the grid binning, not here.
    public bool sample(float3 shadingPosW, out LightSample ls)
    {
        ls.Li = float3(0.0); ls.pdf = 1.0; ls.dir = float3(0,0,1); ls.distance = 0.0;
        float3 toLight = position - shadingPosW;
        float  dist    = length(toLight);
        if (dist <= 0.0) return false;
        float3 L      = toLight / dist;
        float  d2     = dist * dist;
        float  atten  = 1.0 / (d2 + kPointLightSourceRadiusSq);            // softened inverse-square (HDR near source)

        float3 gobo = float3(1.0);
        if (goboTextureIndex != kInvalidTextureIndex)
        {
            float3x3 wToL = float3x3(worldToLightX, worldToLightY, worldToLightZ);
            float3 lightLocalDir = mul(wToL, -L);
            gobo = gTexturesCube[NonUniformResourceIndex(goboTextureIndex)]
                .SampleLevel(gMaterialSampler, lightLocalDir, 0.0).rgb;
        }

        ls.Li       = color * intensity * gobo * atten;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};

public extension SpotLight : ILight
{
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }
    public float getRadius()       { return radius; }
    public bool castsShadow()   { return shadowEnabled != 0u; }
    public float3 sampleSourcePoint(float3 dirToLight, float2 u)
    {
        return diskSourcePoint(position, sourceRadius, dirToLight, u);
    }

    // Radiance = color · intensity · softened inverse-square (same radial term as
    // PointLight — plain inverse-square, HDR near source, no reach window; the
    // cutoff is grid-only), times an angular cone term (analytic smoothstep from
    // the FOV edge to the center, or a projected gobo via viewProjection when bound).
    public bool sample(float3 shadingPosW, out LightSample ls)
    {
        ls.Li = float3(0.0); ls.pdf = 1.0; ls.dir = float3(0,0,1); ls.distance = 0.0;
        float3 toLight = position - shadingPosW;
        float  dist    = length(toLight);
        if (dist <= 0.0) return false;
        float3 L      = toLight / dist;
        float  d2     = dist * dist;
        float  atten  = 1.0 / (d2 + kPointLightSourceRadiusSq);            // softened inverse-square (HDR near source)

        float3 gobo = float3(1.0);
        float  cone = 1.0;
        if (goboTextureIndex != kInvalidTextureIndex)
        {
            float4 lc = mul(viewProjection, float4(shadingPosW, 1.0));
            if (lc.w <= 0.0) return false;
            float2 uv = lc.xy / lc.w;
            if (any(uv < float2(0.0)) || any(uv > float2(1.0))) return false;
            gobo = sampleBindless2DLevel(goboTextureIndex, uv, 0.0).rgb;
        }
        else
        {
            float cosTheta = dot(-L, direction);
            if (cosTheta < cosOuterAngle) return false;
            cone = smoothstep(cosOuterAngle, 1.0, cosTheta);
        }

        ls.Li       = color * intensity * gobo * cone * atten;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};

public extension RectLight : ILight
{
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }
    public float getRadius()       { return radius; }
    public bool castsShadow()   { return shadowEnabled != 0u; }
    // Soft shadows sample the actual rectangle: a uniform point across the
    // width×height plane (right/up axes). The area itself IS the source, so there
    // is no disk radius — a larger source rectangle casts a softer penumbra.
    public float3 sampleSourcePoint(float3 dirToLight, float2 u)
    {
        return position + (u.x - 0.5) * width * right + (u.y - 0.5) * height * up;
    }

    // Analytic closest-point-on-rectangle shading (UE Rect Light model). The
    // emitter is a width×height rect at `position`, framed by (right, up, normal).
    // We shade the closest point on the rect as a softened inverse-square source,
    // windowed to 0 at the attenuation `radius`. Intensity is the TOTAL energy
    // averaged over the surface (radiance = intensity / area), so a bigger rect is
    // dimmer per unit area. An optional `sourceTextureIndex` tints the emission
    // (rect-local UV; does NOT affect shadows — those sample the area in
    // sampleSourcePoint). One-sided: only the +normal hemisphere is lit. pdf = 1.
    // Barn doors (barnDoorAngle θ, barnDoorLength L) confine the beam: four flaps hinged on the
    // rect edges, tilted from the normal by θ and L units long, form a smaller exit aperture that
    // occludes off-axis light (see barnAxis). θ → 90° or L = 0 ⇒ flaps open ⇒ no-op.
    public bool sample(float3 shadingPosW, out LightSample ls)
    {
        ls.Li = float3(0.0); ls.pdf = 1.0; ls.dir = float3(0,0,1); ls.distance = 0.0;

        // Shading point in the rect's local frame.
        float3 d  = shadingPosW - position;
        float  lx = dot(d, right);
        float  ly = dot(d, up);
        float  lz = dot(d, normal);
        if (lz <= 0.0) return false;                       // behind the emitter

        float hw = width  * 0.5;
        float hh = height * 0.5;

        // Closest point on the rect: clamp the tangential coords to the extent.
        float  cx = clamp(lx, -hw, hw);
        float  cy = clamp(ly, -hh, hh);
        float3 Q  = position + right * cx + up * cy;

        float3 toLight = Q - shadingPosW;
        float  dist    = length(toLight);
        if (dist <= 0.0) return false;
        float3 L     = toLight / dist;
        float  d2    = dist * dist;

        // Softened inverse-square, windowed to 0 at the attenuation radius
        // (UE-style (1 - (d/r)^4)^2 bound on visible influence).
        float window = 1.0;
        if (radius > 0.0)
        {
            float x  = dist / radius;
            float x4 = (x * x) * (x * x);
            float w  = saturate(1.0 - x4);
            window   = w * w;
        }
        float atten = window / (d2 + kPointLightSourceRadiusSq);

        // Intensity is averaged over the emitter surface (UE semantics).
        float area = max(width * height, 1.0e-4);

        float3 srcTex = float3(1.0);
        if (sourceTextureIndex != kInvalidTextureIndex)
        {
            float2 uv = float2(cx / width + 0.5, 0.5 - cy / height);
            srcTex = sampleBindless2DLevel(sourceTextureIndex, uv, 0.0).rgb;
        }

        // Barn-door beam shaping: occlude off-axis light per tangential axis.
        float sinA, cosA;
        sincos(barnDoorAngle, sinA, cosA);
        float barn = barnAxis(lx, hw, lz, sinA, cosA, barnDoorLength)
                   * barnAxis(ly, hh, lz, sinA, cosA, barnDoorLength);

        ls.Li       = color * (intensity / area) * srcTex * atten * barn;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};
