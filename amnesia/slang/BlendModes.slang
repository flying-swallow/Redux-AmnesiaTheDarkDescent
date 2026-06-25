#pragma once
// Shared legacy blend-mode math. Previously copy-pasted (constants + the
// 5-way switches) into Decal.frag, Particle.frag and Translucent.frag, plus
// the composite's applyDecalBlend.
//
// The legacy renderer blended raw gamma texels in a display-space UNORM
// target; this renderer blends linear HDR. The scheme (see
// kPerceptualBlendExp in Constants.h for the full derivation):
//   * sample → displaySpaceSample re-encodes the hardware-sRGB-decoded texel
//     back to the authored display space,
//   * applyBlendModulation folds alpha (sceneAlpha × fog visibility) and the
//     light level into the source per blend mode, still in display space,
//   * encodeBlendOutputLinear decodes to linear ONCE at output — exact for
//     the dst products (MUL/MULX2, power law); the blend-with-dst sums
//     (ALPHA/PREMUL) distribute the decode over powered premultiplied
//     weights src·pow(a,k) / dst·pow(1−a,k) instead (pipeline ALPHA factors
//     are ONE / 1−srcA).
// Translucent keeps the two stages separate because its display-space
// reflection/rim add sits between them; Particle/Decal call them
// back-to-back (Decal with the identity modulation alpha=1, lightLevel=1).
//
// blendDisplaySpaceExact is the OTHER, intentionally different scheme: the
// composite projects clustered decals with dst in hand, so it software-blends
// exactly in display space — no powered-weight approximation — and uses the
// RAW eMaterialBlendMode numbering (kMaterialBlendMode*, None = 0), not the
// push-constant kBlendMode* (0-4) the raster passes use.

import SceneTypes;                 // kBlendMode* / kMaterialBlendMode* / kPerceptualBlendExp
import Utils.Color.ColorHelpers;   // linearToSRGB / sRGBToLinear

// Re-encode a hardware-sRGB-decoded texel into the authored display space
// (the space the legacy renderer blended in).
public float4 displaySpaceSample(float4 s)
{
    return float4(linearToSRGB(s.rgb), s.a);
}

// Stage 1 — display-space source modulation (the legacy per-mode "pre-bias").
// alpha is sceneAlpha × fog visibility; lightLevel the AffectedByLightLevel
// dim factor. Identity at alpha == 1, lightLevel == 1. Never touches .a for
// ADD/MUL/MULX2, so the result's .a IS the legacy display-alpha discard
// operand the passes test.
public float4 applyBlendModulation(uint blendMode, float4 displayColor,
                                   float alpha, float lightLevel)
{
    switch (blendMode)
    {
        case kBlendModeAdd:
        {
            displayColor.xyz *= alpha * lightLevel;
            break;
        }
        case kBlendModeMul:
        {
            // Fade toward the multiplicative identity (1) as alpha drops.
            displayColor.xyz += (float3(1.0f) - displayColor.xyz) * (1.0f - alpha);
            break;
        }
        case kBlendModeMulX2:
        {
            // Fade toward the 2·src·dst neutral (0.5) as alpha·L drops.
            float blendMulAlpha = lightLevel * alpha;
            displayColor.xyz = displayColor.xyz * blendMulAlpha
                             + float3(0.5f) * (1.0f - blendMulAlpha);
            break;
        }
        case kBlendModeAlpha:
        {
            displayColor.xyz *= lightLevel;
            displayColor.a   *= alpha;
            break;
        }
        case kBlendModePremulAlpha:
        {
            displayColor     *= alpha;
            displayColor.xyz *= lightLevel;
            break;
        }
    }
    return displayColor;
}

// Stage 2 — decode the display-space source to linear for the hardware
// blender. Products with dst (ADD's dst-free term, MUL, MULX2) decode
// exactly; ALPHA/PREMUL reshape into powered premultiplied weights.
// MULX2: legacy is 2·dst·m and the HW factors give 2·src·dst →
// src = toLin(2·m)/2, so the authored 0.5-gray neutral stays neutral.
public float4 encodeBlendOutputLinear(uint blendMode, float4 displayColor)
{
    const float k = kPerceptualBlendExp;
    switch (blendMode)
    {
        case kBlendModeAdd:
        case kBlendModeMul:
        {
            displayColor.xyz = sRGBToLinear(displayColor.xyz);
            break;
        }
        case kBlendModeMulX2:
        {
            displayColor.xyz = sRGBToLinear(2.0f * displayColor.xyz) * 0.5f;
            break;
        }
        case kBlendModeAlpha:
        {
            // Energy-conserving variant: the powered premultiplied weights
            // src·pow(a,k) / dst·pow(1−a,k) sum to LESS than 1 at partial
            // alpha (worst near a=0.5), so the legacy form darkened/lost
            // energy and made low-alpha surfaces (dust, haze) nearly vanish.
            // Normalize the two weights to sum to 1 — keeps the perceptual
            // bias SHAPE but removes the darkening dip.
            float a  = saturate(displayColor.a);
            float ws = pow(a, k);
            float wd = pow(1.0f - a, k);
            float n  = max(ws + wd, 1e-4f);
            displayColor.xyz = sRGBToLinear(displayColor.xyz) * (ws / n);
            displayColor.a   = ws / n;          // HW dst factor (1 − src.a) = wd/n
            break;
        }
        case kBlendModePremulAlpha:
        {
            float a = saturate(displayColor.a);
            displayColor.xyz = sRGBToLinear(displayColor.xyz);
            displayColor.a = 1.0f - pow(1.0f - a, k);
            break;
        }
    }
    return displayColor;
}

// EXACT display-space software blend — composite-time clustered decals, where
// dst is in hand so the legacy hardware blend is replicated bit-for-bit (no
// powered-weight approximation). NOTE: takes the RAW eMaterialBlendMode
// numbering (kMaterialBlendModeNone = 0 … PremulAlpha = 5) as stored in
// GpuDecal.blendMode — NOT the push-constant kBlendMode* scheme above.
public float3 blendDisplaySpaceExact(uint materialBlendMode,
                                     float3 dstDisplay, float4 srcDisplay)
{
    switch (materialBlendMode)
    {
        case kMaterialBlendModeAdd:         return dstDisplay + srcDisplay.rgb;
        case kMaterialBlendModeMul:         return dstDisplay * srcDisplay.rgb;
        case kMaterialBlendModeMulX2:       return 2.0f * dstDisplay * srcDisplay.rgb;
        case kMaterialBlendModeAlpha:       return lerp(dstDisplay, srcDisplay.rgb, srcDisplay.a);
        case kMaterialBlendModePremulAlpha: return srcDisplay.rgb + dstDisplay * (1.0f - srcDisplay.a);
        default:                            return dstDisplay;   // None
    }
}
