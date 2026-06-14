// Hand-written Metal port of amnesia/slang/SurfelGI/SurfelPreparePass.cs.slang
//
// Stage D — promotes the previous frame's ValidSurfel count into DirtySurfel,
// clamps the FreeSurfel stack to its capacity, and zeros the per-frame
// counters. Dispatched 1x1x1. Cell-grid clearing lives in a separate pass
// because kCellCount exceeds what a 1x1x1 dispatch can cover.
//
// gSurfelCounter layout: [0]=valid [1]=dirty [2]=free [3]=cell
//                        [4]=requestedRay [5]=missBounce
// The set-0 bindless globals are read through the Metal argument buffer (Set0)
// bound at buffer(0); [[id(10)]] == kBindingSurfelCounter.
#include <metal_stdlib>
using namespace metal;

constant uint kTotalSurfelLimit = 150000u;

struct Set0 {
    device uint* gSurfelCounter [[id(10)]];   // kBindingSurfelCounter
};

[[kernel]] void csMain(uint3 dispatchThreadId [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]]) {
    device uint* gSurfelCounter = set0.gSurfelCounter;
    uint dirtySurfelCount = clamp(gSurfelCounter[0], 0u, kTotalSurfelLimit);

    gSurfelCounter[0] = 0u;                                          // valid -> reset
    gSurfelCounter[1] = dirtySurfelCount;                            // dirty <- previous valid
    gSurfelCounter[2] = uint(clamp(as_type<int>(gSurfelCounter[2]),  // free stack clamped to capacity
                                   int(0), int(kTotalSurfelLimit)));
    gSurfelCounter[3] = 0u;                                          // cell
    gSurfelCounter[4] = 0u;                                          // requested ray
    gSurfelCounter[5] = 0u;                                          // miss bounce
}
