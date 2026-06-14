// Hand-written Metal port of amnesia/slang/PostEffect_RadialBlur/posteffect_radial_blur.frag.slang
//
// Five-tap radial blur from screen-center outward; intensity ramps with distance
// past blurStartDist (in half-screen-width units).
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1
//   push constant (RadialBlurPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
};

struct RadialBlurPC {
    float  size;
    float  blurStartDist;
    float2 screenDim;
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

static float3 SampleTextureBlur(texture2d<float, access::sample> tex, sampler s,
                                float2 viewTexel, float2 screenCoord,
                                float2 dir, float sizeMul, float colorMul) {
    float2 pos = (screenCoord + dir * sizeMul) * viewTexel;
    return tex.sample(s, pos).rgb * colorMul;
}

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant RadialBlurPC& pc [[buffer(1)]]) {
    float2 viewTexel      = 1.0f / pc.screenDim;
    float2 halfScreenSize = pc.screenDim * 0.5f;
    float2 screenCoord    = pin.v_uv * pc.screenDim;

    float2 vDir = halfScreenSize - screenCoord;
    float  fDist = length(vDir) / halfScreenSize.x;
    vDir = normalize(vDir);
    fDist = max(0.0f, fDist - pc.blurStartDist);

    vDir *= fDist * pc.size * pc.screenDim.x;

    texture2d<float, access::sample> tex = set0.sourceInput;
    sampler s = set0.inputSampler;
    float3 vDiffuseColor = float3(0.0f);
    vDiffuseColor += SampleTextureBlur(tex, s, viewTexel, screenCoord, vDir, -1.0f, 0.1f);
    vDiffuseColor += SampleTextureBlur(tex, s, viewTexel, screenCoord, vDir, -0.5f, 0.2f);
    vDiffuseColor += SampleTextureBlur(tex, s, viewTexel, screenCoord, vDir,  0.0f, 0.5f);
    vDiffuseColor += SampleTextureBlur(tex, s, viewTexel, screenCoord, vDir,  0.5f, 0.2f);
    vDiffuseColor += SampleTextureBlur(tex, s, viewTexel, screenCoord, vDir,  1.0f, 0.1f);
    vDiffuseColor /= 1.1f;
    return float4(vDiffuseColor, 1.0f);
}
