// Primary-ray V-buffer RT shader. Traces one primary ray per swapchain
// pixel through `gRtAccel`, packs (instanceID, primitiveIndex,
// barycentrics) into `gPackedHitInfo` for the surfel passes to consume.
// On the first bounce the closest-hit reflects once before recording
// the second hit — same one-bounce reflection trick the Falcor port did
// so transparent / mirror-style surfaces still produce a usable hit.
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
import Bindless.resource;
import Bindless.interface;
import PerFrame.resource;
import Brdf;

// A V-buffer pixel stores one hit, so the one-bounce reroute can only
// represent a sharp (mirror) reflection. Reflective materials whose GGX
// roughness exceeds this fall through to the primary hit and are reflected
// by SurfelGIRenderPass's Fresnel-weighted cubemap path instead.
static const float kMirrorRoughThreshold = 0.1f;

// Resolution comes from DispatchRaysDimensions() — no per-pass CB.
// gPackedHitInfo is on the bindless set (kBindingPackedHitInfo, set 0).
// The Falcor port wrote a separate per-pixel depth buffer; the engine
// reconstructs depth from gPackedHitInfo + the camera projection so the
// dedicated `gDepth` RWTexture has been dropped.

struct Payload
{
    float3 origin;
    float3 direction;
    uint   length;     // 0 = primary ray, >=1 = reflection bounce
};

[shader("miss")]
void miss(inout Payload payload)
{
    // Only the primary miss clears the V-buffer pixel. A reflection-bounce
    // miss must leave the primary hit (written in closeHit before the
    // recursive TraceRay) intact — overwriting with (0,0,0,0) here is what
    // produced the speckled "holes" across diffuse surfaces when the bounce
    // escaped the scene.
    if (payload.length == 0)
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

    // Always store the closest hit, whether this is the primary ray or a
    // reflection bounce. If it later turns out to be a mirror surface, the
    // recursive TraceRay below re-enters closeHit and overwrites this pixel
    // with the reflected hit. A bounce-miss leaves the primary hit intact
    // (see the miss shader's payload.length guard).
    gPackedHitInfo[pixelPos] = triangleHit.pack();

    // Only the primary ray spawns a reflection, and only for materials
    // carrying a reflection cubemap (legacy "chrome" / specular surfaces).
    // Opaque diffuse / matte surfaces just keep the primary hit above.
    if (payload.length != 0u)
        return;

    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    if (!gScene.materials.isReflective(materialID))
        return;

    VertexData v          = gScene.getVertexData(triangleHit);
    float3 incidentRayDir = normalize(payload.direction);
    float3 normal         = normalize(v.normalW);
    float3 reflectionRayDir = incidentRayDir - 2.f * dot(incidentRayDir, normal) * normal;

    payload.origin    = v.posW + normal * 1e-4f;  // bias off the surface to avoid self-hit
    payload.direction = normalize(reflectionRayDir);
    payload.length++;

    RayDesc rayDesc;
    rayDesc.Origin    = payload.origin;
    rayDesc.Direction = payload.direction;
    rayDesc.TMin      = 0.f;
    rayDesc.TMax      = 1e30f;

    TraceRay(
        gRtAccel,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xff,
        0, /* hitIdx */
        1, /* rayTypeCount */
        0, /* missIdx */
        rayDesc,
        payload
    );
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixelPos = DispatchRaysIndex().xy;
    Ray ray = gScene.camera.computeRayPinhole(pixelPos, DispatchRaysDimensions().xy);

    Payload payload;
    payload.origin    = ray.origin;
    payload.direction = ray.dir;
    payload.length    = 0;

    RayDesc rayDesc;
    rayDesc.Origin    = ray.origin;
    rayDesc.Direction = ray.dir;
    rayDesc.TMin      = ray.tMin;
    rayDesc.TMax      = ray.tMax;

    TraceRay(
        gRtAccel,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xff,
        0, /* hitIdx */
        1, /* rayTypeCount */
        0, /* missIdx */
        rayDesc,
        payload
    );
}
