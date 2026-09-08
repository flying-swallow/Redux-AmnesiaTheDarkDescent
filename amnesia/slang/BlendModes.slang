#pragma once
// Forward materials blend linear RGB into RGBA16F. Opacity is linear data;
// exposure, tone mapping and user gamma are applied after compositing.
import SceneTypes;
import Utils.Color.ColorHelpers;

// Continuous replacement for the BC1 white halo: fit in authored color space
// (peak, radius, exponent), then decode before HDR blending. Smoothly reach
// zero at the billboard boundary; neither texture quantization nor a hard
// alpha cutoff should introduce contours as the halo is enlarged near camera.
public float4 smoothHaloLinearSample(float2 uv, float4 tint)
{
    float radius = length(uv * 2.0f - 1.0f);
    float falloff = 0.46002168f * exp(-pow(radius / 0.58290833f, 2.67673404f));
    falloff *= 1.0f - smoothstep(0.8f, 1.0f, radius);
    return float4(sRGBToLinear(float3(falloff)) * sRGBToLinear(tint.rgb), tint.a);
}

// Diffuse color samples are already hardware sRGB-decoded. The explicit
// mask option interprets the authored red channel as scalar emission weight.
// Recover that weight from the existing sRGB view, keeping shared texture
// cache entries and ordinary color materials unchanged.
public float4 translucentLinearSample(float4 sample, float4 tint,
                                     uint blendMode, bool diffuseIsMask)
{
    float3 rgb = sample.rgb;
    if (diffuseIsMask)
        rgb = float3(linearToSRGB(sample.r));
    else if (blendMode == kBlendModeMul || blendMode == kBlendModeMulX2)
        // Multipliers are data too; preserve authored 1 / 0.5 neutrals.
        rgb = linearToSRGB(sample.rgb);
    else if (blendMode == kBlendModePremulAlpha)
    {
        // Legacy premultiplied textures multiplied in encoded space.
        // Unassociate there, decode the color, then associate in linear.
        rgb = sample.a > 0.0f
            ? sRGBToLinear(linearToSRGB(sample.rgb) / sample.a) * sample.a
            : float3(0.0f);
    }
    rgb *= sRGBToLinear(tint.rgb);
    // Straight alpha is premultiplied at output; premul texture RGB already
    // contains texture alpha and only needs the additional vertex opacity.
    if (blendMode == kBlendModePremulAlpha)
        rgb *= tint.a * (diffuseIsMask ? sample.a : 1.0f);
    return float4(rgb, sample.a * tint.a);
}

public float4 applyBlendModulation(uint blendMode, float4 color,
                                   float opacity, float lightLevel)
{
    opacity = saturate(opacity);
    color.a = saturate(color.a);
    switch (blendMode)
    {
        case kBlendModeAdd:
            color.rgb *= color.a * opacity * lightLevel;
            break;
        case kBlendModeMul:
            color.rgb = lerp(float3(1.0f), color.rgb, opacity * color.a);
            break;
        case kBlendModeMulX2:
            color.rgb = lerp(float3(0.5f), color.rgb,
                             saturate(opacity * color.a * lightLevel));
            break;
        case kBlendModeAlpha:
            color.rgb *= lightLevel;
            color.a *= opacity;
            break;
        case kBlendModePremulAlpha:
            color.rgb *= opacity * lightLevel;
            color.a *= opacity;
            break;
    }
    return color;
}

// ONE / ONE_MINUS_SRC_ALPHA: premultiply straight-alpha sources exactly once.
// Additive sources retain ONE / ONE and never attenuate the destination.
public float4 encodeBlendOutputLinear(uint blendMode, float4 color)
{
    if (blendMode == kBlendModeAlpha)
        color.rgb *= color.a;
    return color;
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
