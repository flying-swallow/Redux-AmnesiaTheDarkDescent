#pragma once
// Shared surfel spawn / release machinery — the free/valid counter ladder and
// the new-surfel publication, previously duplicated between
// SurfelGenerationPass (elected spawn) and SurfelRayTrace's finalize
// (sleeping reservation spawn). The ladders were identical including the
// deliberate no-rollback-on-failure semantics; only the initialization values
// differ, so those stay per-site.

import bindless;                // gSurfelCounter / index buffers / surfel buffers
import SceneTypes;              // kSurfelCounter* / kTotalSurfelLimit
import SurfelGI.SurfelTypes;    // Surfel, SurfelRecycleInfo

// Pop a free slot + reserve a valid-list entry. Reproduces the reference's
// no-rollback semantics exactly: the free counter stays decremented and the
// valid counter stays incremented even on the failure paths (the counters are
// repaired by the per-frame prepare/update passes — do NOT "fix" this by
// rolling back, it would change pool accounting). On true the caller MUST
// publish (publishNewSurfel) for newIndex/validSlot.
public bool tryAllocateSurfelSlot(out uint newIndex, out uint validSlot)
{
    newIndex  = 0u;
    validSlot = 0u;

    uint freeSurfelCount;
    InterlockedAdd(gSurfelCounter[kSurfelCounterFree], (uint)-1, freeSurfelCount);
    if (!(0u < freeSurfelCount && freeSurfelCount <= kTotalSurfelLimit))
        return false;

    uint validSurfelCount;
    InterlockedAdd(gSurfelCounter[kSurfelCounterValid], 1u, validSurfelCount);
    if (validSurfelCount >= kTotalSurfelLimit)
        return false;

    newIndex  = gSurfelFreeIndexBuffer[freeSurfelCount - 1u];
    validSlot = validSurfelCount;
    return true;
}

// Write the per-surfel buffers for a freshly allocated slot. instanceID must
// already be bounds-checked (< kObjectSlotCapacity) by the caller — both
// spawn sites gate on it BEFORE touching any counters, because
// gBindlessSlotGeneration[instanceID] is an unmapped SSBO read past the slot
// table. The slot-generation capture lets collectCellInfo invalidate this
// surfel if the anchor slot is later reassigned to different geometry.
public void publishNewSurfel(uint newIndex, uint validSlot,
                             Surfel s, SurfelRecycleInfo recycle,
                             uint4 packedHit, uint instanceID,
                             uint initialRefCount)
{
    gSurfelValidIndexBuffer[validSlot]  = newIndex;
    gSurfelBuffer[newIndex]             = s;
    gSurfelRecycleInfoBuffer[newIndex]  = recycle;
    gSurfelGeometryBuffer[newIndex]     = packedHit;
    gSurfelSlotGeneration[newIndex]     = gBindlessSlotGeneration[instanceID];
    gSurfelRefCounter[newIndex]         = initialRefCount;
}

// Return a surfel slot to the free list (collectCellInfo's recycle path).
public void releaseSurfel(uint surfelIndex)
{
    uint freeSurfelCount;
    InterlockedAdd(gSurfelCounter[kSurfelCounterFree], 1u, freeSurfelCount);
    gSurfelFreeIndexBuffer[freeSurfelCount] = surfelIndex;
}
