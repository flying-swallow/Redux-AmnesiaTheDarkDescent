#pragma once
#include "../HostDefinitions.h"
#include "MultiscaleMeanEstimator.slang"

HOST_NAMESPACE_BEGIN 

// Surfel-GI struct layouts shared between Slang (which `import`s this
// module), C++ (which pulls it in via SceneTypes.slang → `#include`), and GLSL
// (same #include path). The `__init` constructor methods are Slang-only
// and gated to `__SLANG_COMPILER__`; C++/GLSL see plain POD layouts.
// `SurfelRecycleInfo` keeps the Slang three-uint16 layout — host code
// that touches `life/frame/status` does so through fields, so the layout
// has to match what the Slang shaders read.

// Field layout mirrors std430 explicitly so host sizeof matches the Slang
// ArrayStride (128). Pad rules:
//   * float3 followed by another float3 needs a trailing-4-byte pad
//     (position → normal). float3 followed by a float packs naturally
//     (radius packs into normal's tail, sumLuminance packs into radiance's
//     tail) so no pad there.
//   * hasHole is uint (Slang bool is 4 bytes; C++ bool is 1 byte — keep
//     uint for both so layouts match).
//   * msmeData has 16-byte base alignment (it contains float3 fields);
//     pad after hasHole pushes it to offset 64.
//   * Trailing pad rounds the host sizeof up to the Slang ArrayStride of
//     128 (std430 rounds struct stride to its base alignment of 16).
// The explicit pad fields below mirror exactly what Slang's natural layout
// auto-inserts for this struct (verified via `spirv-dis ... | grep
// "%Surfel_natural ="`: 9 real + 6 pad members, ArrayStride 128). They are
// required because the C++ host computes plain C layout (float3 is 12B /
// 4-aligned) and would otherwise size the struct at 104 — undersizing the
// bindless surfel buffer and silently dropping the tail surfels via robust
// access. Same convention as MSMEData's `_padMean`. Keep in lockstep with the
// static_assert(sizeof(Surfel)==128) in HybridRenderer.cpp.
SLANG_PUBLIC struct Surfel
{
    SLANG_PUBLIC float3 position;       
    SLANG_PUBLIC uint   rayCount;       
    SLANG_PUBLIC float3 normal;         
    SLANG_PUBLIC float  radius;         
    SLANG_PUBLIC float3 radiance;       
    SLANG_PUBLIC float  sumLuminance;   
    SLANG_PUBLIC MSMEData msmeData;     
    SLANG_PUBLIC uint   rayOffset;      
    SLANG_PUBLIC uint   hasHole;        

#ifdef __SLANG_COMPILER__
    public __init(float3 position, float3 normal, float radius)
    {
        this.position = position;
        this.normal = normal;
        this.radius = radius;
        this.radiance = float3(0.f);
        this.sumLuminance = 0.f;
        this.hasHole = 0u;
        this.msmeData = MSMEData();
        this.rayOffset = 0;
        this.rayCount = 0;
    }
#endif
};

SLANG_PUBLIC struct CellInfo
{
    SLANG_PUBLIC uint surfelCount;
    SLANG_PUBLIC uint cellToSurfelBufferOffset;
};

// Compact per-surfel cull record read by SurfelGenerationPass's hot loop.
// Split out of the 128-byte `Surfel` so the distance + normal-dot cull touches
// a tight 32-byte record (random gathers hit ~1 cache line instead of ~2)
// instead of pulling the whole surfel; the full `Surfel`
// (radiance/msmeData/rayCount) is fetched only for surfels that survive the
// cull. `normal` is stored pre-normalized so the loop drops its per-iteration
// rsqrt. Populated in SurfelUpdatePass::collectCellInfo alongside the surfel's
// position/normal/radius writeback. Stride is 32 under both scalar and std430
// (radius packs into position's tail @12, normal is 16-aligned @16); the host
// static_assert + `spirv-dis ... | grep ArrayStride` guard against drift.
SLANG_PUBLIC struct SurfelBounds
{
    SLANG_PUBLIC float3 position;  // @0
    SLANG_PUBLIC float  radius;    // @12
    SLANG_PUBLIC float3 normal;    // @16  (pre-normalized)
};

SLANG_PUBLIC struct SurfelRayResult
{
    SLANG_PUBLIC float3 dirLocal;       
    SLANG_PUBLIC float  firstRayLength; 
    SLANG_PUBLIC float3 dirWorld;       
    SLANG_PUBLIC float  pdf;            
    SLANG_PUBLIC float3 radiance;       
    SLANG_PUBLIC uint   surfelIndex;    
};

// [status]
// 0x0001 : isSleeping
// 0x0002 : lastSeen
SLANG_PUBLIC struct SurfelRecycleInfo
{
    SLANG_PUBLIC uint16_t life;
    SLANG_PUBLIC uint16_t frame;
    SLANG_PUBLIC uint16_t status;
};

HOST_NAMESPACE_END
