// SurfelClearByInstance — frees surfels anchored to geometry that was
// destroyed mid-level. The host queues the bindless object slots a destroyed
// vertex buffer occupied (VertexBuffer_RI::s_onGiSlotRetired) into gClearParams;
// this pass marks every surfel whose cached hit references one of those slots
// dead (radius = 0). SurfelUpdatePass::collectCellInfo then returns it to the
// free list next — and, critically, radius == 0 makes collectCellInfo skip the
// gScene.getVertexData() pull this frame, so it never dereferences the freed
// vertex buffer-device-address (which would GPUVM-fault).
//
// Matching is by bindless slot (TriangleHit.instanceID == retired slot). This
// can only over-clear — a surfel sitting on a slot that was already reused by a
// live object just respawns next frame — so it is always memory-safe. The
// map-load case is handled separately by the host's resetSurfelState().
//
// Run order: after SurfelPreparePass (+ valid->dirty copy) and before
// collectCellInfo. Dispatched only when the retired-slot list is non-empty.

#include "../Constants.h"

import Bindless.resource;       // gSurfelBuffer, gSurfelGeometryBuffer
import SceneTypes;              // kTotalSurfelLimit
import SurfelGI.SurfelTypes;    // Surfel

// Per-dispatch list of retired bindless object slots. Bound by name
// ("gClearParams") through the standard UpdateFrameUBO path; scalar layout
// (see cmake/shaders.cmake -fvk-use-scalar-layout) packs the uint[] tightly so
// it matches the host's plain-C struct of the same shape. Pure slots[] — the
// live count rides a push constant (below) so this UBO can fill the whole
// guaranteed 16 KB uniform range with slots.
struct SurfelClearParams
{
    uint slots[HPL_RETIRED_SLOTS_PER_DISPATCH];
};

[vk::binding(0, 2)]
ConstantBuffer<SurfelClearParams> gClearParams;

// Number of valid entries in gClearParams.slots for this dispatch. Written via
// vkCmdPushConstants from cHybridRenderer::Draw (VK_SHADER_STAGE_COMPUTE_BIT).
struct SurfelClearPush
{
    uint retiredCount;
};

[vk::push_constant]
SurfelClearPush gPush;

[numthreads(256, 1, 1)]
void csMain(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint i = dispatchThreadId.x;
    if (i >= kTotalSurfelLimit)
        return;

    Surfel surfel = gSurfelBuffer[i];
    if (surfel.radius <= 0.f)
        return; // already free/dead — nothing to clear.

    // instanceID is packed.x of the cached TriangleHit (Bindless/interface.slang
    // TriangleHit::pack), i.e. the bindless object slot this surfel anchors to.
    uint instanceID = gSurfelGeometryBuffer[i].x;

    uint count = min(gPush.retiredCount, (uint)HPL_RETIRED_SLOTS_PER_DISPATCH);
    // ^ count is this dispatch's batch; the host loops dispatches to cover all
    //   retired slots in the frame (see cHybridRenderer::Draw).
    for (uint s = 0; s < count; ++s)
    {
        if (gClearParams.slots[s] == instanceID)
        {
            surfel.radius = 0.f;
            gSurfelBuffer[i] = surfel;
            return;
        }
    }
}
