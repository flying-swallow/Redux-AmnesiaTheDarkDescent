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
import Fog;
import SurfelGI.SurfelShade;        // DiffuseShading / prepareDiffuseShading / evalAnalyticLight / gatherSurfelIndirect
import TranslucentPass.WaterCommon; // waterWaveNormalTangent (shared with SurfelVBuffer.rt)

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

// Screen-space LINEAR albedo (RGBA16F): the albedo resolve pass writes the base
// surface albedo and the decal pass blends decals into it, so reading it here
// makes decals part of the lit surface. Already linear — no decode needed.
[vk::binding(2, 2)]
Texture2D<float4> gAlbedo;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
    float4 posH : SV_Position;
};

// Schlick-Fresnel — mirrors deferred_common.h.fsl::Fresnel(eDotN, bias, pow).
float computeFresnel(float afEDotN, float afFresnelBias, float afFresnelPow)
{
    float fFacing = 1.0f - afEDotN;
    return saturate(afFresnelBias + (1.0f - afFresnelBias) * pow(fFacing, afFresnelPow));
}

// Shade a bounce-V-buffer hit (refraction / reflection) into outgoing
// radiance: direct NEE + albedo·(surfel indirect) + emission, matching the
// primary composite's energy convention. `packed` is a gPacked*HitInfo texel;
// .w bit 0 is the valid flag. On an invalid bounce (TIR / sky / off-TLAS) the
// caller's fallback is used instead — signalled by `valid`.
float3 shadeBounceHit(uint4 packed, out bool valid)
{
    valid = (packed.w & 1u) != 0u;
    if (!valid)
        return float3(0.f);

    HitInfo hi = HitInfo(packed);
    if (!hi.isValid()) { valid = false; return float3(0.f); }

    TriangleHit th = hi.getTriangleHit();
    uint matID = gScene.getMaterialID(th.instanceID);
    VertexData v = gScene.getVertexData(th);
    float illum = gScene.getObject(th.instanceID).illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, matID, illum);

    float3 direct   = (kDefaultRenderDirectLighting   != 0u) ? evalAnalyticLight(sd) : float3(0.f);
    float3 indirect = (kDefaultRenderIndirectLighting != 0u)
                      ? gatherSurfelIndirect(sd.posW, sd.normalW) : float3(0.f);
    return direct + sd.albedo * indirect + sd.emission;
}

float3 linearToSRGB(float3 c)
{
    c = max(c, 0.0f);

    float3 low  = c * 12.92f;
    float3 high = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;

    return lerp(high, low, c <= 0.0031308f);
}

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
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    // Water surface: the primary hit IS the water (isWater material), so shade
    // the whole surface here in one pass — refraction background + reflection +
    // wave-animated Fresnel — instead of a separate resolve pass + raster draw.
    // Both bounces are spawned by SurfelVBuffer.rt's water branch. Checked
    // before the generic isRefractive swap (water also sets HasRefraction).
    if (gScene.materials.isWater(materialID))
    {
        VertexData   vW    = gScene.getVertexData(triangleHit);
        WaterMaterial water = gWaterMaterials[materialID];

        // World-space TBN + wave-animated normal (shared with SurfelVBuffer.rt).
        float3 N = normalize(vW.normalW);
        float3 T = normalize(vW.tangentW);
        float3 B = normalize(cross(N, T));
        float3 nT = waterWaveNormalTangent(water, vW.texC, gPerFrame.afT);
        float3 waveN = normalize(nT.x * T + nT.y * B + nT.z * N);

        // Fresnel from the world-space eye vector (GI pass is world-space).
        // eye points camera→surface; orient the wave normal to face the
        // camera (same as the SurfelVBuffer water bounce) so edotN/fresnel
        // stay correct even if the mesh normal faces away.
        float3 eye   = normalize(vW.posW - gScene.camera.getPosition());
        if (dot(eye, waveN) > 0.f)
            waveN = -waveN;
        float  edotN = max(dot(-eye, waveN), 0.f);
        float  fresnel = computeFresnel(edotN, water.frenselBias, water.frenselPow);

        // Distance fade (reference reflectionFadeStart/End): fade the
        // reflection out with view depth so far water doesn't over-highlight.
        if (water.reflectionFadeEnd > 0.f)
        {
            float viewZ = -mul(gPerFrame.viewMat, float4(vW.posW, 1.f)).z;
            float fadeLen = max(water.reflectionFadeEnd - water.reflectionFadeStart, 1.0e-6f);
            fresnel *= 1.0f - saturate((viewZ - water.reflectionFadeStart) / fadeLen);
        }

        // Refraction background + reflection — full-shaded bounce hits. On an
        // invalid bounce fall back to the water surface's own surfel indirect
        // so the pixel isn't black.
        float3 surfaceFallback = gatherSurfelIndirect(vW.posW, N);
        bool   refrValid, reflValid;
        float3 refrColor = shadeBounceHit(gPackedRefractionHitInfo[pixelPos], refrValid);
        float3 reflColor = shadeBounceHit(gPackedReflectionHitInfo[pixelPos], reflValid);
        if (!refrValid) refrColor = surfaceFallback;
        if (!reflValid) reflColor = surfaceFallback;

        // Refraction (the bottom seen through the water) re-shades dim — the
        // re-shade omits the box-light ambient fill the primary composite
        // shows — so lift it toward the base game's lit-framebuffer brightness.
        // Reflection is a sharp RT trace of the lit scene, brighter/harsher
        // than the old screen-space reflection map, so damp it with the
        // reflection-intensity knob instead of lifting it.
        refrColor *= kWaterBounceExposure;
        reflColor *= kWaterReflectionIntensity;

        // Water diffuse tint at the surface UV (reference combine:
        // surfaceColor * refraction + reflection * fresnel).
        float3 surfaceTint = float3(1.f);
        uint   diffuseTex  = water.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
            surfaceTint = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                              .SampleLevel(gMaterialSampler, vW.texC, 0.f).rgb;

        float3 waterRgb = surfaceTint * refrColor + reflColor * fresnel;
        waterRgb = applyFog(waterRgb, vW.posW, gPerFrame.posW);
        float3 displayLinear = saturate(waterRgb);
		float3 displaySRGB = linearToSRGB(displayLinear);
		return float4(displaySRGB, 1.f);
    }

    // Glass refraction needs no swap here: SurfelVBuffer.rt re-traces the
    // refraction ray as a kind-0 bounce that clobbers gPackedHitInfo with the
    // refracted background, so the primary hit read above IS already that
    // background at glass pixels. (Water is handled by the isWater branch
    // above; reflection is cubemap-based in Translucent.frag.) The glass
    // surface itself is alpha-blended on top in the translucent raster pass.

    VertexData v = gScene.getVertexData(triangleHit);
    float illuminationAmount = gScene.getObject(triangleHit.instanceID).illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount);

    // Albedo comes from the screen-space buffer (base albedo + decals blended in
    // before lighting), already linear. Emission stays from the material. This is
    // what makes decals get lit like the surface.
    sd.albedo = gAlbedo[pixelPos].rgb;

    bool wantDirect   = kDefaultRenderDirectLighting   != 0u;
    bool wantIndirect = kDefaultRenderIndirectLighting != 0u;

    float3 direct   = wantDirect   ? evalAnalyticLight(sd) : float3(0.f);
    float3 indirect = wantIndirect ? gIndirectLighting[pixelPos].xyz : float3(0.f);

    // direct already carries albedo (evalAnalyticLight applies it once, matching
    // the base game's albedo·color·atten·NdotL). indirect is incoming irradiance
    // from the cosine-sampled surfel cache (f = albedo/π, pdf = cosθ/π → reduces
    // to albedo), so it takes exactly one albedo to become outgoing radiance.
    // Applying albedo to (direct + indirect) double-counted it on direct, which
    // dimmed all direct lighting (spotlight cones especially) vs the base game.
    float3 finalGather = direct + sd.albedo * indirect;

    // Emissive (illumination-map) surfaces glow regardless of the direct /
    // indirect toggles — added on top so glowing textures aren't dark.
    finalGather += sd.emission;

    // Per-pixel fog: world fog + each fog-area bounding box from gFogAreas.
    // Replaces the legacy per-object visibility table (which only existed
    // for translucents) — applies uniformly to the opaque composite here.
    finalGather = applyFog(finalGather, v.posW, gPerFrame.posW);

    float3 displayLinear = saturate(finalGather);
	float3 displaySRGB = linearToSRGB(displayLinear);
	return float4(displaySRGB, 1.f);
}
