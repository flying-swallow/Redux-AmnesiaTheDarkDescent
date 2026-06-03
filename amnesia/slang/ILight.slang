#pragma once

// Slang-side ILight interface over the host/device-shared light structs declared
// in SceneTypes.slang. Conformances are added via `extension` so the shared
// header stays a pure data-layout file (consumed by both C++ and shader).
//
// Each light struct stores its type discriminator in `type` at offset 0 — the
// contract documented in SceneTypes.slang. The interface methods assume that
// invariant; do not reorder fields.
//
// Usage example:
//   void shadeAny<L : ILight>(L light, float3 worldPos) {
//       LightSample ls;
//       if (light.sample(worldPos, ls)) { ... }
//   }

#include "HostDefinitions.h"
import SceneTypes;            // light struct fields + kInvalidTextureIndex sentinel
import bindless;             // gTextures2D, gTexturesCube, gMaterialSampler
import LightFalloff;         // sampleAttenuation, sampleSpotCone

// -----------------------------------------------------------------------------
// ILight — common shape for the point/spot/box bindless light SSBO entries.
// `getType()` returns the LIGHT_TYPE_* discriminator (always reads `type` at
// offset 0). `getPosition()` returns the source for point/spot, the AABB
// center for box. `sample()` is the NEE entry point used by the surfel ray
// pipeline and any future direct-lighting code.
// -----------------------------------------------------------------------------
public interface ILight
{
    public uint getType();
    public float3 getColor();
    public float3 getPosition();
    public bool sample(float3 shadingPosW, out LightSample ls);
};

public extension PointLight : ILight
{
    public uint getType()     { return type; }
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }

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
    public uint getType()     { return type; }
    public float3 getColor()    { return color; }
    public float3 getPosition() { return position; }

    // Radiance = color · intensity · softened inverse-square (same radial term as
    // PointLight — plain inverse-square, HDR near source, no reach window; the
    // cutoff is grid-only), times an angular cone term (cone LUT/smoothstep, or a
    // projected gobo via viewProjection when bound).
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
            gobo = gTextures2D[NonUniformResourceIndex(goboTextureIndex)]
                .SampleLevel(gMaterialSampler, uv, 0.0).rgb;
        }
        else
        {
            float cosTheta = dot(-L, direction);
            if (cosTheta < cosOuterAngle) return false;
            cone = sampleSpotCone(cosTheta, cosOuterAngle);
        }

        ls.Li       = color * intensity * gobo * cone * atten;
        ls.dir      = L;
        ls.distance = dist;
        return true;
    }
};
