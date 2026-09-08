/// Screen-space fog helper shared by the main composite pass and the
/// translucent / particle / water fragment shaders. Replaces the legacy per-object fog-area
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
import bindless;
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

    // Category (b): `ray` is formed from the caller's world-space surface and
    // camera positions. In the opaque composite the surface comes from the
    // V-buffer, so this is not a pixel-derived camera ray; rayDir is jitter-
    // independent by construction and must not receive a screen-space term.
    float3 ray = surfacePosW - cameraPosW;
    float cameraDist = length(ray);
    if (cameraDist <= 0.0f) return r;
    float3 rayDir = ray / cameraDist;
    // Fog-area Start/End were authored against view-space Z in the original
    // deferred fog pass. Slab intersection still uses world-distance t, but
    // using that radial distance directly overstates density off-axis.
    float areaDepthScale = abs(mul(gPerFrame.viewMat, float4(rayDir, 0.0f)).z);

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
        FogAreaParams fa = gFogAreas[gPerFrame.fogAreaIndices[i]];

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

        fogDist *= areaDepthScale;
        if (fogDist <= 0.0f || fogDist < fa.start) continue;

        float alpha;
        if (fogDist > fa.end) {
            alpha = 1.0f;
        } else {
            alpha = saturate((fogDist - fa.start) / max(fa.end - fa.start, 1.0e-6f));
            if (fa.falloffExp != 1.0f) alpha = pow(alpha, fa.falloffExp);
        }

        float contribution = saturate(alpha * fa.colorA);
        r.visibility *= (1.0f - contribution);
        // Over-composite fog layers, just as the original alpha-blended
        // passes did. Unattenuated addition made overlapping volumes brighter
        // than either fog color and let bright fog leak through dark fog.
        r.color = r.color * (1.0f - contribution) + contribution * fa.color;
    }

    // Fog colors are authored appearance colors, not light radiance. The
    // legacy fog pass blended them directly into the scene. Our composite is
    // before the global exposure multiply, so cancel that multiply for the
    // fog contribution only (the attenuated scene keeps its normal exposure).
    // Otherwise Archives' 0.8 gray fog is lifted toward white at 2.5x exposure.
    r.color /= kSceneExposure;
    return r;
}

// Convenience wrapper for opaque consumers: returns the composited colour.
float3 applyFog(float3 sceneColor, float3 surfacePosW, float3 cameraPosW)
{
    FogResult fog = computeFog(surfacePosW, cameraPosW);
    return fog.visibility * sceneColor + fog.color;
}
