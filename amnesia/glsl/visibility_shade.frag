#version 460

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_ray_query : require

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

#include "host_device.h"
#include "common.glsl"               // unpack_object_id, unpack_primitive_id
#include "compress.glsl"
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"
#include "shaderUtils.glsl"          // WorldPosFromDepth
#include "random.glsl"               // tea, rand
// shaderUtils_surfel_cell.glsl transitively pulls in shaderUtil_grid.glsl
// (no include guard on the latter), so don't include it directly here.
#include "shaderUtils_surfel_cell.glsl" // isCellValid, getCellPos*, neighborOffset
#include "hawkins.glsl"
#include "bindless_triangle.glsl"

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
// barrier in HybridRenderer.cpp orders the build against this read.
layout(set = 2, binding = 4) uniform accelerationStructureEXT shadowTLAS;

layout(location = 0) in  vec2 screenPosNdc;
layout(location = 0) out vec4 fragColor;

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

bool shadowRayBlocked(vec3 origin, vec3 dir, float tMax)
{
    rayQueryEXT q;
    rayQueryInitializeEXT(
        q, shadowTLAS,
        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT
            | gl_RayFlagsSkipClosestHitShaderEXT,
        0xFFu, origin, 0.0, dir, tMax);
    while (rayQueryProceedEXT(q)) {}
    return rayQueryGetIntersectionTypeEXT(q, true)
        != gl_RayQueryCommittedIntersectionNoneEXT;
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

    // World position from depth — much simpler than the .fsl's
    // clip-space-z reconstruction since we have the actual depth buffer.
    vec2 uv01    = (vec2(pix) + vec2(0.5)) / viewportSize;
    vec3 worldPos = WorldPosFromDepth(uv01, depth, invProjMat, invViewMat);

    // Direct lighting via NEE: pick one random point light per pixel and
    // divide the contribution by the selection PDF (1/N) so the estimator
    // stays unbiased. O(1) shadow ray per pixel; the trade-off is visible
    // per-frame noise without temporal accumulation.
    vec3 direct = vec3(0.0);
    if (pointLightCount > 0u)
    {
        const vec3 rayOrigin = OffsetRayGbuffer(worldPos, worldNormal, depth);

        uint seed = tea(uint(pix.x) | (uint(pix.y) << 16), totalFrames);
        uint lightIdx = min(uint(rand(seed) * float(pointLightCount)),
                            pointLightCount - 1u);
        float lightPdf = 1.0 / float(pointLightCount);
        PointLight pl = pointLights[lightIdx];

        vec3 toL = pl.position - rayOrigin;
        float d  = length(toL);
        // Early-out for out-of-range picks; getRangeAttenuation would also
        // zero them but skipping the shadow ray is a real perf win.
        if (d > 0.0 && d <= pl.radius)
        {
            vec3 L   = toL / d;
            float ndl = max(dot(worldNormal, L), 0.0);
            if (ndl > 0.0 && !shadowRayBlocked(rayOrigin, L, d - 1e-3))
            {
                float attenuation = getRangeAttenuation(pl.radius, d);
                // Lambert BRDF = 1/pi (albedo applied below at line 179).
                // /= lightPdf compensates for uniform 1-of-N selection.
                direct = pl.color * pl.intensity * attenuation * ndl
                       * M_1_OVER_PI / lightPdf;
            }
        }
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
