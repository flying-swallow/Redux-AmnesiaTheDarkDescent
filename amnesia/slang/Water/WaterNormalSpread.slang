// Water-guide normal spread. This module is intentionally self-contained:
// the reducer has no gPerFrame binding, so importing the ray-cone module would
// also pull in PerFrame.resource for no useful reason.

// The normalized candidate normals form a directional sample set. Their mean
// resultant length R = |sum(n)| / count is one when every raster sample agrees
// and falls toward zero as the samples spread over the sphere. For a symmetric
// small-angle lobe, theta = sqrt(2 * (1 - R)) is the RMS angular deviation to
// second order: with half-angle phi around the mean, it is 2*sin(phi/2).
// Mapping that angular estimate through tan gives an effective GGX alpha, with
// a 45-degree cap keeping the result in [0, 1]. This is a lower bound: it can
// only resolve variation present across full-resolution raster samples.
public float waterGuideNormalSpreadAlpha(float3 normalSum, uint count)
{
    if (count < 2u
        || !isfinite(normalSum.x) || !isfinite(normalSum.y)
        || !isfinite(normalSum.z))
        return 0.0f;

    float R = clamp(length(normalSum) / float(count), 0.0f, 1.0f);
    float theta = sqrt(2.0f * max(0.0f, 1.0f - R));
    return tan(min(theta, 0.7853981633974483f));
}

// The stored guide value is a GGX alpha, while NRD's front end takes linear
// roughness. This is the same linearRoughness = sqrt(alpha) convention used
// by PathTracer/NrdPack.cs.slang for the opaque path.
public float waterNrdLinearRoughness(float spreadAlpha)
{
    float alpha = isfinite(spreadAlpha)
                      ? clamp(spreadAlpha, 0.0f, 1.0f)
                      : 0.0f;
    return sqrt(alpha);
}
