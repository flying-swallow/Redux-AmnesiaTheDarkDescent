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
import bindless;
import HitInfo;
import PerFrame.resource;
import Fog;
import SurfelGI.SurfelShade;        // DiffuseShading / prepareDiffuseShading / evalAnalyticLight / gatherSurfelIndirect
import Grid;                         // gridCellPos / gridFlattenCellIndex / gridCellValid (decal grid lookup)
import Utils.Color.ColorHelpers;     // linearToSRGB / sRGBToLinear (legacy display-space decal blend)

[vk::binding(0, 2)]
Texture2D<float4> gIndirectLighting;

// Temporally-accumulated direct lighting (soft shadows), produced by
// DirectLightingPass. Demodulated irradiance — the gather applies the
// (decal-composited) albedo.
[vk::binding(3, 2)]
Texture2D<float4> gDirectLighting;

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

// Blend one decal texel onto the albedo, replicating the legacy hardware
// blend factors (src = decal texel*tint, dst = albedo). Mode is eMaterialBlendMode
// (1=Add, 2=Mul, 3=MulX2, 4=Alpha, 5=PremulAlpha). Operands are DISPLAY-space
// values (projectDecal encodes both sides and decodes the result), so the math
// is bit-for-bit the legacy gbuffer blend.
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

    DiffuseMaterial dm = gScene.getDiffuseMaterial(d.materialID);
    uint dtex = dm.tex[0];
    if (dtex == kInvalidTextureIndex)
        return albedo;

    float4 s = gTextures2D[NonUniformResourceIndex(dtex)]
                   .SampleLevel(gMaterialSampler, uv, 0.0f);
    // Legacy display-space blend, computed EXACTLY (software blend with dst in
    // hand — unlike the raster passes no powered-weight approximation is
    // needed): re-encode the sRGB-decoded texel and the linear albedo back to
    // the authored display space, blend like the legacy gbuffer did, decode
    // the result. d.color is the raw authored tint. This is "the albedo the
    // legacy gbuffer would have held", in linear — fixes e.g. MULX2's 0.5-gray
    // neutral and the residual pink-blood tint of the old linear-blend
    // compromise.
    float4 srcD = float4(linearToSRGB(s.rgb), s.a) * d.color;
    if (srcD.a < 0.01f)                                       // legacy ALPHA_REJECT
        return albedo;

    return sRGBToLinear(applyDecalBlend(d.blendMode, linearToSRGB(albedo), srcD));
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

    // Water and glass refraction need no swap here: SurfelVBuffer.rt re-traces the
    // refraction ray as a kind-0 bounce that clobbers gPackedHitInfo with the
    // refracted background, so the primary hit read above IS already that
    // background at water/glass pixels. The water surface (tint + reflection) and
    // the glass surface are drawn on top in their raster passes.

    VertexData v = gScene.getVertexData(triangleHit);
    UniformObject obj = gScene.getObject(triangleHit.instanceID);
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount);

    // Composite clustered OOB decals straight onto the base albedo, before
    // lighting, so they get lit like the surface (replaces the old albedo_resolve
    // buffer + decal raster pass).
    sd.albedo = compositeDecals(sd.albedo, v.posW, sd.normalW, obj.decalList);

    bool wantDirect   = kDefaultRenderDirectLighting   != 0u;
    bool wantIndirect = kDefaultRenderIndirectLighting != 0u;

    // Direct lighting is now a temporally-accumulated texture (soft shadows),
    // produced by DirectLightingPass; just read it. Indirect from the surfel cache.
    float3 direct   = wantDirect   ? gDirectLighting[pixelPos].xyz : float3(0.f);
    float3 indirect = wantIndirect ? gIndirectLighting[pixelPos].xyz : float3(0.f);

    // Both terms are demodulated irradiance: direct is Σ Li·NdotL·vis from the
    // direct pass (albedo factored out so the accumulation can't smear it),
    // indirect is incoming irradiance from the cosine-sampled surfel cache
    // (f = albedo/π, pdf = cosθ/π → reduces to albedo). Each takes exactly one
    // albedo to become outgoing radiance — applied here, AFTER compositeDecals,
    // so projected decals modulate direct and indirect alike (matching the old
    // inline evalAnalyticLight path and the base game's albedo·color·atten·NdotL).
    float3 finalGather = sd.albedo * (direct + indirect);

    // Emissive (illumination-map) surfaces glow regardless of the direct /
    // indirect toggles — added on top so glowing textures aren't dark.
    finalGather += sd.emission;

    // Per-pixel fog: world fog + each fog-area bounding box from gFogAreas.
    // Replaces the legacy per-object visibility table (which only existed
    // for translucents) — applies uniformly to the opaque composite here.
    finalGather = applyFog(finalGather, v.posW, gPerFrame.posW);

    gOutput[pixelPos] = float4(finalGather, 1.f);
}
