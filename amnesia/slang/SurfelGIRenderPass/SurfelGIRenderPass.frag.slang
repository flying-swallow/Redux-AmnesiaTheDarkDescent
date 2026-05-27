// Final composite pass for surfel-GI rendering.
//
// Reads `gPackedHitInfo` (uint4 TriangleHit) + `gIndirectLighting`
// (per-pixel indirect from the surfel cache) and returns the final
// shaded color. The pass either:
//   * misses → no environment-map eval today, so emits black
//   * hits   → recovers world-space vertex data + diffuse albedo,
//              optionally adds direct lighting via NEE-over-all-lights
//              with inline RayQuery shadow tests, and multiplies the
//              indirect by albedo (since the surfel cache stored
//              incoming radiance with the cosθ factor baked in, the
//              composite weight is just albedo / π · π = albedo)
//
// Converted from the original compute pass to a fragment pass: the per-
// pixel write is now an SV_Target return so the surfel-GI result lands
// in the post-effect pogo buffer (color attachment) instead of a storage
// image. This keeps the entire post-write chain uniformly graphics, so
// the RI_PogoBuffer barrier helpers (which are authored for
// COLOR_ATTACHMENT_OUTPUT ↔ FRAGMENT_SHADER transitions) apply directly.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Bindless.resource;
import Bindless.interface;
import PerFrame.resource;

static const float kInvPi  = 0.31830988618379067154f;
static const float kRayEps = 1.0e-3f;

[vk::binding(0, 2)]
Texture2D<float4> gIndirectLighting;

// Rasterized V-buffer fallback. Same packed layout as `gPackedHitInfo`
// (see SurfelGBuffer.3d.slang's psMain), but produced by the raster
// GBuffer pass instead of the RT V-buffer. We fall back to it when the
// RT hit is invalid so pixels covered by raster geometry but missed by
// the RT path (subpixel jitter at edges, tMax slop, alpha-test
// differences) still get shaded instead of going to env/black.
// NOTE: raster writes .w = 0 on hits, RT writes .w = 1 — HitInfo.isValid()
// passes either as long as the other fields aren't all zero, so the
// instance-0 / prim-0 / barys-(0,0,0) corner case can read as invalid
// from raster. Same edge case that already exists for any consumer of
// the raster V-buffer; not addressed here.
[vk::binding(1, 2)]
Texture2D<uint4> gPackedHitInfoRaster;

struct DiffuseShading
{
    float3 posW;
    float3 normalW;
    float3 albedo;
    float3 emission;
    };

// emission = illumination-map texel (DiffuseMaterial slot 5) * per-object glow
// scalar (UniformObject.illuminationAmount). Without this the surfel composite
// ignores illumination entirely and glowing textures render dark.
DiffuseShading prepareDiffuseShading(VertexData v, uint materialID, float illuminationAmount)
{
    DiffuseShading sd;
    sd.posW    = v.posW;
    sd.normalW = normalize(v.normalW);
    DiffuseMaterial m = gScene.getDiffuseMaterial(materialID);
    uint diffuseTex = m.tex[0];
    if (diffuseTex == kInvalidTextureIndex)
        sd.albedo = float3(0.5f);
    else
        sd.albedo = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                        .SampleLevel(gMaterialSampler, v.texC, 0.0f).rgb;

    uint illumTex = m.tex[5];
    if (illumTex == kInvalidTextureIndex || illuminationAmount <= 0.f)
        sd.emission = float3(0.f);
    else
        sd.emission = gTextures2D[NonUniformResourceIndex(illumTex)]
                          .SampleLevel(gMaterialSampler, v.texC, 0.0f).rgb
                      * illuminationAmount;
    return sd;
}

// NEE over all lights. Single sample per light, BRDF = albedo / π,
// shadow visibility via gScene.materials.traceShadowRay (alpha-tested
// non-opaque RayQuery — see the comment on that helper in Scene.slang
// for why a plain Proceed/CommittedStatus pair silently returns "visible"
// against this engine's non-opaque BLAS). Falcor's version iterated
// lights and used `sampleLight(...)` + Falcor's BSDF; the project's
// `sampleLightByIndex` returns a unified LightSample.
float3 evalAnalyticLight(DiffuseShading sd)
{
    uint lightCount = gScene.getLightCount();
    if (lightCount == 0) return float3(0.f);

    float3 Lr = float3(0.f);
    float3 origin = sd.posW + sd.normalW * kRayEps;

    for (uint i = 0; i < lightCount; ++i)
    {
        LightSample ls;
        if (!gScene.sampleLightByIndex(i, sd.posW, ls)) continue;

        float NdotL = dot(sd.normalW, ls.dir);
        if (NdotL <= 0.f) continue;

        if (!gScene.materials.traceShadowRay(origin, ls.dir, ls.distance)) continue;

        // Base game omits the 1/π Lambert factor (deferred_light_pointlight.frag.fsl
        // / visibility_shade.frag: albedo·color·atten·ndl); match it so direct
        // reads at the original brightness. Albedo is applied here (composite).
        Lr += sd.albedo * ls.Li * NdotL;
    }
    return Lr;
}

struct PSIn
{
    [vk::location(0)] float2 v_uv;
    float4 posH : SV_Position;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    uint2 pixelPos = uint2(pin.posH.xy);
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);

    if (!hitInfo.isValid())
    {
        // RT miss — fall back to the rasterized V-buffer for this pixel.
        // The two paths share the packed TriangleHit layout, so we can
        // re-wrap and re-test isValid() with no extra unpack work.
        hitInfo = HitInfo(gPackedHitInfoRaster.Load(int3(pixelPos, 0)));

        if (!hitInfo.isValid())
        {
            // Both paths missed — no env-map binding today, emit black.
            // When env map gets wired into the bindless set, this is
            // where the lookup goes:
            //   `float3 envColor = gEnvMap.SampleLevel(..., ray, 0).rgb;`
            return float4(0.f, 0.f, 0.f, 1.f);
        }
    }

    TriangleHit triangleHit = hitInfo.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    float illuminationAmount = gScene.getObject(triangleHit.instanceID).illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount);

    bool wantDirect   = kDefaultRenderDirectLighting   != 0u;
    bool wantIndirect = kDefaultRenderIndirectLighting != 0u;

    float3 direct   = wantDirect   ? evalAnalyticLight(sd) : float3(0.f);
    float3 indirect = wantIndirect ? gIndirectLighting[pixelPos].xyz : float3(0.f);

    float3 finalGather;
    if (wantDirect && wantIndirect)
    {
        // Diffuse Lambert: weight = f(wi, wo) * dot(wo, n) / pdf(wo).
        // With f = albedo / π and pdf = cosθ / π for the cosine-sampled
        // surfel cache that produced indirect, this reduces to albedo.
        finalGather = sd.albedo * (direct  + indirect);
    }
    else
    {
        finalGather = direct + indirect;
    }

    // Emissive (illumination-map) surfaces glow regardless of the direct /
    // indirect toggles — added on top so glowing textures aren't dark.
    finalGather += sd.emission;

    return float4(finalGather, 1.f);
}
