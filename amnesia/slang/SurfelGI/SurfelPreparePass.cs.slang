// Stage D — promotes the previous frame's ValidSurfel count into
// DirtySurfel, clamps the FreeSurfel stack to its capacity, and zeros the
// per-frame counters. Ported from SurfelGI's SurfelPreparePass.cs.slang.
//
// Cell-grid clearing lives in a separate pass because kCellCount (15.6M)
// exceeds what a 1x1x1 dispatch can cover.

import bindless;
import SceneTypes;
import SurfelGI.SurfelTypes;

[numthreads(1, 1, 1)]
void csMain(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint validSurfelCount = gSurfelCounter[kSurfelCounterValid];
    uint dirtySurfelCount = clamp(validSurfelCount, 0u, kTotalSurfelLimit);

    gSurfelCounter[kSurfelCounterValid]         = 0u;
    gSurfelCounter[kSurfelCounterDirty]         = dirtySurfelCount;
    gSurfelCounter[kSurfelCounterFree]          =
        clamp(asint(gSurfelCounter[kSurfelCounterFree]), 0, (int)kTotalSurfelLimit);
    gSurfelCounter[kSurfelCounterCell]          = 0u;
    gSurfelCounter[kSurfelCounterRequestedRay] = 0u;
    gSurfelCounter[kSurfelCounterMissBounce]   = 0u;
}
