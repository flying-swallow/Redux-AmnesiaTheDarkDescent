// Final composite pass for the bindless visibility-buffer renderer.
//
// Given the g-buffer (visibility uint32 = (objectID, primID), packed normal,
// depth) this fragment runs once per swapchain pixel and:
//
//   1. Decodes (objectID, primID) from the visibility buffer
//   2. Pulls the 3 triangle vertices from the per-instance BDA buffers
//   3. Runs Hawkins / Burns perspective-correct barycentric (hawkins.glsl)
//      to produce ddx/ddy gradients for analytical mip-filtered texture sampling
//   4. Samples the diffuse texture via textureGrad
//   5. Reads the world-space normal from the gbuffer normal target
//   6. Reconstructs world position from the depth buffer via WorldPosFromDepth
//   7. Accumulates direct lighting from the bindless pointLights[] SSBO
//   8. Gathers indirect / GI from the surfel cache (inlined gather)
//   9. Tonemaps + writes to the swapchain image

#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_ray_query : require


// Per-pass image bindings — host wires these to the gbuffer outputs that
// have already transitioned to SHADER_READ_ONLY_OPTIMAL after the gbuffer
// epilogue in HybridRenderer.cpp.
layout(set = 2, binding = 0) uniform usampler2D visibilityBuffer;
layout(set = 2, binding = 1) uniform usampler2D normalBufferTex;
layout(set = 2, binding = 2) uniform sampler2D  depthBufferTex;
// Half-resolution surfel indirect-lighting map produced by
// surfel_generation_pass.comp. Sampled with linear filtering so the
// fullscreen composite bilinearly upsamples to swapchain resolution.
layout(set = 2, binding = 3) uniform sampler2D  surfelIndirect;
// Same TLAS used by surfel_raytrace.comp. The accel-build -> fragment-shader
// barrier in HybridRenderer.cpp orders the build against this read. Named
// `topLevelAS` to match the identifier expected by the helpers in
// traceray_rq.glsl / layouts.glsl (those references compile only when a
// `topLevelAS` is in scope, even if the call site uses the *TLAS-parameter
// variants). Binding stays at 4 because bindings 0..3 are already used by
// the gbuffer sampled-images in this pass.
layout(set = 2, binding = 4) uniform accelerationStructureEXT topLevelAS;

layout(location = 0) in  vec2 screenPosNdc;
layout(location = 0) out vec4 fragColor;

#include "host_device.h"
#include "common.glsl"               // unpack_object_id, unpack_primitive_id
#include "compress.glsl"
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"
#include "shaderUtils.glsl"          // WorldPosFromDepth
#include "random.glsl"               // tea, rand
#include "globals.glsl"              // Ray struct
// shaderUtils_surfel_cell.glsl transitively pulls in shaderUtil_grid.glsl
// (no include guard on the latter), so don't include it directly here.
#include "shaderUtils_surfel_cell.glsl" // isCellValid, getCellPos*, neighborOffset
#include "hawkins.glsl"
#include "bindless_triangle.glsl"
#include "traceray_rq.glsl"          // AnyHit (shared shadow trace)
// Piecewise sRGB transfer. Needed because the swapchain is selected as
// VK_FORMAT_*_UNORM (see RISwapchain.cpp) so the hardware does not encode
// gamma on write. If the swapchain is ever switched to a _SRGB variant,
// drop the linearTosRGB() call inside toneMapUncharted().
vec3 linearTosRGB(vec3 c)
{
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

vec3 toneMapUncharted2Impl(vec3 c)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((c * (A * c + C * B) + D * E) / (c * (A * c + B) + D * F)) - E / F;
}

vec3 toneMapUncharted(vec3 c)
{
    const float W            = 11.2;
    const float exposureBias = 2.0;
    c = toneMapUncharted2Impl(c * exposureBias);
    vec3 whiteScale = 1.0 / toneMapUncharted2Impl(vec3(W));
    return linearTosRGB(c * whiteScale);
}

// glTF KHR_lights_punctual range attenuation: physical 1/d^2 with a smooth
// (1 - (d/r)^4) rolloff that reaches zero at the light's `radius`. range <= 0
// is treated as unlimited (matches glTF).
float getRangeAttenuation(float range, float distance)
{
    if (range <= 0.0) return 1.0;
    float rangeRolloff = max(min(1.0 - pow(distance / range, 4.0), 1.0), 0.0);
    return rangeRolloff / max(distance * distance, 1e-6);
}

void main()
{
    ivec2 pix = ivec2(gl_FragCoord.xy);

    // Sky / no-geometry path. Depth==1 (the gbuffer-clear value with
    // standard 0..1 depth range) means no triangle rasterized here, so
    // skip the whole reconstruction. Output a flat dark gray for now —
    // a proper sky / env-map sample can drop in later.
    float depth = texelFetch(depthBufferTex, pix, 0).r;
    if (depth >= 1.0)
    {
        fragColor = vec4(0.02, 0.02, 0.025, 1.0);
        return;
    }

    uint vis      = texelFetch(visibilityBuffer, pix, 0).r;
    uint objectId = unpack_object_id(vis);
    uint primId   = unpack_primitive_id(vis);

    UniformObject obj = sceneObjects[objectId];

    BindlessTriangleVtx vtx;
    FetchBindlessTriangle(objectId, primId, vtx);

    // Project all 3 vertices through the same MVP the gbuffer used so the
    // Hawkins NDC coordinates align with gl_FragCoord / screenPosNdc.
    mat4 mvp   = projMat * viewMat * obj.modelMat;
    vec4 clip0 = mvp * vec4(vtx.posL[0], 1.0);
    vec4 clip1 = mvp * vec4(vtx.posL[1], 1.0);
    vec4 clip2 = mvp * vec4(vtx.posL[2], 1.0);

    vec2 twoOverWS = vec2(2.0) / viewportSize;
    BarycentricDeriv bary =
        CalcFullBary(clip0, clip1, clip2, screenPosNdc, twoOverWS);

    GradientInterpolationResults uvr =
        Interpolate2DWithDeriv(bary, vtx.uv[0], vtx.uv[1], vtx.uv[2]);

    DiffuseMaterial mat = opaqueMaterial[MATERIAL_ID(obj)];
    uint diffSlot = DiffuseMaterial_DiffuseTexture_ID(mat);
    vec3 albedo   = vec3(0.5);
    if (diffSlot != INVALID_TEXTURE_INDEX)
    {
        albedo = textureGrad(
            sampler2D(textures_2d[nonuniformEXT(diffSlot)], materialSampler),
            uvr.interp, uvr.dx, uvr.dy
        ).rgb;
    }

    // World normal — packed-oct from the gbuffer normal target.
    uint nrmPacked   = texelFetch(normalBufferTex, pix, 0).r;
    vec3 worldNormal = decompress_unit_vec(nrmPacked);

    // World position reconstructed straight from the visibility-buffer
    // triangle: transform the three local-space verts to world space and
    // interpolate with the same perspective-correct Hawkins barycentrics
    // already in scope. This is exact at the fragment center (no
    // depth-buffer precision loss / matrix-inversion wobble), so the
    // shadow-ray origin is stable as the camera moves.
    vec3 worldP0 = (obj.modelMat * vec4(vtx.posL[0], 1.0)).xyz;
    vec3 worldP1 = (obj.modelMat * vec4(vtx.posL[1], 1.0)).xyz;
    vec3 worldP2 = (obj.modelMat * vec4(vtx.posL[2], 1.0)).xyz;
    vec3 worldPos = InterpolateWithDeriv_float3x3_rows(
        bary, worldP0, worldP1, worldP2);

    // uv01 still needed below for the surfel-indirect texture sample.
    vec2 uv01 = (vec2(pix) + vec2(0.5)) / viewportSize;

    // Direct lighting: deterministic sum over all point lights. One shadow
    // ray per in-range light per pixel. The previous 1-of-N stochastic pick
    // with /pdf compensation was unbiased only with temporal accumulation,
    // which isn't wired up yet — so iterate.
    vec3 direct = vec3(0.0);
    for (uint i = 0u; i < pointLightCount; ++i)
    {
        PointLight pl = pointLights[i];

        vec3 toL = pl.position - worldPos;
        float d  = length(toL);
        // Range cull first — skips the shadow ray for lights that can't
        // reach this pixel.
        if (d <= 0.0 || d > pl.radius) continue;

        vec3 L   = toL / d;
        float ndl = max(dot(worldNormal, L), 0.0);
        if (ndl <= 0.0) continue;

        // Closest-hit shadow test, light -> surface. We don't bias the
        // ray to dodge the receiver triangle; instead we let the
        // traversal report whatever it hits closest, and treat a hit on
        // the receiver itself (matched by (objectId, primId) against
        // the visibility buffer for this pixel) as "no occluder".
        // Lights embedded in or placed behind geometry still occlude
        // correctly because the enclosing mesh's triangles will be hit
        // before the receiver.
        PtPayload hit;
        ClosestHit(Ray(pl.position, -L), hit);
        bool selfHit = hit.instanceCustomIndex == int(objectId) &&
                       hit.primitiveID         == int(primId);
        if (hit.hitT < INFINITY && !selfHit) continue;

        float attenuation = getRangeAttenuation(pl.radius, d);
        // Lambert BRDF = 1/pi (albedo applied at finalColor).
        direct += pl.color * pl.intensity * attenuation * ndl * M_1_OVER_PI;
    }

    // Indirect from the surfel cache — surfel_generate pre-computed the
    // per-pixel gather at half-res into surfelIndirect. The linear sampler
    // does the upsample; no per-fragment cell iteration needed here.
    vec3 indirect = texture(surfelIndirect, uv01).rgb;

    vec3 finalColor = albedo * (direct + indirect);

    // Uncharted 2 tonemap + manual linear->sRGB (swapchain is UNORM).
    vec3 mapped = toneMapUncharted(finalColor);
    fragColor   = vec4(mapped, 1.0);
}
