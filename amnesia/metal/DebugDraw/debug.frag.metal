// Hand-written Metal port of amnesia/slang/DebugDraw/debug.frag.slang
//
// Editor / debug overlay batcher — flat vertex color. Pairs with
// debug.vert.metal / debug_2d.vert.metal.
//
// Bindings: none used by this stage (DebugPass UBO unreferenced here).
#include <metal_stdlib>
using namespace metal;

struct PSIn {
    float4 posH [[position]];
    float4 v_color;
};

fragment float4 psMain(PSIn pin [[stage_in]]) {
    return pin.v_color;
}
