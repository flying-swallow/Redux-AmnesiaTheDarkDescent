#pragma once

// HitInfo / TriangleHit (un)packing — matches the Falcor source-compat surface
// used by the V-buffer / direct-lighting / path-trace passes:
// `HitInfo h = HitInfo(packedUVec4)`, `h.isValid()`,
// `h.getTriangleHit()`, and `TriangleHit::pack()` for writes. The underlying
// structs (data layout) live in SceneTypes.slang; these extensions add the
// pack/unpack behaviour.

import SceneTypes;

public extension HitInfo
{
    public bool isValid()
    {
        return any(packed != uint4(0u, 0u, 0u, 0u));
    }

    public TriangleHit getTriangleHit()
    {
        TriangleHit h;
        h.instanceID     = packed.x;
        h.primitiveIndex = packed.y;
        h.barycentrics   = float2(
            f16tof32(packed.z & 0xFFFFu),
            f16tof32((packed.z >> 16) & 0xFFFFu));
        return h;
    }
};

public extension TriangleHit
{
    public uint4 pack()
    {
        uint4 r;
        r.x = instanceID;
        r.y = primitiveIndex;
        r.z = (f32tof16(barycentrics.x) & 0xFFFFu)
            | ((f32tof16(barycentrics.y) & 0xFFFFu) << 16);
        r.w = 1u;  // non-zero w so HitInfo::isValid() returns true even when
                   // instance id, prim index, and barys all hash to zero.
        return r;
    }
};
