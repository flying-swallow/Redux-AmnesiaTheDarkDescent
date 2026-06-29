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
import BlendModes;                   // blendDisplaySpaceExact (raw eMaterialBlendMode numbering)

struct MainCompositePushConstants
{
    uint overlayMode;
};

[vk::push_constant]
ConstantBuffer<MainCompositePushConstants> gPushConstants;

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

// Per-world decal data, baked by cWorld::Compile and bound on set 2 (the
// per-pass set) alongside this pass's resources above. Declared here rather than
// in bindless.slang because MainCompositePass is the only consumer — putting them
// in the shared module collided with every other pass's set-2 scratch. Bindings
// start at kBindingWorldDecals (4) to clear the composite's slots 0..3 above.
[vk::binding(kBindingWorldDecals, kWorldDecalSet)]
StructuredBuffer<GpuDecal> gDecals;
// Flat per-object decal-index pool. UniformObject.decalList packs (offset<<8)|count;
// compositeDecals reads gObjectDecalIndices[offset + k] → a stable gDecals[] index.
[vk::binding(kBindingWorldObjectDecalIndices, kWorldDecalSet)]
StructuredBuffer<uint> gObjectDecalIndices;

// Per-viewport decal accumulators: Type="Decal" meshes (Mul/MulX2/Add, no alpha)
// rasterized this frame. decalMul holds the multiplicative factor (white where
// no decal), decalAdd the additive term (black where no decal). Applied to the
// base albedo before lighting: albedo = albedo*decalMul + decalAdd, so the mesh
// decals are lit + fogged by the composite (the V-buffer renderer has no albedo
// G-buffer to write them into). Set-2 slots 6,7 after the world decal buffers.
[vk::binding(6, 2)]
Texture2D<float4> gDecalMul;
[vk::binding(7, 2)]
Texture2D<float4> gDecalAdd;

// --- Decal projection (folded in from the old albedo_resolve pass) ----------
// Clustered OOB decals are composited directly onto the base albedo here, before
// lighting, so they get lit like the surface — no separate albedo buffer/pass.

// The per-texel decal blend is blendDisplaySpaceExact (BlendModes.slang):
// software blend with dst in hand, replicating the legacy hardware blend
// factors bit-for-bit in display space. Takes the RAW eMaterialBlendMode
// numbering GpuDecal.blendMode stores (None=0 … PremulAlpha=5).

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

    // Decal diffuse texture is baked directly into the decal (a pinned, stable
    // gTextures2D[] slot), so no per-frame material indirection is needed.
    uint dtex = d.diffuseTexIndex;
    if (dtex == kInvalidTextureIndex)
        return albedo;

    float4 s = sampleBindless2DLevel(dtex, uv, 0.0f);
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

    return sRGBToLinear(blendDisplaySpaceExact(d.blendMode, linearToSRGB(albedo), srcD));
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
    const uint overlayMode = gPushConstants.overlayMode;
    
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
    
    // Modes 1–5 are already rendered as diagnostic colors by
    // SurfelGenerationPass. Display them without albedo, direct lighting,
    // emission, decals, or fog modifying them.
    if (overlayMode >= kOverlayModeVariance && overlayMode <= kOverlayModeCoverage)
    {
        gOutput[pixelPos] = float4(gIndirectLighting[pixelPos].xyz, 1.f);
        return;
    }

    TriangleHit triangleHit = hitInfo.getTriangleHit();
    UniformObject obj = gSceneObjects[triangleHit.instanceID];

    if (overlayMode == kOverlayModeShadowFlag)
    {
        const bool castsShadow = (obj.renderFlags & kObjectFlagShadowCaster) != 0u;
        gOutput[pixelPos] = float4(castsShadow ? float3(0.f, 1.f, 0.f)
                                               : float3(1.f, 0.f, 0.f),
                                   1.f);
        return;
    }

    uint materialID = obj.materialID;

    // Water and glass refraction need no swap here: SurfelVBuffer.rt re-traces the
    // refraction ray as a kind-0 bounce that clobbers gPackedHitInfo with the
    // refracted background, so the primary hit read above IS already that
    // background at water/glass pixels. The water surface (tint + reflection) and
    // the glass surface are drawn on top in their raster passes.

    VertexData v = getVertexData(triangleHit);
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount);

    // Composite clustered OOB decals straight onto the base albedo, before
    // lighting, so they get lit like the surface (replaces the old albedo_resolve
    // buffer + decal raster pass).
    sd.albedo = compositeDecals(sd.albedo, v.posW, sd.normalW, obj.decalList);

    // Rasterized Type="Decal" mesh decals (Mul/MulX2/Add): apply the multiply
    // factor then the additive term (same place + reason as the clustered decals
    // above). Done in DISPLAY (gamma) space — matching the legacy gamma diffuse
    // G-buffer — so MulX2's 0.5-gray neutral stays neutral (2×0.5=1) instead of
    // darkening; saturate matches the legacy UNORM [0,1] clamp. White/black
    // identities leave non-decal pixels unchanged. Lit + fogged below.
    float3 albedoG = linearToSRGB(sd.albedo);
    albedoG = saturate(albedoG * gDecalMul[pixelPos].rgb + gDecalAdd[pixelPos].rgb);
    sd.albedo = sRGBToLinear(albedoG);

    bool wantDirect   = kDefaultRenderDirectLighting   != 0u;
    bool wantIndirect = kDefaultRenderIndirectLighting != 0u;

    if (overlayMode == kOverlayModeDirectOnly)
    {
        wantDirect   = true;
        wantIndirect = false;
    }
    else if (overlayMode == kOverlayModeIndirectOnly)
    {
        wantDirect   = false;
        wantIndirect = true;
    }

    float3 direct = wantDirect ? gDirectLighting[pixelPos].xyz : float3(0.f);
    float3 indirect = wantIndirect ? gIndirectLighting[pixelPos].xyz : float3(0.f);

    // Both terms are demodulated irradiance: direct is Σ Li·NdotL·vis from the
    // direct pass (albedo factored out so the accumulation can't smear it),
    // indirect is incoming irradiance from the cosine-sampled surfel cache
    // (f = albedo/π, pdf = cosθ/π → reduces to albedo). Each takes exactly one
    // albedo to become outgoing radiance — applied here, AFTER compositeDecals,
    // so projected decals modulate direct and indirect alike (matching the old
    // inline evalAnalyticLight path and the base game's albedo·color·atten·NdotL).
    float3 finalGather = sd.albedo * (direct + indirect);

    if (!(overlayMode == kOverlayModeDirectOnly || overlayMode == kOverlayModeIndirectOnly))
    {
        // Emissive (illumination-map) surfaces glow regardless of the direct /
        // indirect toggles — added on top so glowing textures aren't dark.
        finalGather += sd.emission;

        // Per-pixel fog: world fog + each fog-area bounding box from gFogAreas.
        // Replaces the legacy per-object visibility table (which only existed
        // for translucents) — applies uniformly to the opaque composite here.
        finalGather = applyFog(finalGather, v.posW, gPerFrame.posW);
    }

    gOutput[pixelPos] = float4(finalGather, 1.f);
}
