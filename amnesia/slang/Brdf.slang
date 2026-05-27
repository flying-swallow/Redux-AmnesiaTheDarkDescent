#pragma once
//
// Physically based (Cook-Torrance GGX) BRDF for the surfel-GI shading paths.
//
// HPL2 materials are not authored as PBR, so the surface parameters are
// reconstructed from the legacy DiffuseMaterial fields (see SceneTypes.slang):
//   * albedo      = diffuse texture (tex[0])
//   * F0          = frenselBias (normal-incidence specular reflectance),
//                   scaled by the specular-map intensity (tex[3].x) when present
//   * roughness   = specular-map gloss (tex[3].y) decoded through the legacy
//                   Blinn-Phong exponent `n = exp2(gloss*10)+1` and remapped to
//                   a GGX alpha (deferred_light_frag.glsl:234 is the source)
//   * fresnelPow  = frenselPow, used as a generalized Schlick exponent
// heightMapScale / heightMapBias are parallax (UV-warp) parameters with no
// reflectance meaning and are intentionally unused here.
//
// The specular lobe is gated on the material actually carrying a specular map
// or a reflection cubemap (kMaterialFlagEnableSpecular / *EnableCubeMap); plain
// matte materials fall back to pure Lambert so their look is unchanged.
//
// evalBrdf() returns the physically based BRDF value f (diffuse term carries
// the 1/pi normalization); callers multiply by Li * N.L. sampleBrdf() picks a
// lobe stochastically and returns L plus the throughput weight f*N.L/pdf.

import SceneTypes;          // DiffuseMaterial + kMaterialFlag* / kInvalidTextureIndex
import Bindless.resource;   // gTextures2D, gMaterialSampler
import Random;              // RNG

static const float kBrdfPi   = 3.14159265358979323846f;
// Roughness floor — keeps the GGX lobe from collapsing to a delta (infinite
// NDF spike / NaN throughput) for near-mirror surfaces.
static const float kMinRough = 0.04f;
// Normal-incidence specular reflectance for dielectrics (~0.04). Every HPL2
// material is a non-metal (no metallic flag), so the spec-map lobe is anchored
// here and only scaled by the artist's specular-map intensity.
static const float kDielectricF0 = 0.04f;

public struct SurfaceBRDF
{
    public float3 N;          // shading normal (world space, unit)
    public float3 albedo;     // diffuse reflectance
    public float3 F0;         // specular reflectance at normal incidence
    public float  rough;      // GGX alpha (already remapped from gloss)
    public float  fresnelPow; // generalized Schlick exponent (artist frenselPow)
    public bool   hasSpecular;
};

float brdfLuminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// Rows (T, B, N): local->world via mul(localDir, basis), matching the
// get_tangentspace() convention used elsewhere in the surfel passes.
float3x3 brdfTangentToWorld(float3 N)
{
    float3 helper = abs(N.x) > 0.99f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(N, helper));
    float3 B = normalize(cross(N, T));
    return float3x3(T, B, N);
}

// Generalized Schlick Fresnel. `p` is frenselPow (physical Schlick uses 5;
// HPL2 authors default to 8 for a tighter rim — honored here).
public float3 fresnelSchlick(float cosTheta, float3 F0, float p)
{
    float m = clamp(1.0f - cosTheta, 0.0f, 1.0f);
    return F0 + (float3(1.0f) - F0) * pow(m, p);
}

// GGX / Trowbridge-Reitz normal distribution.
float distGGX(float NoH, float a)
{
    float a2 = a * a;
    float d  = (NoH * NoH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(kBrdfPi * d * d, 1e-7f);
}

// Smith height-correlated visibility (already folds in the 1/(4 NoL NoV)).
float visSmithGGX(float NoV, float NoL, float a)
{
    float a2   = a * a;
    float ggxV = NoL * sqrt(NoV * NoV * (1.0f - a2) + a2);
    float ggxL = NoV * sqrt(NoL * NoL * (1.0f - a2) + a2);
    return 0.5f / max(ggxV + ggxL, 1e-7f);
}

// Physically based BRDF value (no N.L factor). Diffuse carries 1/pi.
public float3 evalBrdf(SurfaceBRDF s, float3 V, float3 L)
{
    float NoL = dot(s.N, L);
    float NoV = dot(s.N, V);
    if (NoL <= 0.0f || NoV <= 0.0f) return float3(0.0f);

    float3 H   = normalize(V + L);
    float  NoH = saturate(dot(s.N, H));
    float  VoH = saturate(dot(V, H));

    float3 F       = s.hasSpecular ? fresnelSchlick(VoH, s.F0, s.fresnelPow) : float3(0.0f);
    float3 diffuse = s.albedo * (float3(1.0f) - F) * (1.0f / kBrdfPi);

    float3 spec = float3(0.0f);
    if (s.hasSpecular)
    {
        float D   = distGGX(NoH, s.rough);
        float Vis = visSmithGGX(NoV, NoL, s.rough);
        spec = D * Vis * F;
    }
    return diffuse + spec;
}

// Sample an outgoing direction. Returns false when the sample is below the
// horizon / degenerate. `weight` is f * N.L / pdf (lobe-selection probability
// folded in), i.e. the factor a path throughput should be multiplied by.
public bool sampleBrdf(SurfaceBRDF s, float3 V, inout RNG rng, out float3 L, out float3 weight)
{
    L = s.N;
    weight = float3(0.0f);

    float NoV = dot(s.N, V);
    if (NoV <= 0.0f) return false;

    // Energy split: diffuse keeps the (1 - F) the specular lobe reflects away.
    float3 Fv = s.hasSpecular ? fresnelSchlick(NoV, s.F0, s.fresnelPow) : float3(0.0f);
    float3 kd = s.albedo * (float3(1.0f) - Fv);

    float wDiff = brdfLuminance(kd);
    float wSpec = s.hasSpecular ? brdfLuminance(Fv) : 0.0f;
    float sum   = wDiff + wSpec;
    float pSpec = (s.hasSpecular && sum > 1e-6f) ? clamp(wSpec / sum, 0.05f, 0.95f) : 0.0f;

    float3x3 basis = brdfTangentToWorld(s.N);

    if (rng.next_float() < pSpec)
    {
        // Specular: importance-sample the GGX half-vector.
        float a    = s.rough;
        float u1   = rng.next_float();
        float u2   = rng.next_float();
        float phi  = 2.0f * kBrdfPi * u1;
        float cosT = sqrt((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2));
        float sinT = sqrt(max(0.0f, 1.0f - cosT * cosT));
        float3 Hl  = float3(sinT * cos(phi), sinT * sin(phi), cosT);
        float3 H   = normalize(mul(Hl, basis));

        L = reflect(-V, H);
        float NoL = dot(s.N, L);
        if (NoL <= 0.0f) return false;

        float NoH = saturate(dot(s.N, H));
        float VoH = saturate(dot(V, H));
        float Vis = visSmithGGX(max(NoV, 1e-4f), NoL, a);
        float3 F  = fresnelSchlick(VoH, s.F0, s.fresnelPow);

        // pdf(L) = D*NoH/(4 VoH); with height-correlated Vis the f*N.L/pdf
        // estimator collapses to 4 * Vis * F * N.L * VoH / NoH.
        weight = (4.0f * Vis * NoL * VoH / max(NoH, 1e-4f)) * F / pSpec;
        return true;
    }

    // Diffuse: cosine-weighted hemisphere. f*N.L/pdf = kd.
    float u1   = rng.next_float();
    float u2   = rng.next_float();
    float r    = sqrt(u1);
    float phi  = 2.0f * kBrdfPi * u2;
    float3 Ll  = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0f, 1.0f - u1)));
    L = normalize(mul(Ll, basis));
    if (dot(s.N, L) <= 0.0f) return false;

    weight = kd / max(1.0f - pSpec, 1e-4f);
    return true;
}

// Reconstruct PBR surface parameters from a DiffuseMaterial + interpolated
// UV/normal. `N` is the world-space shading normal.
public SurfaceBRDF prepareSurface(DiffuseMaterial m, float3 N, float2 uv)
{
    SurfaceBRDF s;
    s.N = N;

    uint diffuseTex = m.tex[0];
    if (diffuseTex == kInvalidTextureIndex)
        s.albedo = float3(0.5f);
    else
        s.albedo = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                       .SampleLevel(gMaterialSampler, uv, 0.0f).rgb;

    bool hasSpec = ((m.materialConfig & kMaterialFlagEnableSpecular) != 0u)
                 && (m.tex[3] != kInvalidTextureIndex);
    bool hasCube = (m.materialConfig & kMaterialFlagEnableCubeMap) != 0u;

    s.fresnelPow  = max(m.frenselPow, 1.0f);
    s.hasSpecular = hasSpec;

    if (hasSpec)
    {
        float2 spec = gTextures2D[NonUniformResourceIndex(m.tex[3])]
                          .SampleLevel(gMaterialSampler, uv, 0.0f).xy;
        float specIntensity = spec.x;
        float gloss         = spec.y;
        // Legacy gloss encoding: Blinn-Phong exponent n = exp2(gloss*10)+1
        // (~2..1025); remap Phong exponent -> GGX alpha = sqrt(2/(n+2)).
        float n     = exp2(gloss * 10.0f) + 1.0f;
        float alpha = sqrt(2.0f / (n + 2.0f));
        s.rough = clamp(alpha, kMinRough, 1.0f);
        // Dielectric F0 (~0.04) scaled by the legacy specular-map intensity.
        // NOT frenselBias: in the base game frenselBias only drove the *cubemap*
        // reflection (helper_reflection.glsl), so feeding its 0.2 default into
        // the microfacet F0 made every spec-mapped stone a 20%-reflective
        // surface. Intensity 0 => no specular highlight.
        s.F0 = float3(kDielectricF0 * saturate(specIntensity));
    }
    else if (hasCube)
    {
        // Reflective (cubemap) material with no spec map: legacy reflection is
        // mirror-sharp, so use a near-zero roughness and frenselBias as F0.
        s.rough = kMinRough;
        s.F0    = float3(max(m.frenselBias, 0.0f));
    }
    else
    {
        s.rough = 1.0f;
        s.F0    = float3(0.0f);
    }
    return s;
}
