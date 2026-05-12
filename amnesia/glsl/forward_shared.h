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

// Sentinel returned by the renderer's texture slot allocator when a slot is
// missing or the pool is exhausted; matches the lo16/hi16 packed index in
// DiffuseMaterial::tex[].
#define INVALID_TEXTURE_INDEX   (0xffffu)

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

struct DiffuseMaterial {
    uint  tex[4];
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
// in a uniform block at set=1, binding=0 (see forward_shader_common.glsl).
struct PerFrameConstants {
    mat4  invViewRotationMat;
    mat4  viewMat;
    mat4  invViewMat;
    mat4  projMat;
    mat4  viewProjMat;

    float worldFogStart;
    float worldFogLength;
    float oneMinusFogAlpha;
    float fogFalloffExp;
    vec4  worldFogColor;

    vec2  viewTexel;
    vec2  viewportSize;
    float afT;
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

static_assert(sizeof(MSMEData)          == 48, "MSMEData scalar layout drift");
static_assert(sizeof(SurfelCounter)     == 16, "SurfelCounter scalar layout drift");
static_assert(sizeof(Surfel)            == 96, "Surfel scalar layout drift");
static_assert(sizeof(SurfelRecycleInfo) == 16, "SurfelRecycleInfo scalar layout drift");
static_assert(sizeof(SurfelRay)         == 32, "SurfelRay scalar layout drift");

static_assert(sizeof(DiffuseMaterial) == 36,
              "DiffuseMaterial std430 layout drift");
static_assert(sizeof(UniformObject) == 208,
              "UniformObject std430 layout drift");
// PerFrameConstants std140 size: 5*64 (matrices) + 4*4 (fog scalars) + 16
// (worldFogColor) + 8 (viewTexel) + 8 (viewportSize) + 4 (afT) = 372,
// padded up to a multiple of 16 for the block = 384 with no trailing fields.
// The C++ struct ends at 372; downstream code zero-initializes it and writes
// a 372-byte payload, which the GLSL UBO reads as 372 useful bytes inside a
// 384-byte block. No explicit trailing pad needed at the C++ level.
static_assert(sizeof(PerFrameConstants) == 372,
              "PerFrameConstants std140 layout drift");

// Historical C++ names; the canonical names match the GLSL twins above.
using ObjectGPUData      = UniformObject;
using DiffuseMaterialGPU = DiffuseMaterial;

} // namespace hpl
#endif

#endif // FORWARD_SHARED_H
