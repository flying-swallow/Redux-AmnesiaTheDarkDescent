// Hand-written Metal port of amnesia/slang/SurfelGI/SurfelUpdatePass.cs.slang
//
// Three entry points covering the surfel lifecycle:
//   collectCellInfo          (32,1,1)  promote dirty->valid, free dead, count
//                                      cell occupancy, allocate rays.
//   accumulateCellInfo       (4,4,4)   prefix-sum cell counts into offsets.
//   updateCellToSurfelBuffer (32,1,1)  scatter valid surfel indices into cells.
//
// Self-contained; scalar struct layouts (packed types). Set-0 globals are read
// through one shared Set0 argument buffer (buffer(0), [[id]] = kBinding*). The
// three kernels share Set0; gPerFrame is direct at buffer(1).
#include <metal_stdlib>
using namespace metal;

constant float kCellUnit      = 0.5f;
constant uint  kSurfelNeighborCellCount = 125u;
constant uint  kRefCountThreshold = 32u;
constant float kSurfelTargetArea = 50000.0f;
constant uint  kDefaultMinRayCount = 4u;
constant uint  kDefaultMaxRayCount = 64u;
constant uint  kRayBudget = 150000u * 64u;
constant float kDefaultVarianceSensitivity = 40.0f;
constant uint  kSurfelCounterValid = 0u, kSurfelCounterDirty = 1u, kSurfelCounterFree = 2u;
constant uint  kSurfelCounterCell = 3u, kSurfelCounterRequestedRay = 4u;

// --- Shared struct layouts + utilities: mirrored from the Slang modules
// (flattened into this file at stage time by cmake/flatten_metal_includes.py).
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, HitInfo, TriangleHit, VertexData, lights
#include "SurfelGI/SurfelTypes.metalh"  // Surfel, CellInfo, SurfelRayResult, SurfelRecycleInfo, SurfelBounds (+ MSMEData)
#include "SurfelGI/SurfelUtils.metalh"  // cell math, surfelNeighborCellPos, isSurfelIntersectCell, calcSurfelRadius (+ M_PI_, kCellDimension)

struct Set0 {
    device ulong*          gOpaquePositionHandles [[id(3)]];
    device ulong*          gOpaqueTangentHandles  [[id(4)]];
    device ulong*          gOpaqueNormalHandles   [[id(5)]];
    device ulong*          gOpaqueUv0Handles      [[id(6)]];
    device ulong*          gOpaqueIndexHandles    [[id(8)]];
    device uint*           gSurfelCounter         [[id(10)]];
    device Surfel*         gSurfelBuffer          [[id(11)]];
    device uint4*          gSurfelGeometryBuffer  [[id(12)]];
    device uint*           gSurfelValidIndexBuffer [[id(13)]];
    device uint*           gSurfelDirtyIndexBuffer [[id(14)]];
    device uint*           gSurfelFreeIndexBuffer [[id(15)]];
    device SurfelRecycleInfo* gSurfelRecycleInfoBuffer [[id(16)]];
    device SurfelRayResult* gSurfelRayResultBuffer [[id(17)]];
    device CellInfo*       gCellInfoBuffer        [[id(18)]];
    device uint*           gCellToSurfelBuffer    [[id(19)]];
    device UniformObject*  gSceneObjects          [[id(20)]];
    device uint*           gSurfelRefCounter      [[id(27)]];
    device uint*           gSurfelReservationBuffer [[id(28)]];
    device SurfelBounds*   gSurfelBoundsBuffer    [[id(37)]];
    device uint*           gBindlessSlotGeneration [[id(38)]];
    device uint*           gSurfelSlotGeneration  [[id(39)]];
};
// Set-1 program argument buffer (buffer(1)); [[id]] = VK binding within set 1.
struct Set1 { constant SceneConstants* gPerFrame [[id(0)]]; };

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // TriVtx, loadMat, getVertexData, getTriangleHit
#include "SurfelGI/SurfelLifecycle.metalh"   // releaseSurfel, tickSurfelLifecycle, isAnchorStale (+ kMaxLife, kSleepingMaxLife, kObjectSlotCapacity, kTotalSurfelLimit)

[[kernel]] void collectCellInfo(uint3 dispatchThreadId [[thread_position_in_grid]],
                                device Set0& set0 [[buffer(0)]],
                                device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    uint dirtySurfelCount = set0.gSurfelCounter[kSurfelCounterDirty];
    if (dispatchThreadId.x >= dirtySurfelCount) return;

    uint surfelIndex = set0.gSurfelDirtyIndexBuffer[dispatchThreadId.x];
    Surfel surfel = set0.gSurfelBuffer[surfelIndex];

    SurfelRecycleInfo recycle = set0.gSurfelRecycleInfoBuffer[surfelIndex];
    bool isSleeping = (recycle.status & 0x0001u) != 0u;
    bool hasEnoughRefCount = set0.gSurfelRefCounter[surfelIndex] > kRefCountThreshold;
    recycle = tickSurfelLifecycle(recycle, hasEnoughRefCount, isSleeping);

    TriangleHit hit = getTriangleHit(set0.gSurfelGeometryBuffer[surfelIndex]);

    bool alive = surfel.radius > 0.0f && recycle.life > 0u && !isAnchorStale(surfelIndex, hit, set0);
    if (!alive) { releaseSurfel(surfelIndex, set0); return; }

    uint validSlot = atomic_fetch_add_explicit((device atomic_uint*)&set0.gSurfelCounter[kSurfelCounterValid], 1u, memory_order_relaxed);
    set0.gSurfelValidIndexBuffer[validSlot] = surfelIndex;

    float3 cameraPos  = gPerFrame->posW;
    uint2  resolution = uint2(gPerFrame->viewportSize);
    float  fovy       = gPerFrame->cameraFov;

    // reprojectSurfel
    VertexData data = getVertexData(hit, set0);
    surfel.position = data.posW;
    surfel.normal   = data.normalW;
    surfel.radius   = calcSurfelRadius(distance(cameraPos, data.posW), fovy, resolution,
                                       kSurfelTargetArea * (isSleeping ? 16.0f : 1.0f), kCellUnit);
    if (isSleeping) surfel.radius = max(surfel.radius, kCellUnit * 0.5f);

    // publishSurfelBounds
    SurfelBounds bounds;
    bounds.position = surfel.position;
    bounds.radius   = surfel.radius;
    bounds.normal   = normalize(float3(surfel.normal));
    set0.gSurfelBoundsBuffer[surfelIndex] = bounds;

    // countIntersectingCells
    int3 cellPos = getCellPos(float3(surfel.position), cameraPos, kCellUnit);
    for (uint i = 0u; i < kSurfelNeighborCellCount; ++i) {
        int3 neighborPos = surfelNeighborCellPos(cellPos, i);
        if (isSurfelIntersectCell(float3(surfel.position), surfel.radius, neighborPos, cameraPos, kCellUnit)) {
            uint fi = getFlattenCellIndex(neighborPos);
            atomic_fetch_add_explicit((device atomic_uint*)&set0.gCellInfoBuffer[fi].surfelCount, 1u, memory_order_relaxed);
        }
    }

    // allocateSurfelRays
    uint lower = isSleeping ? (kDefaultMinRayCount / 4u) : (kDefaultMaxRayCount / 4u);
    uint upper = isSleeping ?  kDefaultMinRayCount       :  kDefaultMaxRayCount;
    uint rayRequestCount = uint(clamp(mix(float(lower), float(upper),
                                          length(float3(surfel.msmeData.variance)) * kDefaultVarianceSensitivity),
                                      float(lower), float(upper)));
    uint rayOffset = atomic_fetch_add_explicit((device atomic_uint*)&set0.gSurfelCounter[kSurfelCounterRequestedRay], rayRequestCount, memory_order_relaxed);
    if (rayOffset < kRayBudget) {
        surfel.rayOffset = rayOffset;
        surfel.rayCount  = rayRequestCount;
        SurfelRayResult initRR;
        initRR.dirLocal = float3(0.0f); initRR.firstRayLength = 0.0f; initRR.dirWorld = float3(0.0f);
        initRR.pdf = 0.0f; initRR.radiance = float3(0.0f); initRR.surfelIndex = surfelIndex;
        for (uint r = 0u; r < rayRequestCount; ++r)
            set0.gSurfelRayResultBuffer[rayOffset + r] = initRR;
    }

    recycle.status = isSleeping ? ushort(0x0001) : ushort(0x0000);   // clears lastSeen
    set0.gSurfelRecycleInfoBuffer[surfelIndex] = recycle;
    set0.gSurfelBuffer[surfelIndex] = surfel;
    set0.gSurfelRefCounter[surfelIndex] = 0u;
}

[[kernel]] void accumulateCellInfo(uint3 dispatchThreadId [[thread_position_in_grid]],
                                   device Set0& set0 [[buffer(0)]],
                                   device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame; (void)gPerFrame;
    if (any(dispatchThreadId >= uint3(kCellDimension))) return;
    uint flattenIndex = dispatchThreadId.z * (kCellDimension * kCellDimension)
                      + dispatchThreadId.y * kCellDimension + dispatchThreadId.x;
    set0.gSurfelReservationBuffer[flattenIndex] = 0u;
    if (set0.gCellInfoBuffer[flattenIndex].surfelCount == 0u) return;

    uint old = atomic_fetch_add_explicit((device atomic_uint*)&set0.gSurfelCounter[kSurfelCounterCell],
                                         set0.gCellInfoBuffer[flattenIndex].surfelCount, memory_order_relaxed);
    set0.gCellInfoBuffer[flattenIndex].cellToSurfelBufferOffset = old;
    set0.gCellInfoBuffer[flattenIndex].surfelCount = 0u;
}

[[kernel]] void updateCellToSurfelBuffer(uint3 dispatchThreadId [[thread_position_in_grid]],
                                         device Set0& set0 [[buffer(0)]],
                                         device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    uint validSurfelCount = set0.gSurfelCounter[kSurfelCounterValid];
    if (dispatchThreadId.x >= validSurfelCount) return;

    uint surfelIndex = set0.gSurfelValidIndexBuffer[dispatchThreadId.x];
    Surfel surfel = set0.gSurfelBuffer[surfelIndex];

    float3 cameraPos = gPerFrame->posW;
    int3 cellPos = getCellPos(float3(surfel.position), cameraPos, kCellUnit);
    for (uint i = 0u; i < kSurfelNeighborCellCount; ++i) {
        int3 neighborPos = surfelNeighborCellPos(cellPos, i);
        if (isSurfelIntersectCell(float3(surfel.position), surfel.radius, neighborPos, cameraPos, kCellUnit)) {
            uint fi = getFlattenCellIndex(neighborPos);
            uint prevCount = atomic_fetch_add_explicit((device atomic_uint*)&set0.gCellInfoBuffer[fi].surfelCount, 1u, memory_order_relaxed);
            set0.gCellToSurfelBuffer[set0.gCellInfoBuffer[fi].cellToSurfelBufferOffset + prevCount] = surfelIndex;
        }
    }
}
