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
// Runs as a compute pass: one thread per pixel writes the surfel-GI result
// into the post-effect pogo buffer bound as a storage image (gOutput). The
// renderer transitions the pogo attach to GENERAL around the dispatch and back
// to COLOR_ATTACHMENT_OPTIMAL afterwards, so the downstream pogo toggle and the
// translucent/particle raster passes are unaffected.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Bindless.resource;
import Bindless.interface;
import PerFrame.resource;
import Fog;
import SurfelGI.SurfelShade;        // DiffuseShading / prepareDiffuseShading / evalAnalyticLight / gatherSurfelIndirect
import Grid;                         // gridCellPos / gridFlattenCellIndex / gridCellValid (decal grid lookup)
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

// Output: the post-effect pogo HDR target, bound as a storage image. The
// renderer puts it in GENERAL for the dispatch and back to COLOR afterwards.
[vk::binding(2, 2)]
RWTexture2D<float4> gOutput;

// --- Decal projection (folded in from the old albedo_resolve pass) ----------
// Clustered OOB decals are composited directly onto the base albedo here, before
// lighting, so they get lit like the surface — no separate albedo buffer/pass.

// Blend one decal texel onto the (linear) albedo, replicating the legacy hardware
// blend factors (src = decal texel*tint, dst = albedo). Mode is eMaterialBlendMode
// (1=Add, 2=Mul, 3=MulX2, 4=Alpha, 5=PremulAlpha).
float3 applyDecalBlend(uint mode, float3 dst, float4 src)
{
    switch (mode)
    {
    case 1u: return dst + src.rgb;                  // Add
    case 2u: return dst * src.rgb;                  // Mul
    case 3u: return 2.0f * dst * src.rgb;           // MulX2
    case 4u: return lerp(dst, src.rgb, src.a);      // Alpha
    case 5u: return src.rgb + dst * (1.0f - src.a); // PremulAlpha
    default: return dst;                            // None
    }
}

// Reject surfaces that are near-perpendicular to the decal's projection axis
// (the bleed artifact: a decal box catching side walls / edges). Two-sided and
// derived from the box's own axes, so it's independent of the projection-axis
// sign/convention. |cos| below this (~angle > ~75deg from the axis) is culled.
static const float kDecalFacingCos = 0.25f;

// Project one decal (by index): box-clip, facing-cull, atlas-UV, sample, blend.
// The per-object decal list (compositeDecals) is the editor's static association,
// so a listed decal is one this object was clipped to.
float3 projectDecal(uint di, float3 albedo, float3 posW, float3 normalW)
{
    GpuDecal d = gDecals[di];

    float3 lp = mul(d.invModelMat, float4(posW, 1.0f)).xyz;   // world -> box-local
    if (any(abs(lp) > 0.5f))
        return albedo;

    // Facing cull in box-local space: transform the surface normal by the same
    // invModelMat used for the position clip (sign-consistent with the box), and
    // require a real component along the projection axis (local Y). Reject only
    // near-perpendicular surfaces — keeps both facings, so it can't hide a
    // correctly-placed decal from an axis-sign mismatch.
    float3 nLocal = mul((float3x3)d.invModelMat, normalW);
    float  nLen   = length(nLocal);
    if (nLen < 1.0e-6f || abs(nLocal.y) <= kDecalFacingCos * nLen)
        return albedo;

    // Project along local Y; remap into the selected atlas cell, matching the
    // editor's baked UVs (DecalCreator.cpp:346-359). subDivIndex is the 0-based
    // cell used directly; clamp to [0,1] as the editor does so stale/degenerate
    // cell values (e.g. CurrentSubDiv=3 with a 1x1 grid) sample the edge instead
    // of off-texture.
    //   u = clamp((cellCol + (localX+0.5)) / subDivX, 0, 1)
    //   v = 1 - clamp((cellRow + (localZ+0.5)) / subDivY, 0, 1)   (V flipped)
    uint  sx = max(d.subDivX, 1u);
    uint  sy = max(d.subDivY, 1u);
    float fU = float(d.subDivIndex % sx);
    float fV = float(d.subDivIndex / sx);
    float2 uv = float2(saturate((fU + (lp.x + 0.5f)) / float(sx)),
                       1.0f - saturate((fV + (lp.z + 0.5f)) / float(sy)));

    DiffuseMaterial dm = gDiffuseMaterials[d.materialID];
    uint dtex = dm.tex[0];
    if (dtex == kInvalidTextureIndex)
        return albedo;

    float4 s = gTextures2D[NonUniformResourceIndex(dtex)]
                   .SampleLevel(gMaterialSampler, uv, 0.0f);
    s *= d.color;
    if (s.a < 0.01f)                                          // legacy ALPHA_REJECT
        return albedo;

    return applyDecalBlend(d.blendMode, albedo, s);
}

// Composite the decals precomputed for this object (the static association the
// editor clipped it to). `decalList` packs (offset << 8) | count into the shared
// gObjectDecalIndices pool. `normalW` is the hit surface normal (facing cull).
float3 compositeDecals(float3 albedo, float3 posW, float3 normalW, uint decalList)
{
    uint count  = decalList & 0xFFu;
    uint offset = decalList >> 8;
    for (uint k = 0u; k < count; ++k)
        albedo = projectDecal(gObjectDecalIndices[offset + k], albedo, posW, normalW);
    return albedo;
}

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

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint2 pixelPos = tid.xy;
    uint  outW, outH;
    gOutput.GetDimensions(outW, outH);
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;   // edge threads of the rounded-up dispatch

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
            gOutput[pixelPos] = float4(0.f, 0.f, 0.f, 1.f);
            return;
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
        refrColor *= kWaterRefractionExposure;
        reflColor *= kWaterReflectionExposure;

        // Water diffuse tint at the surface UV (reference combine:
        // surfaceColor * refraction + reflection * fresnel).
        float3 surfaceTint = float3(1.f);
        uint   diffuseTex  = water.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
            surfaceTint = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                              .SampleLevel(gMaterialSampler, vW.texC, 0.f).rgb;

        float3 waterRgb = surfaceTint * refrColor + reflColor * fresnel;
        waterRgb = applyFog(waterRgb, vW.posW, gPerFrame.posW);
        gOutput[pixelPos] = float4(waterRgb, 1.f);
        return;
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

    // Composite clustered OOB decals straight onto the base albedo, before
    // lighting, so they get lit like the surface (replaces the old albedo_resolve
    // buffer + decal raster pass).
    uint decalList = gScene.getObject(triangleHit.instanceID).decalList;
    sd.albedo = compositeDecals(sd.albedo, v.posW, sd.normalW, decalList);

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

    gOutput[pixelPos] = float4(finalGather, 1.f);
}
