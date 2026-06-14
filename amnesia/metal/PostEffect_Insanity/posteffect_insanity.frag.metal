// Hand-written Metal port of amnesia/slang/PostEffect_Insanity/posteffect_insanity.frag.slang
//
// Insanity screen distortion: animated sine waves (amplitude from two blended
// amp maps) plus a radial zoom warp (from a zoom map). UV-distortion only; at
// waveAlpha == zoomAlpha == 0 it is an exact passthrough.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1   (prior pogo half = tonemapped scene)
//     ampMap0       2
//     ampMap1       3
//     zoomMap       4
//   push constant (InsanityPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
    texture2d<float, access::sample> ampMap0      [[id(2)]];
    texture2d<float, access::sample> ampMap1      [[id(3)]];
    texture2d<float, access::sample> zoomMap      [[id(4)]];
};

struct InsanityPC {
    float time;        // wave animation phase (mfT)
    float amplitude;   // blend between ampMap0 and ampMap1
    float waveAlpha;   // sine-wave distortion strength
    float zoomAlpha;   // radial zoom-warp strength
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant InsanityPC& pc [[buffer(1)]]) {
    float2 uv01 = pin.v_uv;
    uint w = set0.sourceInput.get_width();
    uint h = set0.sourceInput.get_height();
    float2 size = float2(float(w), float(h));

    // Amplitude map: blend the two animated maps, scale to a pixel-space sine
    // amplitude (the 0.04 * screenHeight factor matches the legacy .fsl).
    float3 a0 = set0.ampMap0.sample(set0.inputSampler, uv01).rgb;
    float3 a1 = set0.ampMap1.sample(set0.inputSampler, uv01).rgb;
    float3 amp = a0 * (1.0f - pc.amplitude) + a1 * pc.amplitude;
    float2 ampDir = (pc.waveAlpha * 0.04f * size.y) * amp.xy;

    // Radial zoom warp: zoom.xy is a direction from center (0.5), zoom.z magnitude.
    float3 zoom = set0.zoomMap.sample(set0.inputSampler, uv01).rgb;
    float2 pix = uv01 * size;
    pix += (zoom.xy - 0.5f) * 2.0f * 0.6f * zoom.z * size.y * pc.zoomAlpha;

    // Animated sine distortion, phase offset per axis (1.83 matches the legacy).
    float2 s = (pix / size.y) * 0.6f;
    pix.x += sin(pc.time + s.y) * ampDir.x;
    pix.y += sin(pc.time + s.x * 1.83f) * ampDir.y;

    return float4(set0.sourceInput.sample(set0.inputSampler, pix / size).rgb, 1.0f);
}
