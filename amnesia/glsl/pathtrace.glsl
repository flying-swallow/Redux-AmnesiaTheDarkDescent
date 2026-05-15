//-------------------------------------------------------------------------------------------------
// Minimal path-trace helpers for surfel_raytrace.comp.
//
// The original NVIDIA RTX-sample pathtracer that lived here (Disney/GLTF
// BSDFs, MIS, sun-sky, env IBL, debug modes, camera samplePixel) was retired
// when the renderer migrated to the bindless scene model. The actual
// surfel-ray loop now lives in shaderUtils_surfel_cell.glsl's
// surfelPathTrace(), guarded by `#ifdef LAYOUTS_GLSL`.
//
// What stays here: only the tiny helpers used by both the path loop and the
// surfel shading paths. Bigger building blocks (CreateCoordinateSystem,
// CosineSampleHemisphere, OctUVToDir, ...) live in common.glsl.

#ifndef PATHTRACE_GLSL
#define PATHTRACE_GLSL 1

// Offset a ray origin off a surface along the normal to avoid self-intersection.
// Conservative scalar offset — fine for the bindless renderer's geometry scale.
vec3 OffsetRayBindless(in vec3 posW, in vec3 normalW)
{
    const float kSelfIntersectEps = 0.001;
    return posW + normalW * kSelfIntersectEps;
}

// Cosine-weighted hemisphere sample around +Z. Caller is responsible for
// rotating into the world-space frame (CreateCoordinateSystem in common.glsl).
// Lifted from the old pbr_disney.glsl since the BSDF file is no longer
// included by the bindless surfel-raytrace chain.
vec3 CosineSampleHemisphere(float r1, float r2)
{
    vec3 dir;
    float r   = sqrt(r1);
    float phi = TWO_PI * r2;
    dir.x = r * cos(phi);
    dir.y = r * sin(phi);
    dir.z = sqrt(max(0.0, 1.0 - dir.x * dir.x - dir.y * dir.y));
    return dir;
}

#endif  // PATHTRACE_GLSL
