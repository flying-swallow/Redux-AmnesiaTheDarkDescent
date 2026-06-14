// Hand-written Metal port of amnesia/slang/PostEffect_ToneMap/posteffect_tonemap.frag.slang
//
// Output-encode pass: applies exposure, ACES filmic tone curve, sRGB OETF, and
// a final user-gamma adjust, clamping the HDR pogo into a display-encoded [0,1]
// the tail blit copies to the UNORM swapchain.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1
//   push constant (ToneMapPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

#include "Utils/Color/ColorHelpers.metalh"   // linearToSRGB()

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
};

struct ToneMapPC {
    float exposure;
    float shadowLift;
    float gamma;     // user display-gamma (1.0 = no-op)
    float _pad1;
};

// Stephen Hill's "ACES fitted" RRT+ODT(sRGB). Slang declares these as HLSL
// row-major and applies them as mul(matrix, vector) (= row . vector). Metal's
// float3x3(c0,c1,c2) takes COLUMNS and M*v is the column-major product, so the
// matrices below are the transpose of the Slang literals: each constructor
// argument is a *column* of the original (= original rows read down).
constant float3x3 ACESInputMat = float3x3(
    float3(0.59719f, 0.07600f, 0.02840f),
    float3(0.35458f, 0.90834f, 0.13383f),
    float3(0.04823f, 0.01566f, 0.83777f));
constant float3x3 ACESOutputMat = float3x3(
    float3( 1.60475f, -0.10208f, -0.00327f),
    float3(-0.53108f,  1.10813f, -0.07276f),
    float3(-0.07367f, -0.00605f,  1.07602f));

static float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

static float3 ACESFitted(float3 color) {
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return saturate(color);          // linear sRGB [0,1]
}

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant ToneMapPC& pc [[buffer(1)]]) {
    float3 hdr = set0.sourceInput.sample(set0.inputSampler, pin.v_uv).rgb * pc.exposure;
    float3 display = saturate(ACESFitted(linearToSRGB(hdr)));   // display-encoded sRGB [0,1]
    // User gamma adjust (final): 1.0 = identity, <1 darker / >1 brighter.
    display = pow(display, float3(1.0f / max(pc.gamma, 1e-4f)));
    return float4(display, 1.0f);
}
