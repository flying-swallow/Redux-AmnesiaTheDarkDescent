// Hand-written Metal port of amnesia/slang/Gui/gui.frag.slang
//
// GUI quad fragment stage. Screen-space clip-plane test + optional diffuse
// modulation driven by the textureConfig bitfield.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (GuiPass UBO)     0
//     diffuseSampler         1
//     diffuseMap             2
#include <metal_stdlib>
using namespace metal;

struct GuiPass {
    float4x4 mvp;
    float4   clipPlane[4];
    int      textureConfig;
};

struct Set0 {
    constant GuiPass*              pass           [[id(0)]];
    sampler                        diffuseSampler [[id(1)]];
    texture2d<float, access::sample> diffuseMap   [[id(2)]];
};

struct PSIn {
    float4 posH [[position]];
    float2 v_texcoord;
    float4 v_color;
    float4 v_position;
};

// textureConfig bitfield (mirrors gui.resource.glsl helpers).
static inline bool HasDiffuse(int cfg)    { return (cfg & (1 << 0)) != 0; }
static inline bool HasClipPlanes(int cfg) { return (cfg & (1 << 1)) != 0; }

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]]) {
    constant GuiPass* pass = set0.pass;
    if (HasClipPlanes(pass->textureConfig)) {
        for (int i = 0; i < 4; ++i) {
            float d = dot(pass->clipPlane[i], float4(pin.v_position.xyz, 1.0f));
            if (d < 0.0f)
                discard_fragment();
        }
    }

    float4 color = pin.v_color;
    if (HasDiffuse(pass->textureConfig)) {
        color *= set0.diffuseMap.sample(set0.diffuseSampler, pin.v_texcoord);
    }
    return color;
}
