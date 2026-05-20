#ifndef PARALLAX_GLSL
#define PARALLAX_GLSL

// Parallax-occlusion mapping — port of the legacy
// AmnesiaTheDarkDescent/HPL2/resource/deferred_common.h.fsl
// implementation (WorldSpaceToTangent + GetNormalizedDepth + ParallaxAdvance,
// plus the contact-refinement loop from Andrea Riccardi). Visibility-buffer
// composite calls ParallaxAdvance after the diffuse/specular UV is known and
// before any texture samples that should respect the height-shifted surface.
//
// Heightmap is sampled via the bindless textures_2d[] table — the slot index
// is threaded through as a parameter because we have no per-shader `heightMap`
// global the way the legacy FSL pipeline did.
//
// Including TUs must already have bindless.resource.glsl + per_frame.resource.glsl
// in scope (for textures_2d, materialSampler).

#extension GL_EXT_nonuniform_qualifier : require

// Same constant as legacy deferred_resources.h.fsl:5. Applied as a global
// scale on the per-material heightMapScale so artists can tune one knob to
// dial down all parallax depth at once.
#ifndef PARALLAX_MULTIPLIER
#define PARALLAX_MULTIPLIER 0.7
#endif

vec3 WorldSpaceToTangent(vec3 dir, vec3 normal, vec3 tangent, vec3 bitangent)
{
    return vec3(dot(tangent, dir),
                dot(bitangent, dir),
                dot(normal, dir));
}

// Sample the heightmap at `uv` and clamp to a minimum normalized depth.
// `isSingleChannel`: legacy materialConfig bit 9 — when set, sample .r;
// otherwise sample .a (heightmap packed in alpha alongside e.g. a normal map).
float GetNormalizedDepth(float startDepth, float stopDepth,
                         float inverseDepthRange, vec2 uv,
                         bool isSingleChannel, uint heightTextureSlot)
{
    vec4 sampled = texture(
        sampler2D(textures_2d[nonuniformEXT(heightTextureSlot)], materialSampler),
        uv);
    float currentSample = isSingleChannel ? sampled.r : sampled.a;
    float normalizedDepth = 0.0;
    if (stopDepth - startDepth > 0.0001)
    {
        float minNormalizedDepth = -startDepth * inverseDepthRange;
        normalizedDepth = max(currentSample, minNormalizedDepth);
    }
    return normalizedDepth;
}

// Returns the UV offset (delta) to apply to `uv` so the textured surface
// reads as if it had been displaced by the heightmap.
//
// `dir` is world-space surface→camera (NOT camera→surface). Legacy passed
// `In.pos` which in view space points from eye to surface; the WorldSpaceToTangent
// projection then flips into the tangent-space view dir we march along.
// In the visibility composite, hand in normalize(worldPos - cameraPos).
vec2 ParallaxAdvance(vec2 uv, float depthOffset, float numberOfSteps,
                     float heightMapScale, vec3 dir,
                     vec3 normal, vec3 tangent, vec3 bitangent,
                     bool isSingleChannel, uint heightTextureSlot)
{
    vec3 dirToCameraTS = normalize(WorldSpaceToTangent(dir, normal, tangent, bitangent));

    float dirToCameraZInverse = 1.0 / dirToCameraTS.z;
    float step        = 1.0 / numberOfSteps;
    float currentStep = 0.0;

    // shift in the inverse direction of dirToCameraTS
    vec3 delta = -dirToCameraTS.xyz * heightMapScale * dirToCameraZInverse * step;

    float depthSearchStart   = depthOffset;
    float depthSearchEnd     = depthSearchStart + heightMapScale;
    float inverseDepthFactor = 1.0 / heightMapScale;

    vec3 parallaxOffset = -dirToCameraTS.xyz * dirToCameraZInverse * depthOffset;

    vec4 sampled0 = texture(
        sampler2D(textures_2d[nonuniformEXT(heightTextureSlot)], materialSampler),
        uv + parallaxOffset.xy);
    float currentSample = isSingleChannel ? sampled0.r : sampled0.a;
    float prevSample = 0.0;

    // Coarse linear search for an intersection.
    while (currentSample > currentStep)
    {
        currentStep    += step;
        parallaxOffset += delta;

        prevSample    = currentSample;
        currentSample = GetNormalizedDepth(depthSearchStart, depthSearchEnd,
                                           inverseDepthFactor,
                                           uv + parallaxOffset.xy,
                                           isSingleChannel,
                                           heightTextureSlot);
    }

    if (currentStep > 0.0)
    {
        // Contact-refinement (Andrea Riccardi). Roll back one step and re-walk
        // with the much finer (step * step) increment to pin the intersection.
        parallaxOffset -= delta;
        currentStep    -= step;
        currentSample   = prevSample;

        vec3  adjustedDelta = delta * step;
        float adjustedStep  = step * step;

        while (currentSample > currentStep)
        {
            currentStep    += adjustedStep;
            parallaxOffset += adjustedDelta;
            prevSample      = currentSample;

            currentSample = GetNormalizedDepth(depthSearchStart, depthSearchEnd,
                                               inverseDepthFactor,
                                               uv + parallaxOffset.xy,
                                               isSingleChannel,
                                               heightTextureSlot);
        }
    }

    // Surfaces that resolved to a depth in front of the geometry would invert
    // the displacement — clamp to zero offset (matches legacy guard).
    if (parallaxOffset.z > 0.0)
    {
        parallaxOffset = vec3(0.0);
    }
    return parallaxOffset.xy;
}

#endif // PARALLAX_GLSL
