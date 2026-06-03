// Primary-ray V-buffer RT shader. Traces one primary ray per swapchain
// pixel through `gRtAccel`, packs (instanceID, primitiveIndex,
// barycentrics) into `gPackedHitInfo` for the surfel passes to consume.
// Refractive glass AND water re-trace their refraction ray as a kind-0 bounce
// that clobbers gPackedHitInfo with the refracted background (the GI pass then
// shades that background; the glass/water surface is rasterised on top —
// Translucent.frag for glass, the water raster pass for water). There are no
// separate refraction/reflection bounce buffers; water reflection is done in the
// raster water pass.
//
// Ported from Falcor's SurfelVBuffer.rt.slang to the project Scene + bindless
// surface. The Falcor anyhit helpers (`getGeometryInstanceID()`,
// `getVertexData(id, prim, attribs)`) are replaced with `InstanceID()` +
// `gScene.getVertexData(triangleHit)`. `gScene.rtAccel` is replaced with the
// bindless `gRtAccel` global, and the Falcor `Ray.toRayDesc()` helper is
// expanded inline because the project's Ray struct doesn't carry that
// method.

#include "../HostDefinitions.h"

import SceneTypes;
import Scene;
import bindless;
import HitInfo;
import PerFrame.resource;
import Brdf;
import TranslucentPass.WaterCommon;   // waterWaveNormalTangent (shared with Water.frag)

// Resolution comes from DispatchRaysDimensions() — no per-pass CB.
//
// One V-buffer texture (bindless set 1):
//   gPackedHitInfo — primary surface hit (every covered pixel); also the
//                    refracted background for glass AND water (the kind-0
//                    refraction reroute clobbers it). Water reflection is drawn
//                    by the raster water pass; there are no bounce buffers.
//
// The Falcor port wrote a separate per-pixel depth buffer; the engine
// reconstructs depth from gPackedHitInfo + the camera projection so the
// dedicated `gDepth` RWTexture has been dropped.

// Payload.kind routes the closeHit / miss write. Primary rays (kind 0) write
// gPackedHitInfo; glass refraction re-traces as kind 0 to overwrite it with
// the refracted background. Water spawns kind 1 (refraction) and kind 2
// (reflection) bounces whose secondary closeHit writes the matching bounce
// texture (stamping the source surface ID into .w).
struct Payload
{
    float3 origin;
    float3 direction;
    uint   length;            // 0 = primary, >=1 = bounce
    uint   kind;              // 0 = primary, 1 = refraction-bounce, 2 = reflection-bounce
    uint   sourceInstanceID;  // for bounce writes: the primary's instanceID
};

[shader("miss")]
void miss(inout Payload payload)
{
    // Only the PRIMARY miss (kind 0, length 0) clears gPackedHitInfo. The
    // glass refraction reroute also runs as kind 0 but at length 1 — if that
    // bounce escapes to sky we must NOT clear the pixel (the primary already
    // wrote the glass surface there; clearing it reintroduces speckled holes
    // through glass). The two bounce textures are cleared host-side each
    // frame, so a kind 1/2 miss is silent via the .w==0 valid flag.
    if (payload.kind == 0u && payload.length == 0u)
    {
        uint2 pixelPos = DispatchRaysIndex().xy;
        gPackedHitInfo[pixelPos] = uint4(0, 0, 0, 0);
    }
}

[shader("anyhit")]
void anyHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test against the diffuse material's alpha slot.
    TriangleHit hit;
    hit.instanceID     = InstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics   = attribs.barycentrics;
    VertexData v = gScene.getVertexData(hit);
    uint materialID = gScene.getMaterialID(hit.instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
}

[shader("closesthit")]
void closeHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    uint2 pixelPos = DispatchRaysIndex().xy;

    TriangleHit triangleHit;
    triangleHit.instanceID     = InstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics   = attribs.barycentrics;

    // All hits write the primary V-buffer. Water/glass refraction re-traces as a
    // kind-0 bounce (length 1) that clobbers gPackedHitInfo with the refracted
    // background — there are no separate bounce buffers anymore (reflection moved
    // to the raster water pass).
    gPackedHitInfo[pixelPos] = triangleHit.pack();

    // Only the primary ray spawns a follow-up bounce. Secondary bounces
    // just keep the closest hit recorded above.
    if (payload.length != 0u)
        return;

    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    // Water surface: re-trace the refraction ray as a kind-0 bounce (length 1)
    // that clobbers gPackedHitInfo with the underwater background — exactly the
    // glass path below, but bent by the wave-animated normal. The GI composite
    // then shades that background through the normal opaque path; the water
    // surface (tint + reflection) is drawn in the raster water pass. No reflection
    // bounce / bounce buffers anymore. Checked before the generic glass branch
    // (water also sets HasRefraction) so it uses the wave normal, not the flat one.
    if (gScene.materials.isWater(materialID))
    {
        VertexData v = gScene.getVertexData(triangleHit);
        WaterMaterial water = gScene.getWaterMaterial(materialID);

        // World-space TBN from the hit, then the wave-perturbed normal.
        float3 N = normalize(v.normalW);
        float3 T = normalize(v.tangentW);
        float3 B = normalize(cross(N, T));
        float3 nT = waterWaveNormalTangent(water, v.texC, gPerFrame.afT);
        float3 waveN = normalize(nT.x * T + nT.y * B + nT.z * N);

        float3 incident = normalize(payload.direction);

        // Camera rays hit water from outside → always ENTERING. Orient the wave
        // normal against the ray so a normal facing away doesn't flip eta > 1 and
        // spuriously total-internal-reflect (which would leave no visible bottom).
        if (dot(incident, waveN) > 0.0f)
            waveN = -waveN;

        float refractScale = max(water.refractionScale, 0.0f);
        // eta = n_air / n_water < 1 (entering); refract() is non-zero for eta < 1.
        float  eta          = 1.0f / (1.0f + refractScale);
        float3 refractedDir = refract(incident, waveN, eta * kWaterRefractionIntensity);
        // On TIR (refract() -> 0) leave the primary as the water surface hit
        // (shaded as flat water-colour albedo) — same fallback as glass.
        if (dot(refractedDir, refractedDir) >= 1.0e-6f)
        {
            refractedDir = normalize(refractedDir);
            payload.origin           = v.posW + refractedDir * 1.0e-4f;
            payload.direction        = refractedDir;
            payload.length++;                 // 0 -> 1 (don't recurse again)
            payload.kind             = 0u;    // clobber gPackedHitInfo
            payload.sourceInstanceID = triangleHit.instanceID;

            RayDesc rd;
            rd.Origin    = payload.origin;
            rd.Direction = payload.direction;
            rd.TMin      = 0.0f;
            rd.TMax      = 1.0e30f;
            TraceRay(
                gRtAccel,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                kRayMaskOpaque, 0, 1, 0, rd, payload);
        }
        return;
    }

    // Refractive translucents (glass, refractive decals) ray-bend: re-trace the
    // refraction ray as a kind-0 bounce that clobbers gPackedHitInfo with the
    // refracted background (the GI pass shades it). The glass surface itself is
    // rasterised on top in Translucent.frag.slang.
    if (gScene.materials.isRefractive(materialID))
    {
        VertexData v          = gScene.getVertexData(triangleHit);
        float3 incidentRayDir = normalize(payload.direction);
        float3 normalW        = normalize(v.normalW);

        // IOR sourced from TranslucentMaterial.refractionScale — the legacy
        // shader used this as a screen-space distortion magnitude; reusing
        // it here as a relative IOR offset keeps materials authoring the
        // same scalar for both renderers. eta = n1/n2 (ratio entering).
        TranslucentMaterial mat = gScene.getTranslucentMaterial(materialID);
        float refractScale  = max(mat.refractionScale, 0.0f);

        // Distinguish entry vs exit by sign of dot(incident, normal). On
        // entry (cosI < 0) eta = 1 / (1 + scale); on exit, flip eta and
        // the normal so refract() sees a forward-facing geometry.
        float  cosI    = dot(incidentRayDir, normalW);
        bool   exiting = cosI > 0.0f;
        float3 surfN   = exiting ? -normalW : normalW;
        float  eta     = exiting ? (1.0f + refractScale)
                                 : (1.0f / (1.0f + refractScale));

        float3 refractedDir = refract(incidentRayDir, surfN, eta);

        // Total internal reflection — refract() returns the zero vector.
        // Leave the refraction V-buffer pixel at its cleared (invalid)
        // state; the consumer (Translucent.frag.slang) will fall back to
        // drawing the surface normally without a refracted backdrop.
        if (dot(refractedDir, refractedDir) >= 1.0e-6f) {
            refractedDir = normalize(refractedDir);
            // Bias along the refracted direction (away from the surface in
            // the direction we're now travelling) to avoid self-hit.
            payload.origin    = v.posW + refractedDir * 1.0e-4f;
            payload.direction = refractedDir;
            payload.length++;
            payload.kind             = 0u;
            payload.sourceInstanceID = triangleHit.instanceID;

            RayDesc rayDesc;
            rayDesc.Origin    = payload.origin;
            rayDesc.Direction = payload.direction;
            rayDesc.TMin      = 0.0f;
            rayDesc.TMax      = 1.0e30f;

            TraceRay(
                gRtAccel,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                kRayMaskOpaque,
                0, /* hitIdx */
                1, /* rayTypeCount */
                0, /* missIdx */
                rayDesc,
                payload
            );
        }
    }

    // No ray-traced reflection bounce: reflection in this game is cubemap-based
    // (static cubemap Fresnel in Translucent.frag for glass). Water is the only
    // surface that ray-traces a reflection, handled in its branch above.
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixelPos = DispatchRaysIndex().xy;
    Ray ray = gScene.camera.computeRayPinhole(pixelPos, DispatchRaysDimensions().xy);

    Payload payload;
    payload.origin           = ray.origin;
    payload.direction        = ray.dir;
    payload.length           = 0;
    payload.kind             = 0u;
    payload.sourceInstanceID = 0xffffffffu;  // sentinel — primary has no source

    RayDesc rayDesc;
    rayDesc.Origin    = ray.origin;
    rayDesc.Direction = ray.dir;
    rayDesc.TMin      = ray.tMin;
    rayDesc.TMax      = ray.tMax;

    TraceRay(
        gRtAccel,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        kRayMaskOpaque | kRayMaskTranslucent,
        0, /* hitIdx */
        1, /* rayTypeCount */
        0, /* missIdx */
        rayDesc,
        payload
    );
}
