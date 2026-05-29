/// Screen-space fog helper shared by the opaque (SurfelGI composite) and
/// translucent fragment shaders. Replaces the legacy per-object fog-area
/// visibility (RendererDeferred.cpp:365-478) with a per-pixel iteration
/// that walks the gFogAreas SSBO and accumulates both a visibility scalar
/// and an additive fog colour.
///
/// Returned visibility is multiplicative: 1 = no fog, 0 = fully fogged.
/// Opaque consumers do `finalColor = visibility * sceneColor + fogColor`.
/// Translucent consumers multiply visibility into their per-pixel alpha so
/// translucent surfaces fade behind fog without painting fog colour.
///
/// World fog (gPerFrame.worldFogStart / worldFogLength / fogFalloffExp /
/// worldFogColor) is folded into the same accumulator so both kinds of fog
/// composite consistently.

#include "Constants.h"
import SceneTypes;
import Bindless.resource;
import PerFrame.resource;

struct FogResult {
    float  visibility;  // 1 = no fog, 0 = fully fogged
    float3 color;       // accumulated fog colour, premultiplied by its contribution weight
};

FogResult computeFog(float3 surfacePosW, float3 cameraPosW)
{
    FogResult r;
    r.visibility = 1.0f;
    r.color      = float3(0.0f);

    float3 ray = surfacePosW - cameraPosW;
    float cameraDist = length(ray);
    if (cameraDist <= 0.0f) return r;
    float3 rayDir = ray / cameraDist;

    // World fog — linear distance from the camera through the worldFogStart /
    // worldFogLength band, raised to fogFalloffExp. Matches the legacy
    // translucency.frag.fsl world-fog block but evaluated in world-space
    // distance instead of view-space depth so opaque + translucent agree.
    if (gPerFrame.worldFogLength > 0.0f) {
        float fogAmount = pow(
            saturate((cameraDist - gPerFrame.worldFogStart) / gPerFrame.worldFogLength),
            gPerFrame.fogFalloffExp);
        // gPerFrame.oneMinusFogAlpha = (1 - worldFogColor.a) — recover .a
        float worldFogAlpha = 1.0f - gPerFrame.oneMinusFogAlpha;
        float contribution  = fogAmount * worldFogAlpha;
        r.visibility *= (1.0f - contribution);
        r.color      += contribution * gPerFrame.worldFogColor.rgb;
    }

    // Per-fog-area iteration. The work per pixel is bounded by
    // gPerFrame.fogAreaCount, which is typically a handful (capped at
    // kFogAreaCapacity host-side).
    uint count = gPerFrame.fogAreaCount;
    for (uint i = 0u; i < count; ++i) {
        FogAreaParams fa = gFogAreas[i];

        // Move the ray into the box's unit-cube local space. Point uses w=1
        // (translation applied), direction uses w=0 (rotation+scale only).
        float3 boxCamOrigin = mul(fa.invModelMat, float4(cameraPosW, 1.0f)).xyz;
        float3 boxRayDir    = mul(fa.invModelMat, float4(rayDir,     0.0f)).xyz;

        // Slab method against [-0.5, 0.5]^3. Guard divide-by-zero on
        // axis-aligned rays by replacing tiny denominators with a large
        // value — the slab will then trivially pass on that axis.
        float3 invDir = float3(
            abs(boxRayDir.x) > 1.0e-6f ? 1.0f / boxRayDir.x : 1.0e30f,
            abs(boxRayDir.y) > 1.0e-6f ? 1.0f / boxRayDir.y : 1.0e30f,
            abs(boxRayDir.z) > 1.0e-6f ? 1.0f / boxRayDir.z : 1.0e30f);
        float3 t1 = (float3(-0.5f) - boxCamOrigin) * invDir;
        float3 t2 = (float3( 0.5f) - boxCamOrigin) * invDir;
        float3 tmin = min(t1, t2);
        float3 tmax = max(t1, t2);
        float  tEntry = max(max(tmin.x, tmin.y), tmin.z);
        float  tExit  = min(min(tmax.x, tmax.y), tmax.z);

        // No intersection with the slab volume.
        if (tExit < 0.0f || tExit < tEntry) continue;

        bool cameraInside = all(abs(boxCamOrigin) <= float3(0.5f));

        // Outside-box but ray ends before reaching the box -> contributes
        // nothing (the surface is in front of the fog volume).
        if (!cameraInside && cameraDist < tEntry) continue;

        bool showBackInside  = (fa.flags & 1u) != 0u;
        bool showBackOutside = (fa.flags & 2u) != 0u;

        // Distance the eye→surface ray spends inside the box. Mirrors the
        // legacy GetFogAreaVisibilityForObject branch table.
        float fogDist;
        if (cameraInside) {
            fogDist = showBackInside ? min(tExit, cameraDist) : cameraDist;
        } else {
            fogDist = showBackOutside
                          ? min(tExit - tEntry, cameraDist - tEntry)
                          : (cameraDist - tEntry);
        }

        if (fogDist <= 0.0f || fogDist < fa.start) continue;

        float alpha;
        if (fogDist > fa.end) {
            alpha = 1.0f;
        } else {
            alpha = saturate((fogDist - fa.start) / max(fa.end - fa.start, 1.0e-6f));
            if (fa.falloffExp != 1.0f) alpha = pow(alpha, fa.falloffExp);
        }

        float contribution = alpha * fa.colorA;
        r.visibility *= (1.0f - contribution);
        r.color      += contribution * fa.color;
    }

    return r;
}

// Convenience wrapper for opaque consumers: returns the composited colour.
float3 applyFog(float3 sceneColor, float3 surfacePosW, float3 cameraPosW)
{
    FogResult fog = computeFog(surfacePosW, cameraPosW);
    return fog.visibility * sceneColor + fog.color;
}
