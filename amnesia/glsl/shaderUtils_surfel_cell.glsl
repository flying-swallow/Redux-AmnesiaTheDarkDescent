#include "shaderUtil_grid.glsl"
#include "light_falloff.glsl"  // sampleAttenuation (point-light NEE branch)
// Surfel and cells

//vec3 getCameraPosition(SceneCamera camera)
//{
//    return vec3(camera.viewInverse[3]);
//}


bool isCellValid(vec3 cellPos)
{
    if (abs(cellPos.x) >= CELL_GRID_DIM / 2)
        return false;
    if (abs(cellPos.y) >= CELL_GRID_DIM / 2)
        return false;
    if (abs(cellPos.z) >= CELL_GRID_DIM / 2)
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

                indirectLighting += surfel.radiance * contribution * smoothstep(0.f, 3.f, float(surfelRecycleInfo[surfelIndex].frame));
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
    vec2 specular; // Add specular: x = intensity, y = power
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

    // Specular texture: sample tex[3]
    h.specular = vec2(0.0);
    const uint specSlot = DiffuseMaterial_SpecularTexture_ID(mat);
    if (specSlot != INVALID_TEXTURE_INDEX) {
        h.specular = texture(sampler2D(textures_2d[nonuniformEXT(specSlot)],
                                       materialSampler),
                             h.uv).xy;
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

// Per-light NEE helpers. One function per light type — the geometry-, gobo-,
// and falloff-related branches are simply too different to share without
// turning the body into a tangle of `if`s. Both return `true` when the light
// would contribute (pre-shadow); the caller fires the deferred shadow ray
// and adds `contribution` if it survives.
//
// `lightPdf` is the inverse selection probability for the picked light from
// the combined directional pool (1/(pointLightCount+spotLightCount)). Folded
// into `contribution` here so the caller doesn't have to remember which
// branch needed it.
//
// `throughput * albedo * M_1_OVER_PI` is the Lambert BRDF factor at the
// shaded hit; `lightDir` is the surface→light unit vector returned for the
// caller's shadow-ray setup. `lightPos` is just `light.position`, returned
// alongside so the caller can build the shadow ray without re-reading the
// SSBO.
bool neePoint(in PointLight pl,
              in vec3 posW, in vec3 ffnormal,
              in vec3 albedo, in vec3 throughput,
              in float lightPdf,
              in vec3 V, in vec2 specular,
              out vec3 contribution,
              out vec3 lightPos,
              out vec3 lightDir)
{
    contribution = vec3(0.0);
    lightPos     = pl.position;
    lightDir     = vec3(0.0);

    vec3 toL = pl.position - posW;
    float d  = length(toL);
    if (d <= 0.0 || d > pl.radius) return false;

    vec3 L = toL / d;
    float ndl = max(dot(ffnormal, L), 0.0);
    if (ndl <= 0.0) return false;

    // Legacy 1D-as-2D LUT keyed on (d/r)², analytic saturate(1-(d/r)²)
    // fallback. Matches visibility_shade.frag's direct point loop.
    float r2 = (d * d) / (pl.radius * pl.radius);
    float attenuation = sampleAttenuation(pl.attenuationTextureIndex, r2);

    vec3 gobo = vec3(1.0);
    if (pl.goboTextureIndex != INVALID_TEXTURE_INDEX)
    {
        mat3 wToL = mat3(pl.worldToLightX,
                         pl.worldToLightY,
                         pl.worldToLightZ);
        vec3 lightLocalDir = wToL * (-L);
        gobo = texture(
            samplerCube(textures_cube[nonuniformEXT(pl.goboTextureIndex)],
                        materialSampler),
            lightLocalDir).rgb;
    }

    // Lambert BRDF: albedo / π.
    vec3 diffuseTerm = albedo * ndl;

    // Blinn-Phong specular term with energy normalization.
    vec3 specularTerm = vec3(0.0);
    if (pl.intensity > 0.0 && specular.x > 0.0)
    {
        vec3 H = normalize(L + V);
        float specPower = exp2(specular.y * 10.0) + 1.0;
        float normFactor = (specPower + 2.0) * 0.5;
        float specVal = pl.intensity * specular.x * normFactor
                      * pow(clamp(dot(H, ffnormal), 0.0, 1.0), specPower);
        specularTerm = vec3(specVal);
    }

    contribution = throughput * (diffuseTerm + specularTerm)
                 * pl.color * gobo * attenuation / lightPdf;
    lightDir = L;
    return true;
}

bool neeSpot(in SpotLight sl,
             in vec3 posW, in vec3 ffnormal,
             in vec3 albedo, in vec3 throughput,
             in float lightPdf,
             in vec3 V, in vec2 specular,
             out vec3 contribution,
             out vec3 lightPos,
             out vec3 lightDir)
{
    contribution = vec3(0.0);
    lightPos     = sl.position;
    lightDir     = vec3(0.0);

    vec3 toL = sl.position - posW;
    float d  = length(toL);
    if (d <= 0.0 || d > sl.radius) return false;

    vec3 L = toL / d;
    // Cone cull early — outside the cone we contribute nothing regardless
    // of ndl or shadowing.
    float cosTheta = dot(-L, sl.direction);
    if (cosTheta < sl.cosOuterAngle) return false;

    float ndl = max(dot(ffnormal, L), 0.0);
    if (ndl <= 0.0) return false;

    float r2 = (d * d) / (sl.radius * sl.radius);
    float attenuation = sampleAttenuation(sl.attenuationTextureIndex, r2);

    // Gobo replaces the cone-falloff LUT when present (matches legacy
    // deferred_light_spotlight.frag.fsl's `if (HasGoboMap) ... else cone`).
    vec3  gobo = vec3(1.0);
    float cone = 1.0;
    if (sl.goboTextureIndex != INVALID_TEXTURE_INDEX)
    {
        vec4 lc = sl.viewProjection * vec4(posW, 1.0);
        if (lc.w > 0.0)
        {
            // g_mtxTextureUnitFix already baked into sl.viewProjection —
            // xy is [0,1] after the perspective divide.
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
        cone = sampleSpotCone(sl.coneFalloffTextureIndex,
                              cosTheta, sl.cosOuterAngle);
    }

    // Lambert BRDF: albedo / π.
    vec3 diffuseTerm = albedo * ndl;

    // Blinn-Phong specular term with energy normalization.
    vec3 specularTerm = vec3(0.0);
    if (sl.intensity > 0.0 && specular.x > 0.0)
    {
        vec3 H = normalize(L + V);
        float specPower = exp2(specular.y * 10.0) + 1.0;
        float normFactor = (specPower + 2.0) * 0.5;
        float specVal = sl.intensity * specular.x * normFactor
                      * pow(clamp(dot(H, ffnormal), 0.0, 1.0), specPower);
        specularTerm = vec3(specVal);
    }

    contribution = throughput * (diffuseTerm + specularTerm)
                 * sl.color * attenuation * cone * gobo / lightPdf;
    lightDir = L;
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
        // Shadow-ray direction follows visibility_shade.frag's convention:
        // light → surface, with a self-hit guard against the surfel hit's
        // (instanceCustomIndex, primitiveID). HPL2 lights are often embedded
        // inside their prop mesh; tracing surface→light with AnyHit reports
        // the prop interior as an occluder and zeros the contribution.
        // Contribution is staged here and the shadow ray fires after the
        // next-bounce ray is set up (deferred shadow).
        vec3 pendingDirect   = vec3(0.0);
        vec3 pendingLightPos = vec3(0.0);
        vec3 pendingL        = vec3(0.0);
        int  pendingHitInst  = prd.instanceCustomIndex;
        int  pendingHitPrim  = prd.primitiveID;
        bool pendingShadow   = false;

        // Stochastic single-sample NEE over the combined point+spot pool.
        // Box lights stay out of the pool — they're add-blend ambient with
        // no shadow ray, accumulated deterministically after the bounce.
        uint directionalLightCount = pointLightCount + spotLightCount;
        if (directionalLightCount > 0u)
        {
            uint lightIdx = min(uint(rand(prd.seed) * float(directionalLightCount)),
                                directionalLightCount - 1u);
            float lightPdf = 1.0 / float(directionalLightCount);

            vec3 V = -r.direction; // View direction pointing back to ray origin / previous hit

            if (lightIdx < pointLightCount)
            {
                pendingShadow = neePoint(pointLights[lightIdx],
                                         hit.posW, ffnormal,
                                         hit.albedo, throughput, lightPdf,
                                         V, hit.specular,
                                         pendingDirect, pendingLightPos, pendingL);
            }
            else
            {
                pendingShadow = neeSpot(spotLights[lightIdx - pointLightCount],
                                        hit.posW, ffnormal,
                                        hit.albedo, throughput, lightPdf,
                                        V, hit.specular,
                                        pendingDirect, pendingLightPos, pendingL);
            }
        }

        vec3 boxAdd = vec3(0);
        for (uint bi = 0u; bi < boxLightCount; ++bi)
        {
            BoxLight bl = boxLights[bi];
            mat3 wToL = mat3(bl.worldToLightX, bl.worldToLightY, bl.worldToLightZ);
            vec3 localPos = wToL * (hit.posW - bl.center);
            if (any(greaterThan(abs(localPos), bl.halfSize))) continue;
            boxAdd += bl.color;
        }
        
        radiance += throughput * hit.albedo * boxAdd;
  

        // Cosine-weighted Lambertian bounce, sampled around ffnormal so
        // back-face hits don't produce samples pointing into the surface.
        vec3 T, B;
        CreateCoordinateSystem(ffnormal, T, B);
        vec2 u = rand2(prd.seed);
        vec3 localDir = CosineSampleHemisphere(u.x, u.y);
        vec3 newDir = normalize(localDir.x * T + localDir.y * B + localDir.z * ffnormal);

        // Specular-aware throughput update using the normalized Blinn-Phong BRDF:
        vec3 V = -r.direction;
        vec3 H = normalize(newDir + V);
        float specPower = exp2(hit.specular.y * 10.0) + 1.0;
        float specVal = hit.specular.x * ((specPower + 2.0) * 0.5)
                      * pow(clamp(dot(H, ffnormal), 0.0, 1.0), specPower);
        throughput *= clamp(hit.albedo + vec3(specVal), 0.0, 0.95);

        r.origin    = OffsetRay(hit.posW, ffnormal);
        r.direction = newDir;

        // Deferred shadow ray for the NEE sample chosen above. Mirrors
        // visibility_shade.frag's convention: shoot light→surface with a
        // ClosestHit and accept-as-lit when either nothing was hit or the
        // closest hit is the surfel's own triangle (self-hit guard, matched
        // by instanceCustomIndex + primitiveID). This handles HPL2's
        // common case of lights placed inside / behind their fixture mesh.
        if (pendingShadow)
        {
            PtPayload occ;
            occ.seed = prd.seed;
            ClosestHit(Ray(pendingLightPos, -pendingL), occ);
            bool selfHit = occ.instanceCustomIndex == pendingHitInst &&
                           occ.primitiveID         == pendingHitPrim;
            if (occ.hitT >= INFINITY || selfHit)
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
        vec4 irradiance = vec4(0.0);
        uint randSeed   = tea(surfelIndex, totalFrames);
        if (finalizePathWithSurfel(lastHit.posW, lastHit.normalW, randSeed, irradiance))
        {
            radiance += irradiance.xyz * throughput;
        }
    }

    return radiance;
}


#endif // LAYOUTS_GLSL
