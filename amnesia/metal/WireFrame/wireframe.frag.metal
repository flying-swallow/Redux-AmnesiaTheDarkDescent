// Hand-written Metal port of amnesia/slang/WireFrame/wireframe.frag.slang
//
// Wireframe debug renderer fragment stage. Flat color from the per-draw UBO.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (WireFramePass UBO) 0
#include <metal_stdlib>
using namespace metal;

struct WireFramePass {
    float4x4 mvp;
    float4   color;
};

struct Set0 {
    constant WireFramePass* pass [[id(0)]];
};

fragment float4 psMain(device Set0& set0 [[buffer(0)]]) {
    return set0.pass->color;
}
