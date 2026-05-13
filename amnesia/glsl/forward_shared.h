#ifndef FORWARD_SHARED_H
#define FORWARD_SHARED_H

#ifdef __cplusplus
#include <cstdint>
namespace hpl {
typedef uint32_t uint;
typedef float    mat4[16];
typedef float    vec4[4];
typedef float    vec3[3];
typedef float    vec2[2];
#endif

// Bindless slot capacities. The C++ side sizes its BindlessPool / SSBO
// allocations from these; the GLSL side uses TEXTURE_SLOT_CAPACITY as the
// `textures_2d[]` array length.
#define OBJECT_SLOT_CAPACITY    (16384u * 2u)
#define TEXTURE_SLOT_CAPACITY   16384u
#define MATERIAL_SLOT_CAPACITY  16384u

// Surfel-GI capacities. Mirrors `kMaxSurfelCount` / `kMaxRayCount` in
// amnesia/glsl/host_device.h — kept here in C-preprocessor form so the C++
// allocator (cHybridRenderer) and any GLSL pass that includes this header
// share a single source of truth for sizing the surfel SSBOs declared in
// bindless.resource.glsl (set 0, bindings 10..16).
#define SURFEL_MAX_CAPACTIY     150000u
#define MAX_RAY_COUNT           (SURFEL_MAX_CAPACTIY * 64u)

// Cell-grid capacities. The cell grid is static infrastructure shared by all
// surfel passes (set 0, bindings 17..19 in bindless.resource.glsl). CELL_COUNT
// mirrors host_device.h::kCellCount = kCellDimension^3 with kCellDimension=64.
// CELL_TO_SURFEL_CAPACITY budgets the flat per-cell surfel-index table: a
// single surfel can intersect up to 27 neighbour cells (3x3x3 stamp in
// surfel_update.comp), so worst case = SURFEL_MAX_CAPACTIY * 27.
#define CELL_GRID_DIM           64u
#define CELL_COUNT              (CELL_GRID_DIM * CELL_GRID_DIM * CELL_GRID_DIM)
#define CELL_TO_SURFEL_CAPACITY (SURFEL_MAX_CAPACTIY * 27u)

// Sentinel returned by the renderer's texture slot allocator when a slot is
// missing or the pool is exhausted. Matches the full-uint32 entries in
// DiffuseMaterial::tex[] — uses UINT32_MAX so it's distinguishable from any
// valid bindless slot index even past the current TEXTURE_SLOT_CAPACITY.
#define INVALID_TEXTURE_INDEX   (0xffffffffu)

// Forward-pass set 0 binding indices (the engine-owned bindless set).
#define BINDING_TEXTURES_2D                 0
#define BINDING_OPAQUE_POSITION_HANDLES     3
#define BINDING_OPAQUE_TANGENT_HANDLES      4
#define BINDING_OPAQUE_NORMAL_HANDLES       5
#define BINDING_OPAQUE_UV0_HANDLES          6
#define BINDING_OPAQUE_COLOR_HANDLES        7
#define BINDING_OPAQUE_INDEX_HANDLES        8
#define BINDING_MATERIAL_SAMPLER            9
// Surfel-GI SSBOs (mirrors bindless.resource.glsl set=0, bindings 10..16).
#define BINDING_SURFEL_COUNTER              10
#define BINDING_SURFEL_BUFFER               11
#define BINDING_SURFEL_ALIVE                12
#define BINDING_SURFEL_DEAD                 13
#define BINDING_SURFEL_DIRTY                14
#define BINDING_SURFEL_RECYCLE              15
#define BINDING_SURFEL_RAY                  16
// Cell-grid SSBOs (mirrors bindless.resource.glsl set=0, bindings 17..19).
#define BINDING_CELL_BUFFER                 17
#define BINDING_CELL_COUNTER                18
#define BINDING_CELL_TO_SURFEL              19
// Scene-object / opaque-material tables (mirrors bindless.resource.glsl
// set=0, bindings 20..21). Slot-allocated by cHybridRenderer alongside the
// other bindless pools (OBJECT_SLOT_CAPACITY / MATERIAL_SLOT_CAPACITY).
#define BINDING_SCENE_OBJECTS               20
#define BINDING_OPAQUE_MATERIAL             21

// Texture-slot indices into the bindless `textures_2d[]` array (set=0,
// binding=0). One uint32 per slot — see per_frame.resource.glsl for the
// `DiffuseMaterial_*Texture_ID` accessors that index this array.
// Slot layout: 0=Diffuse, 1=NMap, 2=Alpha, 3=Specular,
//              4=Height, 5=Illumination, 6=DissolveAlpha, 7=CubeMapAlpha.
struct DiffuseMaterial {
    uint  tex[8];
    uint  materialConfig;
    float heightMapScale;
    float heightMapBias;
    float frenselBias;
    float frenselPow;
};

struct UniformObject {
    float dissolveAmount;
    uint  materialID;
    float lightLevel;
    float illuminationAmount;
    mat4  modelMat;
    mat4  invModelMat;
    mat4  uvMat;
};

// Per-frame UBO contents (std140). On the GLSL side this struct is wrapped
// in a uniform block at set=1, binding=0 (see per_frame.resource.glsl).
struct PerFrameConstants {
    mat4  invViewRotationMat;
    mat4  viewMat;
    mat4  invViewMat;
    mat4  projMat;
    mat4  invProjMat;        // inverse of projMat — for WorldPosFromDepth

    float worldFogStart;
    float worldFogLength;
    float oneMinusFogAlpha;
    float fogFalloffExp;
    vec4  worldFogColor;

    vec2  viewTexel;
    vec2  viewportSize;
    float afT;
    uint  totalFrames;       // monotonic frame counter for randSeed inputs
    float cameraFov;         // main camera vertical FOV (radians)
};

#ifdef __cplusplus
// C++ twins of the surfel structs defined in amnesia/glsl/host_device.h.
// Kept inside the __cplusplus guard so they don't collide with the GLSL
// definitions when this header is included from a shader. Scalar layout on
// the GLSL side packs without alignment padding, which already matches the
// natural C++ layout of `float[3]` + `float`/`uint` fields used here. The
// static_asserts below catch any drift.
struct MSMEData {
    vec3  mean;
    float vbbr;
    vec3  shortMean;
    float inconsistency;
    vec3  variance;
    float pad;
};

struct SurfelCounter {
    uint aliveSurfelCnt;
    uint deadSurfelCnt;
    uint dirtySurfelCnt;
    uint surfelRayCnt;
};

struct Surfel {
    vec3 position;  float radius;
    vec3 radiance;  uint  normal;
    uint objID;
    uint rayOffset;
    uint rayCount;
    uint irradiance;
    MSMEData msmeData;
};

struct SurfelRecycleInfo {
    uint life;
    uint frame;
    uint status;
    uint lastSeenFrame;
};

struct SurfelRay {
    uint  surfelID;
    uint  dir_o;
    float pdf;
    float pad;
    vec3  radiance;
    float t;
};

struct CellInfo {
    uint surfelOffset;
    uint surfelCount;
};

struct CellCounter {
    uint totalCellCount;
    uint aliveSurfelInCell;
};

// Historical C++ names; the canonical names match the GLSL twins above.
using ObjectGPUData      = UniformObject;
using DiffuseMaterialGPU = DiffuseMaterial;

} // namespace hpl
#endif

#endif // FORWARD_SHARED_H
