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

#include "forward_shared.h"
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
#include "light_falloff.glsl"        // sampleAttenuation
#include "fresnel.glsl"              // Fresnel (legacy bias/pow fit)
#include "parallax.glsl"             // ParallaxAdvance + PARALLAX_MULTIPLIER

// Uncharted-2 tonemap. Maps open-ended HDR (multiple lights, specular peaks)
// down into [0,1] before the SRGB swapchain quantizes and encodes. Stays in
// linear space — the hardware does the linear→sRGB transfer at swapchain write.
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
    return c * whiteScale;
}


// Spot/point light falloff helpers (sampleAttenuation, sampleSpotCone) live
// in light_falloff.glsl (included above). The direct (this file) and surfel
// NEE paths both use them so the two paths can't drift apart on attenuation.

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

    // Apply the per-object UV transform the gbuffer used (gbuffer.vert:61) so
    // texture samples in this composite land at the same UV the gbuffer
    // alpha-tested at. Skipping uvMat was a silent mismatch — invisible for
    // identity uvMat (most static walls) but wrong for animated/tiled/scrolled
    // materials.
    mat4 uvM = obj.uvMat;
    vec2 uv0 = (uvM * vec4(vtx.uv[0], 0.0, 1.0)).xy;
    vec2 uv1 = (uvM * vec4(vtx.uv[1], 0.0, 1.0)).xy;
    vec2 uv2 = (uvM * vec4(vtx.uv[2], 0.0, 1.0)).xy;
    GradientInterpolationResults uvr =
        Interpolate2DWithDeriv(bary, uv0, uv1, uv2);

    DiffuseMaterial mat = opaqueMaterial[MATERIAL_ID(obj)];

    // World position reconstructed straight from the visibility-buffer
    // triangle: transform the three local-space verts to world space and
    // interpolate with the same perspective-correct Hawkins barycentrics
    // already in scope. Exact at the fragment center, so the shadow-ray
    // origin is stable as the camera moves.
    vec3 worldP0 = (obj.modelMat * vec4(vtx.posL[0], 1.0)).xyz;
    vec3 worldP1 = (obj.modelMat * vec4(vtx.posL[1], 1.0)).xyz;
    vec3 worldP2 = (obj.modelMat * vec4(vtx.posL[2], 1.0)).xyz;
    vec3 worldPos = InterpolateWithDeriv_float3x3_rows(
        bary, worldP0, worldP1, worldP2);

    uint nrmPacked   = texelFetch(normalBufferTex, pix, 0).r;
    vec3 worldNormal = decompress_unit_vec(nrmPacked);

    // Tangent-space basis — bit-for-bit matching gbuffer.vert:63-66.
    // Per-vertex normalize → Hawkins interpolate → final normalize. The
    // bitangent is computed via cross-IN-LOCAL-space then transformed by
    // normalMat (cross(local_tangent, local_normal) per vertex). Doing
    // cross-in-world after transform would diverge under non-uniform scale
    // (stretched walls) and put B out of the tangent plane — that was the
    // residual vertical-surface warping cause.
    mat3 normalMat = transpose(mat3(obj.invModelMat));

    vec3 wT0 = normalize(normalMat * vtx.tangentL[0].xyz);
    vec3 wT1 = normalize(normalMat * vtx.tangentL[1].xyz);
    vec3 wT2 = normalize(normalMat * vtx.tangentL[2].xyz);
    vec3 T = normalize(InterpolateWithDeriv_float3x3_rows(bary, wT0, wT1, wT2));

    vec3 wB0 = normalize(normalMat * cross(vtx.tangentL[0].xyz, vtx.normalL[0]));
    vec3 wB1 = normalize(normalMat * cross(vtx.tangentL[1].xyz, vtx.normalL[1]));
    vec3 wB2 = normalize(normalMat * cross(vtx.tangentL[2].xyz, vtx.normalL[2]));
    vec3 B = normalize(InterpolateWithDeriv_float3x3_rows(bary, wB0, wB1, wB2));

    //uint heightSlot = DiffuseMaterial_HeightTexture_ID(mat);
    //if (heightSlot != INVALID_TEXTURE_INDEX)
    //{
    //    // Surface→camera in world space. ParallaxAdvance is space-agnostic —
    //    // it only needs (dir, N, T, B) consistent with each other, which they
    //    // are here (all world space). Magnitude is irrelevant; the helper
    //    // normalizes after WorldSpaceToTangent.
    //    vec3 dir =   invViewMat[3].xyz - worldPos;
    //    // materialConfig bit 9: heightmap is single-channel (.r) vs alpha-
    //    // packed (.a). Set host-side in MaterialResource.cpp when a Height
    //    // texture is bound.
    //    bool isSingleChannel = (mat.materialConfig & (1u << 9)) != 0u;
    //    vec2 dUV = ParallaxAdvance(uvr.interp, 0.0, 32.0,
    //                               mat.heightMapScale * PARALLAX_MULTIPLIER,
    //                               dir, worldNormal, T, B,
    //                               isSingleChannel, heightSlot);
    //    uvr.interp += dUV;
    //}

    // Diffuse sample (parallax-offset UV).
    uint diffSlot = DiffuseMaterial_DiffuseTexture_ID(mat);
    vec3 albedo   = vec3(0.5);
    if (diffSlot != INVALID_TEXTURE_INDEX)
    {
        albedo = textureGrad(
            sampler2D(textures_2d[nonuniformEXT(diffSlot)], materialSampler),
            uvr.interp, uvr.dx, uvr.dy
        ).rgb;
    }

    // Specular gbuffer surrogate: the visibility-buffer model resolves
    // materials deferred, so sample the material's specular texture inline.
    // Matches the base-game `specularMap.xy` (intensity, packed-power) read
    // from the deferred gbuffer. Missing slot → no specular.
    uint specSlot = DiffuseMaterial_SpecularTexture_ID(mat);
    vec2 specularTexel = vec2(0.0);
    if (specSlot != INVALID_TEXTURE_INDEX)
    {
        specularTexel = textureGrad(
            sampler2D(textures_2d[nonuniformEXT(specSlot)], materialSampler),
            uvr.interp, uvr.dx, uvr.dy
        ).xy;
    }

    // Tangent-space normal map. Override `worldNormal` before the light loops
    // so shadow rays, NdotL, and Blinn-Phong all see the perturbed normal.
    // Note the `- 0.5` (not `* 2 - 1`): legacy normal maps are stored as
    // `(n + 0.5)`, an asymmetric encoding. Preserving it avoids re-baking
    // every existing normal map asset.
    uint nrmSlot = DiffuseMaterial_NormalTexture_ID(mat);
    if (nrmSlot != INVALID_TEXTURE_INDEX)
    {
        vec3 nSample = textureGrad(
            sampler2D(textures_2d[nonuniformEXT(nrmSlot)], materialSampler),
            uvr.interp, uvr.dx, uvr.dy
        ).xyz - 0.5;
        worldNormal = normalize(
            nSample.x * T + nSample.y * B + nSample.z * worldNormal);
    }

    // Cube-map reflection + Fresnel, folded into albedo (matches legacy
    // `Out.diffuse = diffuseColor + reflectionColor * fFresnel` — the
    // reflection then rides through the same `albedo * (direct + indirect)`
    // modulation in the final composite).
    uint cubeSlot = DiffuseMaterial_CubeMapTexture_ID(mat);
    if (cubeSlot != INVALID_TEXTURE_INDEX)
    {
        vec3 V = normalize(invViewMat[3].xyz - worldPos);
        vec3 R = reflect(-V, worldNormal);
        vec3 envColor = texture(
            samplerCube(textures_cube[nonuniformEXT(cubeSlot)], materialSampler),
            R).rgb;
        float fres = Fresnel(max(dot(V, worldNormal), 0.0),
                             mat.frenselBias, mat.frenselPow);
        // Optional reflection mask (legacy `reflectionColor *= cubeMapAlpha.wwww`).
        uint cubeAlphaSlot = DiffuseMaterial_CubeMapAlphaTexture_ID(mat);
        if (cubeAlphaSlot != INVALID_TEXTURE_INDEX)
        {
            float mask = textureGrad(
                sampler2D(textures_2d[nonuniformEXT(cubeAlphaSlot)], materialSampler),
                uvr.interp, uvr.dx, uvr.dy
            ).a;
            envColor *= mask;
        }
        albedo += envColor * fres;
    }

    // uv01 still needed below for the surfel-indirect texture sample.
    vec2 uv01 = (vec2(pix) + vec2(0.5)) / viewportSize;

    // Direct lighting: deterministic sum over all point lights. One shadow
    // ray per in-range light per pixel.
    //
    // Lighting model matches the base-game deferred_light_pointlight.frag.fsl:
    //   - `pl.color` is the final diffuse RGB; `pl.intensity` is the specular
    //     flag/multiplier (cColor.a), NOT a diffuse multiplier.
    //   - Attenuation comes from a 1D-as-2D LUT keyed on (d/r)^2, falling back
    //     to saturate(1-(d/r)^2) when no map is bound. No 1/d^2 divisor.
    //   - No 1/pi Lambert normalization (legacy artistic balance).
    //   - Optional gobo cube projection in light-local space.
    //   - Optional Blinn-Phong specular sampled from the material's specular
    //     texture (above), accumulated separately so albedo doesn't dampen it.
    vec3 direct     = vec3(0.0);
    vec3 directSpec = vec3(0.0);
    vec3 viewDir    = normalize(invViewMat[3].xyz - worldPos);
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

        float r2 = (d * d) / (pl.radius * pl.radius);
        float attenuation = sampleAttenuation(pl.attenuationTextureIndex, r2);

        // Gobo cube projection in the light's local frame.
        vec3 gobo = vec3(1.0);
        if (pl.goboTextureIndex != INVALID_TEXTURE_INDEX)
        {
            mat3 wToL = mat3(pl.worldToLightX, pl.worldToLightY, pl.worldToLightZ);
            // Sample direction matches the base-game convention:
            // light → surface in light-local space (i.e. R^T * (-L)).
            vec3 lightLocalDir = wToL * (-L);
            gobo = texture(
                samplerCube(textures_cube[nonuniformEXT(pl.goboTextureIndex)],
                            materialSampler),
                lightLocalDir).rgb;
        }

        // Lambert BRDF: albedo / π. Albedo is applied at composite time
        // (`finalColor = albedo * direct + ...`), so the BRDF factor that
        // multiplies into `direct` here is just `M_1_OVER_PI`.
        direct += pl.color * gobo * attenuation * ndl;

        // Blinn-Phong specular. Gated on the legacy `lightColor.w > 0` flag
        // (now pl.intensity) AND the presence of a material specular map.
        if (pl.intensity > 0.0 && specSlot != INVALID_TEXTURE_INDEX)
        {
            vec3 H = normalize(L + viewDir);
            float specPower = exp2(specularTexel.y * 10.0) + 1.0;
            float specVal = pl.intensity * specularTexel.x
                          * pow(clamp(dot(H, worldNormal), 0.0, 1.0), specPower);
            directSpec += pl.color * gobo * specVal * attenuation;
        }
    }

    // Spot lights — range + cone cull, optional ray-traced shadow, optional
    // gobo projection through the light's view-projection matrix.
    for (uint i = 0u; i < spotLightCount; ++i)
    {
        SpotLight sl = spotLights[i];

        vec3 toL = sl.position - worldPos;
        float d  = length(toL);
        if (d <= 0.0 || d > sl.radius) continue;

        vec3 L = toL / d;

        float ndl = max(dot(worldNormal, L), 0.0);
        if (ndl <= 0.0) continue;

        PtPayload hit;
        ClosestHit(Ray(sl.position, -L), hit);
        bool selfHit = hit.instanceCustomIndex == int(objectId) &&
                        hit.primitiveID         == int(primId);
        if (hit.hitT < INFINITY && !selfHit) continue;

        // Radial attenuation: legacy 1D LUT keyed on (d/r)². When no LUT is
        // bound, sampleAttenuation falls back to saturate(1-(d/r)²) — matches
        // the canonical attenuationLightMap default and is NOT 1/d² physics.
        float r2 = (d * d) / (sl.radius * sl.radius);
        float attenuation = sampleAttenuation(sl.attenuationTextureIndex, r2);

        // Gobo projection — sample the light's view-projection in NDC, then
        // map [-1,1] → [0,1]. Pixels behind the light or outside the projected
        // rect are zeroed; cosTheta already guarantees in-cone. When a gobo
        // is bound it replaces the cone factor (matches legacy
        // deferred_light_spotlight.frag.fsl's `if (HasGoboMap) ... else cone`
        // branch); otherwise apply the 1D cone-falloff LUT (smoothstep
        // fallback inside sampleSpotCone when no LUT is bound).
        vec3  gobo = vec3(1.0);
        float cone = 1.0;
        if (sl.goboTextureIndex != INVALID_TEXTURE_INDEX)
        {
            vec4 lc = sl.viewProjection * vec4(worldPos, 1.0);
            if (lc.w > 0.0)
            {
                // g_mtxTextureUnitFix is already baked into sl.viewProjection
                // (see LightSpot::GetViewProjMatrix), so after the perspective
                // divide the xy coords are already in [0,1] — no extra bias.
                vec2 uv = lc.xy / lc.w;
                if (all(greaterThanEqual(uv, vec2(0.0))) &&
                    all(lessThanEqual(uv, vec2(1.0))))
                {
                    gobo = texture(
                        sampler2D(textures_2d[nonuniformEXT(sl.goboTextureIndex)],
                                  materialSampler),
                        uv).rgb;
                }
                else
                {
                    gobo = vec3(0.0);
                }
            }
            else
            {
                gobo = vec3(0.0);
            }
        }
        else
        {
            float cosTheta = dot(-L, sl.direction);
            if (cosTheta < sl.cosOuterAngle) continue;
            cone = sampleSpotCone(sl.coneFalloffTextureIndex, cosTheta, sl.cosOuterAngle);
        }

        // Diffuse: sl.intensity (cColor.a) is the legacy specular-only flag,
        // not a diffuse multiplier — matches deferred_light_spotlight.frag.fsl
        // and the point-light branch above. Lambert BRDF (`/π`) applied here
        // for parity with the point-light loop; albedo is applied at composite.
        direct += sl.color * attenuation * ndl * gobo * cone;

        // Blinn-Phong specular. Same gate as the point-light branch
        // (sl.intensity > 0 && material has a specular slot). gobo*cone
        // carries the cone/gobo modulation onto specular too, mirroring the
        // legacy `(diffuse + specular) * gobo * attenuation` composition.
        if (sl.intensity > 0.0 && specSlot != INVALID_TEXTURE_INDEX)
        {
            vec3 H = normalize(L + viewDir);
            float specPower = exp2(specularTexel.y * 10.0) + 1.0;
            float specVal = sl.intensity * specularTexel.x
                          * pow(clamp(dot(H, worldNormal), 0.0, 1.0), specPower);
            directSpec += sl.color * gobo * cone * specVal * attenuation;
        }
    }

    // Indirect from the surfel cache — surfel_generate pre-computed the
    // per-pixel gather at half-res into surfelIndirect. The linear sampler
    // does the upsample; no per-fragment cell iteration needed here.
    //
    // kIndirectScale: surfel-GI contribution scale. 1.0 = full physical
    // indirect after the Lambert-normalized NEE fix in
    // shaderUtils_surfel_cell.glsl::surfelPathTrace. Drop toward 0.0 to bring
    // the scene closer to legacy HPL2 (which has no indirect at all). Lives
    // here as a knob so levels can be tuned without rebuilding the surfel
    // passes.
    vec3 indirect = texture(surfelIndirect, uv01).rgb * BOUNCE_INDIRECT_SCALE;
    vec3 finalColor =  albedo * (direct + indirect) + directSpec;

    // Linear-throughout PBR (see header). Tonemap brings open-ended HDR into
    // [0,1] linear; the SRGB swapchain handles the linear→sRGB encode on write.
    vec3 mapped = toneMapUncharted(finalColor);
    fragColor   = vec4(mapped, 0);
}
