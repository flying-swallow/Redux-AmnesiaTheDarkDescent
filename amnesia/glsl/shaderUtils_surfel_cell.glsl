#include "shaderUtil_grid.glsl"
// Surfel and cells

//vec3 getCameraPosition(SceneCamera camera)
//{
//    return vec3(camera.viewInverse[3]);
//}


bool isCellValid(vec3 cellPos)
{
    if (abs(cellPos.x) >= kCellDimension / 2)
        return false;
    if (abs(cellPos.y) >= kCellDimension / 2)
        return false;
    if (abs(cellPos.z) >= kCellDimension / 2)
        return false;

    return true;
}

bool isSurfelIntersectCell(Surfel surfel, vec3 cellPos, vec3 cameraPosW)
{
    if (!isCellValid(cellPos))
        return false;

    vec3 minPosW = cellPos * cellSize - vec3(cellSize) / 2.0f + cameraPosW;
    vec3 maxPosW = cellPos * cellSize + vec3(cellSize) / 2.0f + cameraPosW;
    vec3 closePoint = min(max(surfel.position, minPosW), maxPosW);

    float dist = distance(closePoint, surfel.position);

    return dist < surfel.radius;
}

float calcRadiusApprox(float area, float distance, float fovy, vec2 resolution) {
    float angle = sqrt(area / 3.14159265359) * fovy * 2.0 / max(resolution.x, resolution.y);
    return distance * tan(angle);
}

float calcSurfelRadius(float distance, float fovy, vec2 resolution) {
    return calcRadiusApprox(surfelSize, distance, fovy, resolution);
}

vec3 calcCellIndirectLighting(vec3 camPos, vec3 worldPos, vec3 worldNor)
{
    //vec3 cellPosIndex = getCellPos(worldPos, camPos);
    //uint flattenIndex = getFlattenCellIndex(cellPosIndex);

    ivec4 cellPosIndex = getCellPosNonUniform(worldPos, camPos);
    uint flattenIndex = getFlattenCellIndexNonUniform(cellPosIndex);

    CellInfo cellInfo = cellBuffer[flattenIndex];
    uint cellOffset = cellInfo.surfelOffset;
    uint cellSurfelCount = cellInfo.surfelCount;

	vec3 indirectLighting = vec3(0.0f);

    for (uint i = 0; i < cellSurfelCount; i++)
    {
        //uint surfelIndex = surfelAlive[i];
        uint surfelIndex = cellToSurfel[cellOffset + i];
        Surfel surfel = surfelBuffer[surfelIndex];
        vec3 bias = worldPos - surfel.position;
        float dist2 = dot(bias, bias);

        if (dist2 < surfel.radius * surfel.radius)
        {
            vec3 surfelNor = decompress_unit_vec(surfel.normal);
            float dotN = dot(worldNor, surfelNor);
            if (dotN > 0.f)
            {
                float dist = sqrt(dist2);
                float contribution = 1.f;

                contribution *= clamp(dotN, 0.f, 1.f);
                contribution *= clamp(1.f - dist / surfel.radius, 0.f, 1.f);
                contribution = smoothstep(0, 1, contribution);

                indirectLighting += surfel.radiance * contribution * smoothstep(0.f, 50.f, float(surfelRecycleInfo[surfelIndex].frame));
            }

        }
    }

    return indirectLighting;
}

const vec3 neighborOffset[27] = vec3[27](
    // z = -1
    vec3(-1, -1, -1),
    vec3(-1, 0, -1),
    vec3(-1, 1, -1),
    vec3(0, -1, -1),
    vec3(0, 0, -1),
    vec3(0, 1, -1),
    vec3(1, -1, -1),
    vec3(1, 0, -1),
    vec3(1, 1, -1),
    // z = 0
    vec3(-1, -1, 0),
    vec3(-1, 0, 0),
    vec3(-1, 1, 0),
    vec3(0, -1, 0),
    vec3(0, 0, 0),
    vec3(0, 1, 0),
    vec3(1, -1, 0),
    vec3(1, 0, 0),
    vec3(1, 1, 0),
    // z = 1
    vec3(-1, -1, 1),
    vec3(-1, 0, 1),
    vec3(-1, 1, 1),
    vec3(0, -1, 1),
    vec3(0, 0, 1),
    vec3(0, 1, 1),
    vec3(1, -1, 1),
    vec3(1, 0, 1),
    vec3(1, 1, 1)
    );



#ifdef LAYOUTS_GLSL

// Bindless surfel-ray path tracer.
//
// History: the original version (still in git) implemented a full Disney /
// GLTF pathtracer on top of the NVIDIA RTX sample scene model. The bindless
// renderer doesn't carry that scene representation, so the path is now a
// Lambertian + bindless-material trace using:
//   - sceneObjects[] / opaqueMaterial[] / textures_2d[] from bindless.resource.glsl
//   - opaque{Position,Normal,Uv0,Index}Handles[]  for BDA vertex pulling
//   - pointLights[] / pointLightCount        for direct lighting
//   - topLevelAS                             from layouts.glsl
// All Disney/GLTF/sun-sky/env paths have been dropped.

// BDA aliases for the per-instance vertex/index buffers. Names purposely kept
// distinct from gbuffer.vert's local aliases so this header can coexist if a
// future shader includes both.
layout(buffer_reference, scalar) readonly buffer RTPositionBuf { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer RTNormalBuf   { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer RTUv0Buf      { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer RTIndexBuf    { uint v[]; };

struct BindlessHit
{
    vec3 posW;
    vec3 normalW;
    vec2 uv;
    vec3 albedo;
    vec3 emissive;
};

// Resolve a TLAS hit (described by prd) into world-space shade state pulled
// from the bindless object/material tables. Returns zero-ish defaults for any
// missing stream so a misconfigured object can't NaN-out the path.
BindlessHit GetBindlessHit(in PtPayload p)
{
    BindlessHit h;

    // Index by instanceCustomIndex, not instanceID. The TLAS build sets
    // `inst.instanceCustomIndex = req.id` (HybridRenderer.cpp:1050), where
    // req.id is the engine's bindless object-slot ID — the same ID that
    // sceneObjects[], opaque*Handles[], and opaqueMaterial[] are keyed by.
    // instanceID is the TLAS-build-order index and has no relation to
    // the bindless slot, so using it here would read whichever object
    // happens to occupy that slot in TLAS build order (wrong material,
    // wrong BDA buffers, wrong UVs → black albedo, garbage hit positions
    // that cause shadow rays to self-occlude every direct light).
    const uint instanceId = uint(p.instanceCustomIndex);
    const UniformObject obj = sceneObjects[instanceId];

    const uint64_t indexAddr = opaqueIndexHandles.data[instanceId];
    const uint64_t posAddr   = opaquePositionHandles.data[instanceId];
    const uint64_t nrmAddr   = opaqueNormalHandles.data[instanceId];
    const uint64_t uvAddr    = opaqueUv0Handles.data[instanceId];

    // Triangle indices — 3 consecutive uints per primitive.
    const uint idxBase = uint(p.primitiveID) * 3u;
    uvec3 tri = uvec3(0u);
    if (indexAddr != 0ul) {
        RTIndexBuf ib = RTIndexBuf(indexAddr);
        tri = uvec3(ib.v[idxBase + 0u], ib.v[idxBase + 1u], ib.v[idxBase + 2u]);
    } else {
        tri = uvec3(idxBase, idxBase + 1u, idxBase + 2u);
    }

    const vec3 bary = vec3(1.0 - p.baryCoord.x - p.baryCoord.y,
                           p.baryCoord.x,
                           p.baryCoord.y);

    vec3 posL = vec3(0.0);
    if (posAddr != 0ul) {
        RTPositionBuf pb = RTPositionBuf(posAddr);
        posL = pb.v[tri.x].xyz * bary.x +
               pb.v[tri.y].xyz * bary.y +
               pb.v[tri.z].xyz * bary.z;
    }

    vec3 normalL = vec3(0.0, 0.0, 1.0);
    if (nrmAddr != 0ul) {
        RTNormalBuf nb = RTNormalBuf(nrmAddr);
        normalL = nb.v[tri.x] * bary.x +
                  nb.v[tri.y] * bary.y +
                  nb.v[tri.z] * bary.z;
    }

    vec2 uv = vec2(0.0);
    if (uvAddr != 0ul) {
        RTUv0Buf ub = RTUv0Buf(uvAddr);
        uv = ub.v[tri.x].xy * bary.x +
             ub.v[tri.y].xy * bary.y +
             ub.v[tri.z].xy * bary.z;
    }

    h.posW    = (obj.modelMat * vec4(posL, 1.0)).xyz;
    h.normalW = normalize(mat3(obj.invModelMat) * normalL);
    h.uv      = uv;

    // Material -> albedo: sample tex[0] (Diffuse). Missing slot -> mid-gray
    // so geometry stays visible.
    const DiffuseMaterial mat = opaqueMaterial[obj.materialID];
    const uint diffuseSlot = DiffuseMaterial_DiffuseTexture_ID(mat);
    if (diffuseSlot != INVALID_TEXTURE_INDEX) {
        h.albedo = texture(sampler2D(textures_2d[nonuniformEXT(diffuseSlot)],
                                     materialSampler),
                           h.uv).rgb;
    } else {
        h.albedo = vec3(0.5);
    }

    // Emissive contribution: illumination texture * per-object scalar. The
    // engine drives `illuminationAmount` per object (e.g. flickering torches),
    // so a single-texture sample is enough; surfels integrate emission as
    // bounce-1 radiance and propagate it the same way as point-light direct.
    h.emissive = vec3(0.0);
    const uint illumSlot = DiffuseMaterial_IlluminiationTexture_ID(mat);
    if (illumSlot != INVALID_TEXTURE_INDEX && obj.illuminationAmount > 0.0) {
        h.emissive = texture(sampler2D(textures_2d[nonuniformEXT(illumSlot)],
                                       materialSampler),
                             h.uv).rgb * obj.illuminationAmount;
    }

    return h;
}

bool finalizePathWithSurfel(vec3 worldPos, vec3 worldNor, uint randSeed, inout vec4 irradiance)
{
    irradiance = vec4(0.0f);
    vec3 camPos = invViewMat[3].xyz;
    //vec3 cellPosIndex = getCellPos(worldPos, camPos);
    ivec4 cellPosIndex = getCellPosNonUniform(worldPos, camPos);
    if (!isCellValid(cellPosIndex))
        return false;

    uint flattenIndex = getFlattenCellIndexNonUniform(cellPosIndex);

    CellInfo cellInfo = cellBuffer[flattenIndex];
    uint cellOffset = cellInfo.surfelOffset;
    uint cellSurfelCount = cellInfo.surfelCount;

    float coverage = 0.f;
    float maxContribution = 0.f;
    uint maxContributionSleepingSurfelIndex = 0xffffffff;

    const uint searchRange = min(16, cellInfo.surfelCount);
	uint searchCnt = 0;

    //uint randSeed = initRandom(uvec2(rtxState.frame, floatBitsToUint(worldPos.x)),
    //    uvec2(floatBitsToUint(worldPos.y), floatBitsToUint(worldPos.z)), rtxState.totalFrames);

	uint targetCnt = min(64, cellInfo.surfelCount);
	float surfelCntF = float(cellInfo.surfelCount);

    for (uint i = 0; i < targetCnt; i++)
    {
        uint currIndex = targetCnt == cellInfo.surfelCount ? i : uint(rand(randSeed) * surfelCntF);
        //uint currIndex = i;

        uint surfelIndex = cellToSurfel[cellOffset + currIndex];
        Surfel surfel = surfelBuffer[surfelIndex];
        vec3 neiNor = decompress_unit_vec(surfel.normal);
        bool isSleeping = (surfelRecycleInfo[surfelIndex].status & 0x0001) != 0;
        vec3 bias = surfel.position - worldPos;
		float dist = length(bias);
        float cosineTheta = dot(bias, worldNor) / dist;
        if (cosineTheta < -0.2 || dot(-bias, neiNor) / dist < -0.2)
            continue;

        if (dist < surfel.radius)
        {
            vec3 surfelNor = decompress_unit_vec(surfel.normal);
            float dotN = dot(worldNor, surfelNor);
            float contribution = 1.f;
            if (dotN > 0.f)
            {

                contribution *= clamp(dotN, 0.f, 1.f);
                contribution *= clamp(1.f - dist / surfel.radius, 0.f, 1.f);
                contribution = smoothstep(0, 1, contribution);

                irradiance += vec4(surfel.radiance, 1.f) * contribution;
				coverage += contribution;
                if (maxContribution < contribution && isSleeping)
                {
                    maxContribution = contribution;
                    maxContributionSleepingSurfelIndex = surfelIndex;
                }

                /*if (isSleeping)
                {
                    sleepingCoverage += contribution;
                    if (maxContribution < contribution)
                    {
                        maxContribution = contribution;
                        maxContributionSleepingSurfelIndex = surfelIndex;
                    }
                }*/
            }
            else
            {
                contribution *= max(cosineTheta, 0.f);
                contribution *= pow(1.f - dist / surfel.radius, 2.0);
                irradiance += vec4(surfel.radiance, 1.f) * contribution;
            }
            surfelRecycleInfo[surfelIndex].status |= 0x0004u;
        }


    }

	if (irradiance.w > 0.1f)
	{
		irradiance /= irradiance.w;
	}
    else
	{
		return false;
	}

    return true;
}

// Surfel-ray path trace.
//
// One ray per call. Per bounce: trace, fetch bindless shade state, accumulate
// direct lighting from the bindless PointLight SSBO (with shadow ray), bounce
// cosine-weighted Lambertian. If we run out of depth we fall back to the
// surfel cache via finalizePathWithSurfel().
//
// Environment misses contribute zero (no IBL in the bindless model).
// `surfelIndex` is the surfel owning this ray; used to seed RNG and to
// constrain the surfel-cache fallback so we don't read a surfel's own
// radiance back into itself.
vec3 surfelPathTrace(Ray r, int maxDepth, uint surfelIndex, inout float firstDepth, inout PtPayload prd)
{
    vec3 radiance   = vec3(0.0);
    vec3 throughput = vec3(1.0);
    bool valid      = true;
    int  depth;
    BindlessHit lastHit;
    lastHit.posW     = vec3(0.0);
    lastHit.normalW  = vec3(0.0, 0.0, 1.0);
    lastHit.uv       = vec2(0.0);
    lastHit.albedo   = vec3(0.0);
    lastHit.emissive = vec3(0.0);

    for (depth = 0; depth < maxDepth; depth++)
    {
        ClosestHit(r, prd);
        if (depth == 0)
        {
            firstDepth = prd.hitT;
        }

        // Miss → zero env contribution (no IBL in the bindless model yet).
        if (prd.hitT >= INFINITY)
        {
            valid = false;
            break;
        }

        BindlessHit hit = GetBindlessHit(prd);
        lastHit = hit;

        // Front-face normal: flip the shading normal toward the ray origin
        // when we hit a back-face / thin two-sided polygon. Without this,
        // dot(normal, L) and the cosine-weighted bounce can use a normal
        // pointing into the surface, which produces signed throughput and
        // negative accumulated radiance.
        vec3 ffnormal = dot(hit.normalW, r.direction) <= 0.0
                            ? hit.normalW : -hit.normalW;

        radiance += throughput * hit.emissive;

        // NEE: pick one directional (point or spot) light per bounce from the
        // combined pool instead of looping all. Box lights are add-blend
        // ambient (no shadow ray, no cone) and accumulate deterministically
        // after the bounce — kept out of the stochastic pool because their
        // contribution doesn't depend on the chosen direction.
        //
        // Contribution is staged here; the shadow ray fires after we've
        // already updated the next-bounce ray (deferred shadow), which
        // keeps live state across AnyHit minimal per the RTX best-practice.
        vec3 pendingDirect = vec3(0.0);
        vec3 pendingShadowOrigin = vec3(0.0);
        vec3 pendingShadowDir    = vec3(0.0);
        float pendingShadowMaxT  = 0.0;
        bool pendingShadow = false;

        uint directionalLightCount = pointLightCount + spotLightCount;
        if (directionalLightCount > 0u)
        {
            uint lightIdx = min(uint(rand(prd.seed) * float(directionalLightCount)),
                                directionalLightCount - 1u);
            float lightPdf = 1.0 / float(directionalLightCount);

            if (lightIdx < pointLightCount)
            {
                PointLight pl = pointLights[lightIdx];

                vec3 toL = pl.position - hit.posW;
                float d  = length(toL);
                if (d > 0.0 && d <= pl.radius)
                {
                    vec3 L = toL / d;
                    float ndl = max(dot(ffnormal, L), 0.0);
                    if (ndl > 0.0)
                    {
                        float falloff = 1.0 - smoothstep(0.0, pl.radius, d);
                        pendingDirect = throughput * hit.albedo * pl.color * pl.intensity
                                      * ndl * falloff * M_1_OVER_PI / lightPdf;
                        pendingShadowOrigin = OffsetRayBindless(hit.posW, ffnormal);
                        pendingShadowDir    = L;
                        pendingShadowMaxT   = max(d - 0.01, 0.0);
                        pendingShadow = true;
                    }
                }
            }
            else
            {
                SpotLight sl = spotLights[lightIdx - pointLightCount];

                vec3 toL = sl.position - hit.posW;
                float d  = length(toL);
                if (d > 0.0 && d <= sl.radius)
                {
                    vec3 L = toL / d;
                    // sl.direction is the light's outward forward; inside the
                    // cone when dot(-L, forward) >= cos(half-angle).
                    float cosTheta = dot(-L, sl.direction);
                    if (cosTheta >= sl.cosOuterAngle)
                    {
                        float ndl = max(dot(ffnormal, L), 0.0);
                        if (ndl > 0.0)
                        {
                            float falloff = 1.0 - smoothstep(0.0, sl.radius, d);

                            // Optional gobo projection through the light's
                            // view-projection. Mirrors visibility_shade.frag's
                            // spot path — outside the projected rect is zeroed.
                            vec3 gobo = vec3(1.0);
                            if (sl.goboTextureIndex != INVALID_TEXTURE_INDEX)
                            {
                                vec4 lc = sl.viewProjection * vec4(hit.posW, 1.0);
                                if (lc.w > 0.0)
                                {
                                    vec3 ndc = lc.xyz / lc.w;
                                    vec2 uv  = ndc.xy * 0.5 + 0.5;
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

                            pendingDirect = throughput * hit.albedo * sl.color * sl.intensity
                                          * ndl * falloff * gobo * M_1_OVER_PI / lightPdf;
                            pendingShadowOrigin = OffsetRayBindless(hit.posW, ffnormal);
                            pendingShadowDir    = L;
                            pendingShadowMaxT   = max(d - 0.01, 0.0);
                            pendingShadow = true;
                        }
                    }
                }
            }
        }

        // Box lights — add-blend volumetric tints. No shadow ray, no cone:
        // the pixel is "inside" when |hit.posW - center| <= halfSize per
        // axis, and the contribution is just color modulated by
        // throughput*albedo (matching visibility_shade.frag's albedo*box
        // term). Alpha is intentionally not used — the legacy
        // deferred_light_box.frag.fsl discards it.
        for (uint bi = 0u; bi < boxLightCount; ++bi)
        {
            BoxLight bl = boxLights[bi];
            if (any(greaterThan(abs(hit.posW - bl.center), bl.halfSize))) continue;
            radiance += throughput * hit.albedo * bl.color;
        }

        // Cosine-weighted Lambertian bounce, sampled around ffnormal so
        // back-face hits don't produce samples pointing into the surface.
        vec3 T, B;
        CreateCoordinateSystem(ffnormal, T, B);
        vec2 u = rand2(prd.seed);
        vec3 localDir = CosineSampleHemisphere(u.x, u.y);
        vec3 newDir = normalize(localDir.x * T + localDir.y * B + localDir.z * ffnormal);
        throughput *= hit.albedo;

        r.origin    = OffsetRay(hit.posW, ffnormal);
        r.direction = newDir;

        // Deferred shadow ray for the NEE sample chosen above.
        if (pendingShadow)
        {
            Ray shadowRay = Ray(pendingShadowOrigin, pendingShadowDir);
            if (!AnyHit(topLevelAS, shadowRay, pendingShadowMaxT))
            {
                radiance += pendingDirect;
            }
        }

        // Russian roulette: from depth 2 onward, terminate paths whose
        // throughput has decayed. Survivors get /= rrPcont so the estimator
        // stays unbiased.
        if (depth >= 2)
        {
            float rrPcont = clamp(max(throughput.x, max(throughput.y, throughput.z)),
                                  0.05, 0.95);
            if (rand(prd.seed) >= rrPcont) break;
            throughput /= rrPcont;
        }
    }

    // Surfel-cache fallback once we hit max depth.
    if (depth == maxDepth && valid)
    {
        vec3 surfelPos = surfelBuffer[surfelIndex].position;
        float radius   = surfelBuffer[surfelIndex].radius;
        if (dot(lastHit.posW, surfelPos) < radius * radius)
        {
            vec4 irradiance = vec4(0.0);
            uint randSeed   = tea(surfelIndex, totalFrames);
            if (finalizePathWithSurfel(lastHit.posW, lastHit.normalW, randSeed, irradiance))
            {
                radiance += irradiance.xyz * throughput;
            }
        }
    }

    return radiance;
}


#endif // LAYOUTS_GLSL
