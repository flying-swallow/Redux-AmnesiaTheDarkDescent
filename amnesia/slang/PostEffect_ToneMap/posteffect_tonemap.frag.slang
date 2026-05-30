/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// ACES filmic tonemap (Stephen Hill fit). Runs as a post-effect after bloom:
// maps the linear-HDR pogo into linear display-referred [0,1] so highlights roll
// off instead of clipping. Output stays LINEAR — the scRGB-linear swapchain +
// display apply the sRGB OETF, so no gamma encode here.
//
// The Hill fit runs the ACEScg input/output matrices around the RRT+ODT curve,
// which desaturate saturated colours (vs Narkowicz's per-channel fit that skews
// reds/blues into neon). `exposure` lifts the scene out of the toe before the
// curve; `shadowLift` raises the black floor afterward so pitch-blacks come back.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
// Push constant: exposure (pre-curve multiply), shadowLift (post black-point lift).

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct ToneMapPC
{
    float exposure;
    float shadowLift;
    float _pad0;
    float _pad1;
};

[vk::push_constant]
ToneMapPC pc;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
};

// Stephen Hill's ACES fit (sRGB/Rec.709 in & out). Matrices are row-major; with
// slang's mul(M, v) = M·v they apply the ACEScg input/output transforms.
static const float3x3 ACESInputMat = float3x3(
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f);

static const float3x3 ACESOutputMat = float3x3(
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f,  1.10813f, -0.00605f,
    -0.00327f, -0.07276f,  1.07602f);

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 hdr = sourceInput.Sample(inputSampler, pin.v_uv).rgb * pc.exposure;
    float3 mapped = ACESFitted(hdr);
    return float4(mapped, 1.0f);
}
