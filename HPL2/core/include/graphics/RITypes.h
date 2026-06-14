
#ifndef RI_TYPES_H
#define RI_TYPES_H

#include "graphics/RIBarrier.h"
#include "graphics/RIDefines.h"
#include "graphics/RIFormat.h"
#include <cassert>
#include <cstring>

#ifdef DEVICE_SUPPORT_VULKAN
#include "volk.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#endif

#ifdef DEVICE_SUPPORT_MTL
#include <Metal/Metal.hpp>           // MTL:: and NS:: (Foundation pulled in transitively)
#include <QuartzCore/QuartzCore.hpp> // CA::MetalLayer / CA::MetalDrawable
#endif

// Macro anguish
#ifdef LoadBitmap
#undef LoadBitmap
#endif

#ifdef SendMessage
#undef SendMessage
#endif

#ifdef CreateEvent
#undef CreateEvent
#endif

#ifdef CreateWindow
#undef CreateWindow
#endif

#include <cstdio>
#include <stdint.h>
#include <variant>
#include <vector>
#undef DestroyAll
#undef ButtonPress

#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

 

#if (DEVICE_IMPL_VULKAN)
#define R_VK_ADD_STRUCT(current, next)                                         \
  {                                                                            \
    void *__pNext = (void *)((current)->pNext);                                \
    (current)->pNext = (next);                                                 \
    (next)->pNext = __pNext;                                                   \
  }
#define VK_WrapResult(res)                                                     \
  __VK_WrapResult(res, __FILE__, __FUNCTION__, __LINE__)

static inline bool __VK_WrapResult(VkResult result, const char *sourceFilename,
                                   const char *functionName, int sourceLine) {
  if (result != VK_SUCCESS) {
    hpl::Log("RI: VK %i, file %s:%i (%s)\n", result, sourceFilename, sourceLine,
             functionName);
    return false;
  }
  return true;
}
#endif

#define RI_QUEUE_GRAPHICS_BIT 0x1
#define RI_QUEUE_COMPUTE_BIT 0x2
#define RI_QUEUE_TRANSFER_BIT 0x4
#define RI_QUEUE_SPARSE_BINDING_BIT 0x8
#define RI_QUEUE_VIDEO_DECODE_BIT 0x10
#define RI_QUEUE_VIDEO_ENCODE_BIT 0x20
#define RI_QUEUE_PROTECTED_BIT 0x40
#define RI_QUEUE_OPTICAL_FLOW_BIT_NV 0x80
#define RI_QUEUE_INVALID 0x0

enum RIPresetLevel_e {
  RI_GPU_PRESET_NONE = 0,
  RI_GPU_PRESET_OFFICE,  // This means unsupported
  RI_GPU_PRESET_VERYLOW, // Mostly for mobile GPU
  RI_GPU_PRESET_LOW,
  RI_GPU_PRESET_MEDIUM,
  RI_GPU_PRESET_HIGH,
  RI_GPU_PRESET_ULTRA,
  RI_GPU_PRESET_COUNT
};

enum RITextureViewType_e {
  RI_VIEWTYPE_SHADER_RESOURCE_1D,
  RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_2D,
  RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_CUBE,
  RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_3D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D,

  RI_VIEWTYPE_COLOR_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_READONLY_STENCIL_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_ATTACHMENT_STENCIL_READONLY,
  RI_VIEWTYPE_DEPTH_STENCIL_READONLY,
  RI_VIEWTYPE_SHADING_RATE_ATTACHMENT
};

enum RITextureUsageBits_e {
  RI_USAGE_NONE = 0,
  RI_USAGE_SHADER_RESOURCE = 0x1,
  RI_USAGE_SHADER_RESOURCE_STORAGE = 0x2,
  RI_USAGE_COLOR_ATTACHMENT = 0x4,
  RI_USAGE_DEPTH_STENCIL_ATTACHMENT = 0x8,
  RI_USAGE_SHADING_RATE = 0x10,
  RI_USAGE_TRANSFER_SRC = 0x20,
  RI_USAGE_TRANSFER_DST = 0x40,
};

// Image-creation flags for RITextureDesc (backend-neutral). On Vulkan
// MUTABLE_FORMAT implies the mutable+extended-usage create flags; on Metal it
// maps to the PixelFormatView texture-usage bit. CUBE_COMPATIBLE allows cube
// views over a 6*N-layer 2D array.
enum RITextureFlagBits_e {
  RI_TEXTURE_FLAG_NONE = 0,
  RI_TEXTURE_FLAG_MUTABLE_FORMAT = 0x1,
  RI_TEXTURE_FLAG_CUBE_COMPATIBLE = 0x2,
  // [mtl] Allocate from the device's shared bindless texture heap so the
  // resource can be made resident for argument-buffer access with a single
  // useHeap() (vs. useResource per texture). Set on material/image textures
  // that populate the bindless gTextures2D/gTexturesCube arrays. No-op on
  // Vulkan. Falls back to a direct device allocation if the heap is exhausted.
  RI_TEXTURE_FLAG_BINDLESS_HEAP = 0x4,
};

// Backend-neutral image-creation descriptor consumed by RITexture::create.
struct RITextureDesc {
  uint8_t type; // RITextureType_e
  uint32_t format; // RI_Format_e
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint16_t mipNum;
  uint16_t layerNum;
  uint32_t usage; // RITextureUsageBits_e bitmask
  uint32_t flags; // RITextureFlagBits_e bitmask
};

// Backend-neutral image-view descriptor consumed by RITextureView::create.
struct RITextureViewDesc {
  enum RITextureViewType_e viewType;
  uint32_t format; // RI_Format_e (view format; may reinterpret the image format)
  uint32_t baseMip;
  uint32_t mipNum;
  uint32_t baseLayer;
  uint32_t layerNum;
};

enum RISampleCount_e {
  RI_SAMPLE_COUNT_1 = 1,
  RI_SAMPLE_COUNT_2 = 2,
  RI_SAMPLE_COUNT_4 = 4,
  RI_SAMPLE_COUNT_8 = 8,
  RI_SAMPLE_COUNT_16 = 16,
  RI_SAMPLE_COUNT_COUNT = 5,
};

enum RIDeviceAPI_e {
  RI_DEVICE_API_UNKNOWN,
  RI_DEVICE_API_VK,
  RI_DEVICE_API_D3D11,
  RI_DEVICE_API_D3D12,
  RI_DEVICE_API_MTL
};

enum RISwapchainFormat_e {
  RI_SWAPCHAIN_BT709_G10_16BIT,
  RI_SWAPCHAIN_BT709_G22_8BIT,
  RI_SWAPCHAIN_BT709_G22_10BIT,
  RI_SWAPCHAIN_BT2020_G2084_10BIT
};

enum RIQueueType_e {
  RI_QUEUE_GRAPHICS,
  RI_QUEUE_COMPUTE,
  RI_QUEUE_COPY,
  RI_QUEUE_LEN
};

enum RIAdapterType_e {
  RI_ADAPTER_TYPE_OTHER,
  RI_ADAPTER_TYPE_CPU,
  RI_ADAPTER_TYPE_VIRTUAL_GPU,
  RI_ADAPTER_TYPE_INTEGRATED_GPU,
  RI_ADAPTER_TYPE_DISCRETE_GPU,
};

enum RIResult_e {
  RI_INCOMPLETE_DEVICE = -2,
  RI_FAIL = -1,
  RI_SUCCESS = 0,
  RI_INCOMPLETE
};

enum RIBufferUsage_e {
  RI_BUFFER_USAGE_NONE = 0,
  RI_BUFFER_USAGE_SHADER_RESOURCE = 0x1,
  RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE = 0x2,
  RI_BUFFER_USAGE_VERTEX_BUFFER = 0x4,
  RI_BUFFER_USAGE_INDEX_BUFFER = 0x8,
  RI_BUFFER_USAGE_CONSTANT_BUFFER = 0x10,
  RI_BUFFER_USAGE_ARGUMENT_BUFFER = 0x20,

  RI_BUFFER_USAGE_SCRATCH = 0x40,
  RI_BUFFER_USAGE_BINDING_TABLE = 0x80,
  RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT = 0x100,
  RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE = 0x200,
  RI_BUFFER_USAGE_TRANSFER_SRC = 0x400,
  RI_BUFFER_USAGE_TRANSFER_DST = 0x800,
  RI_BUFFER_USAGE_INDIRECT = 0x1000,
  // Buffer must be addressable as a raw GPU pointer (Vulkan BDA /
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT). No-op on Metal (gpuAddress is
  // always available).
  RI_BUFFER_USAGE_DEVICE_ADDRESS = 0x2000,
};

// Where a buffer's memory lives, expressed backend-neutrally. Maps to VMA
// memory usage + flags on Vulkan and to MTL storage modes on Metal.
enum RIMemoryLocation_e {
  // Device-local, not host-mapped (VMA AUTO_PREFER_DEVICE / MTL Private).
  // mappedAddress is null; seed via the resource uploader.
  RI_MEMORY_DEVICE,
  // Persistently mapped for sequential host writes
  // (VMA AUTO + MAPPED + HOST_ACCESS_SEQUENTIAL_WRITE / MTL Shared).
  RI_MEMORY_HOST_UPLOAD,
};

// Backend-neutral buffer creation descriptor consumed by RIBuffer::create.
struct RIBufferDesc {
  uint64_t size;
  uint32_t usage; // RIBufferUsage_e bitmask
  RIMemoryLocation_e location;
  uint64_t alignment; // 0 = no special alignment requirement
};

enum RITextureType_e { RI_TEXTURE_1D, RI_TEXTURE_2D, RI_TEXTURE_3D };

enum RIVendor_e { RI_UNKNOWN, RI_NVIDIA, RI_AMD, RI_INTEL };

enum RITopology_e {
  RI_TOPOLOGY_POINT_LIST,
  RI_TOPOLOGY_LINE_LIST,
  RI_TOPOLOGY_LINE_STRIP,
  RI_TOPOLOGY_TRIANGLE_LIST,
  RI_TOPOLOGY_TRIANGLE_STRIP,
  RI_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
  RI_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
  RI_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
  RI_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
  RI_TOPOLOGY_PATCH_LIST
};

// R - fragment's depth or stencil reference
// D - depth or stencil buffer
enum RICompareFunc_e {
  RI_COMPARE_NONE,         // test is disabled
  RI_COMPARE_ALWAYS,       // true
  RI_COMPARE_NEVER,        // false
  RI_COMPARE_EQUAL,        // R == D
  RI_COMPARE_NOT_EQUAL,    // R != D
  RI_COMPARE_LESS,         // R < D
  RI_COMPARE_LESS_EQUAL,   // R <= D
  RI_COMPARE_GREATER,      // R > D
  RI_COMPARE_GREATER_EQUAL // R >= D
};

enum RICullMode_e {
  RI_CULL_MODE_NONE = 0,
  RI_CULL_MODE_FRONT = 0x1,
  RI_CULL_MODE_BACK = 0x2,
  RI_CULL_MODE_BOTH = RI_CULL_MODE_FRONT | RI_CULL_MODE_BACK
};

// Polygon rasterization fill mode. Maps to VkPolygonMode on Vulkan and
// MTL::TriangleFillMode on Metal (where it is encoder state, not pipeline state).
enum RIFillMode_e { RI_FILL_SOLID, RI_FILL_LINE };

enum RIIndexType_e { RI_INDEX_TYPE_16, RI_INDEX_TYPE_32 };

// Backend-neutral descriptor type. RIDescriptor stores one of these in its
// `type` field; the Vulkan path maps it to VkDescriptorType and the Metal path
// to RIMTLDescriptorType_e / MTL::DataType via ri_vk_/ri_mtl_BindlessDescriptorType.
enum RIDescriptorType_e {
  RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
  RI_DESCRIPTOR_TYPE_STORAGE_IMAGE,
  RI_DESCRIPTOR_TYPE_SAMPLER,
  RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
  RI_DESCRIPTOR_TYPE_STORAGE_BUFFER,
  RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE,
};

// Shader-stage visibility bitmask. Values mirror VkShaderStageFlagBits so the
// Vulkan path casts directly; Metal argument buffers ignore stage visibility.
enum RIShaderStageBits_e {
  RI_SHADER_STAGE_VERTEX = 0x00000001,
  RI_SHADER_STAGE_FRAGMENT = 0x00000010,
  RI_SHADER_STAGE_COMPUTE = 0x00000020,
  RI_SHADER_STAGE_RAYGEN = 0x00000100,
  RI_SHADER_STAGE_ANY_HIT = 0x00000200,
  RI_SHADER_STAGE_CLOSEST_HIT = 0x00000400,
  RI_SHADER_STAGE_MISS = 0x00000800,
};

// Bindless binding flags. Values mirror VkDescriptorBindingFlagBits (Vulkan-only
// meaning; Metal ignores them).
enum RIBindlessBindingFlagBits_e {
  RI_BINDLESS_UPDATE_AFTER_BIND = 0x00000001,
  RI_BINDLESS_PARTIALLY_BOUND = 0x00000004,
};

// Backend-neutral pipeline bind point (maps to VkPipelineBindPoint on Vulkan;
// on Metal the encoder type is implicit). Keeps RIProgram's public API free of
// Vulkan enums so the header parses under DEVICE_SUPPORT_MTL.
enum RIPipelineBindPoint_e {
  RI_PIPELINE_BIND_GRAPHICS,
  RI_PIPELINE_BIND_COMPUTE,
  RI_PIPELINE_BIND_RAY_TRACING
};

// Backend-neutral byte size/offset for RI command signatures. Same width as
// VkDeviceSize so the Vulkan bodies pass it straight through, but it keeps the
// abstract RICmd interface free of Vulkan types (so the header parses on the
// Metal build, which doesn't include the Vulkan headers).
typedef uint64_t RIDeviceSize;

// Indirect draw argument structs. Byte-for-byte identical to both
// VkDrawIndirectCommand / MTLDrawPrimitivesIndirectArguments and their indexed
// siblings, so the buffer the renderer fills is consumed verbatim by either
// backend with no conversion. Kept here (not as a Vulkan type) so the indirect
// path compiles on the Metal build, which doesn't include the Vulkan headers.
struct RIDrawIndirectCommand {
  uint32_t vertexCount;
  uint32_t instanceCount;
  uint32_t firstVertex;   // Metal: vertexStart
  uint32_t firstInstance; // Metal: baseInstance
};
static_assert(sizeof(RIDrawIndirectCommand) == 16,
              "RIDrawIndirectCommand must match VkDrawIndirectCommand / "
              "MTLDrawPrimitivesIndirectArguments layout");

struct RIDrawIndexedIndirectCommand {
  uint32_t indexCount;
  uint32_t instanceCount;
  uint32_t firstIndex;    // Metal: indexStart
  int32_t  vertexOffset;  // Metal: baseVertex
  uint32_t firstInstance; // Metal: baseInstance
};
static_assert(sizeof(RIDrawIndexedIndirectCommand) == 20,
              "RIDrawIndexedIndirectCommand must match "
              "VkDrawIndexedIndirectCommand / "
              "MTLDrawIndexedPrimitivesIndirectArguments layout");

enum RIAccelStructureType_e {
  RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL,
  RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL
};

enum RIAccelStructureBuildBits_e {
  RI_ACCEL_BUILD_NONE = 0,
  RI_ACCEL_BUILD_ALLOW_UPDATE = 0x1,
  RI_ACCEL_BUILD_ALLOW_COMPACTION = 0x2,
  RI_ACCEL_BUILD_ALLOW_DATA_ACCESS = 0x4,
  RI_ACCEL_BUILD_PREFER_FAST_TRACE = 0x8,
  RI_ACCEL_BUILD_PREFER_FAST_BUILD = 0x10,
  RI_ACCEL_BUILD_MINIMIZE_MEMORY = 0x20
};

enum RIAccelGeometryType_e {
  RI_ACCEL_GEOMETRY_TYPE_TRIANGLES,
  RI_ACCEL_GEOMETRY_TYPE_AABBS
};

enum RIAccelGeometryBits_e {
  RI_ACCEL_GEOMETRY_NONE = 0,
  RI_ACCEL_GEOMETRY_OPAQUE = 0x1,
  RI_ACCEL_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION = 0x2
};

enum RIAccelInstanceBits_e {
  RI_ACCEL_INSTANCE_NONE = 0,
  RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE = 0x1,
  RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING = 0x2,
  RI_ACCEL_INSTANCE_FORCE_OPAQUE = 0x4,
  RI_ACCEL_INSTANCE_FORCE_NON_OPAQUE = 0x8
};

// requires src and dst, both built with RI_ACCEL_BUILD_ALLOW_UPDATE
enum RIAccelBuildMode_e {
  RI_ACCEL_BUILD_MODE_BUILD,
  RI_ACCEL_BUILD_MODE_UPDATE
};

// matches VkAabbPositionsKHR layout
struct RIAccelAabb {
  RIAccelAabb() { memset(this, 0, sizeof(*this)); }
  float minX, minY, minZ;
  float maxX, maxY, maxZ;
};

// Backend-neutral TLAS instance description. The engine fills these; the HAL
// serializes each into the backend's instance-buffer layout via
// RI_WriteAccelInstance. Vulkan emits a VkAccelerationStructureInstanceKHR
// referencing the BLAS by device address; Metal emits a
// MTL::AccelerationStructureUserIDInstanceDescriptor referencing it by
// blasIndex into the TLAS's instancedAccelerationStructures array (Metal has no
// AS device address). instanceCustomIndex maps to VK instanceCustomIndex / MTL
// userID (the bindless object slot the shader reads back).
struct RIAccelInstanceDesc {
  RIAccelInstanceDesc() { memset(this, 0, sizeof(*this)); }
  float transform[3][4];         // row-major 3x4 affine (math layout)
  uint32_t instanceCustomIndex;  // VK instanceCustomIndex / MTL userID
  uint32_t mask;
  uint32_t flags;                // RIAccelInstanceBits_e
  struct RIAccelStructure *blas; // VK: device-address source
  uint32_t blasIndex;            // MTL: index into instancedAccelerationStructures
};

// Per-backend serialized stride of one instance, and the serializer (dst must
// have at least RI_AccelInstanceStride bytes). Defined in RIRenderer.cpp.
uint32_t RI_AccelInstanceStride(const struct RIRenderer *renderer);
void RI_WriteAccelInstance(const struct RIRenderer *renderer, void *dst,
                           const struct RIAccelInstanceDesc *src);

struct RIAccelTrianglesDesc {
  struct RIBuffer *vertexBuffer;
  uint64_t vertexOffset;
  uint32_t vertexNum;
  uint16_t vertexStride;
  enum RI_Format_e vertexFormat;

  struct RIBuffer *indexBuffer; // optional, NULL = unindexed
  uint64_t indexOffset;
  uint32_t indexNum;
  enum RIIndexType_e indexType;

  struct RIBuffer
      *transformBuffer; // optional, points to RIAccelTransform entries
  uint64_t transformOffset;
};

struct RIAccelAabbsDesc {
  struct RIBuffer *buffer; // points to RIAccelAabb entries
  uint64_t offset;
  uint32_t num;
  uint32_t stride;
};

struct RIAccelGeometryDesc {
  RIAccelGeometryDesc() { memset(this, 0, sizeof(*this)); }
  enum RIAccelGeometryType_e type;
  uint32_t flags; // RIAccelGeometryBits_e
  union {
    struct RIAccelTrianglesDesc triangles;
    struct RIAccelAabbsDesc aabbs;
  };
};

enum RIColorWriteMask_e {
  RI_COLOR_WRITE_NONE = 0,
  RI_COLOR_WRITE_R = 0x1,
  RI_COLOR_WRITE_G = 0x2,
  RI_COLOR_WRITE_B = 0x4,
  RI_COLOR_WRITE_A = 0x8,

  RI_COLOR_WRITE_RGB = RI_COLOR_WRITE_R | RI_COLOR_WRITE_G | RI_COLOR_WRITE_B,

  RI_COLOR_WRITE_RGBA =
      RI_COLOR_WRITE_R | RI_COLOR_WRITE_G | RI_COLOR_WRITE_B | RI_COLOR_WRITE_A
};

// S0 - source color 0
// S1 - source color 1
// D - destination color
// C - blend constants, set by "CmdSetBlendConstants"
enum RIBlendFactor_e {          // RGB                               ALPHA
  RI_BLEND_ZERO,                // 0                                 0
  RI_BLEND_ONE,                 // 1                                 1
  RI_BLEND_SRC_COLOR,           // S0.r, S0.g, S0.b                  S0.a
  RI_BLEND_ONE_MINUS_SRC_COLOR, // 1 - S0.r, 1 - S0.g, 1 - S0.b      1 - S0.a
  RI_BLEND_DST_COLOR,           // D.r, D.g, D.b                     D.a
  RI_BLEND_ONE_MINUS_DST_COLOR, // 1 - D.r, 1 - D.g, 1 - D.b         1 - D.a
  RI_BLEND_SRC_ALPHA,           // S0.a                              S0.a
  RI_BLEND_ONE_MINUS_SRC_ALPHA, // 1 - S0.a                          1 - S0.a
  RI_BLEND_DST_ALPHA,           // D.a                               D.a
  RI_BLEND_ONE_MINUS_DST_ALPHA, // 1 - D.a                           1 - D.a
  RI_BLEND_CONSTANT_COLOR,      // C.r, C.g, C.b                     C.a
  RI_BLEND_ONE_MINUS_CONSTANT_COLOR, // 1 - C.r, 1 - C.g, 1 - C.b         1 -
                                     // C.a
  RI_BLEND_CONSTANT_ALPHA,           // C.a                               C.a
  RI_BLEND_ONE_MINUS_CONSTANT_ALPHA, // 1 - C.a                           1 -
                                     // C.a
  RI_BLEND_SRC_ALPHA_SATURATE,       // min(S0.a, 1 - D.a)                1
  RI_BLEND_SRC1_COLOR,               // S1.r, S1.g, S1.b                  S1.a
  RI_BLEND_ONE_MINUS_SRC1_COLOR, // 1 - S1.r, 1 - S1.g, 1 - S1.b      1 - S1.a
  RI_BLEND_SRC1_ALPHA,           // S1.a                              S1.a
  RI_BLEND_ONE_MINUS_SRC1_ALPHA  // 1 - S1.a                          1 - S1.a
};

enum RIBlendOp_e {
  RI_BLEND_OP_ADD,
  RI_BLEND_OP_SUBTRACT,
  RI_BLEND_OP_REVERSE_SUBTRACT,
  RI_BLEND_OP_MIN,
  RI_BLEND_OP_MAX
};

enum RIWindowType_e {
  RI_WINDOW_UNKNOWN,
  RI_WINDOW_X11,
  RI_WINDOW_WIN32,
  RI_WINDOW_METAL,
  RI_WINDOW_WAYLAND
};

// Backend-neutral graphics-pipeline description consumed by the neutral
// RIProgram::bindPipeline overload. Captures exactly the state the engine's
// render passes set today (topology, raster, depth, per-attachment blend,
// dynamic-rendering color/depth formats, vertex layout) using RI enums so the
// same call site builds a VkGraphicsPipelineCreateInfo on Vulkan or an
// MTL::RenderPipelineDescriptor on Metal. Replaces passing Vk*CreateInfo
// through the public API.
struct RIVertexAttributeDesc {
  uint32_t location;
  uint32_t binding;
  uint32_t format; // RI_Format_e
  uint32_t offset;
};

struct RIVertexStreamDesc {
  uint32_t binding;
  uint32_t stride;
  bool perInstance;
};

struct RIColorAttachmentDesc {
  uint32_t format = 0; // RI_Format_e (RI_FORMAT_UNKNOWN => attachment unused)
  bool blendEnabled = false;
  uint8_t srcColor = RI_BLEND_ONE;     // RIBlendFactor_e
  uint8_t dstColor = RI_BLEND_ZERO;    // RIBlendFactor_e
  uint8_t colorBlendOp = RI_BLEND_OP_ADD; // RIBlendOp_e
  uint8_t srcAlpha = RI_BLEND_ONE;     // RIBlendFactor_e
  uint8_t dstAlpha = RI_BLEND_ZERO;    // RIBlendFactor_e
  uint8_t alphaBlendOp = RI_BLEND_OP_ADD; // RIBlendOp_e
  uint8_t writeMask = RI_COLOR_WRITE_RGBA; // RIColorWriteMask_e
};

enum RIStencilOp_e {
  RI_STENCIL_OP_KEEP,
  RI_STENCIL_OP_ZERO,
  RI_STENCIL_OP_REPLACE,
  RI_STENCIL_OP_INCREMENT_CLAMP,
  RI_STENCIL_OP_DECREMENT_CLAMP,
  RI_STENCIL_OP_INVERT,
  RI_STENCIL_OP_INCREMENT_WRAP,
  RI_STENCIL_OP_DECREMENT_WRAP
};

struct RIStencilOpDesc {
  uint8_t failOp = RI_STENCIL_OP_KEEP;      // RIStencilOp_e (stencil test fails)
  uint8_t passOp = RI_STENCIL_OP_KEEP;      // RIStencilOp_e (stencil+depth pass)
  uint8_t depthFailOp = RI_STENCIL_OP_KEEP; // RIStencilOp_e (stencil pass, depth fail)
  uint8_t compareOp = RI_COMPARE_ALWAYS;    // RICompareFunc_e
  uint8_t compareMask = 0xFF;               // read mask
  uint8_t writeMask = 0xFF;
};

struct RIGraphicsPipelineDesc {
  static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 8;
  static constexpr uint32_t MAX_VERTEX_STREAMS = 16;
  static constexpr uint32_t MAX_VERTEX_ATTRIBUTES = 16;

  uint8_t topology = RI_TOPOLOGY_TRIANGLE_LIST; // RITopology_e
  uint8_t cullMode = RI_CULL_MODE_NONE;         // RICullMode_e
  uint8_t fillMode = RI_FILL_SOLID;             // RIFillMode_e
  bool frontCounterClockwise = false;

  bool depthTestEnable = false;
  bool depthWriteEnable = false;
  uint8_t depthCompareOp = RI_COMPARE_LESS_EQUAL; // RICompareFunc_e
  uint32_t depthStencilFormat = 0;                // RI_Format_e (UNKNOWN => none)

  // Stencil. Reference is a single value applied to both faces (Vulkan bakes it
  // into VkStencilOpState; Metal sets it on the encoder via
  // setStencilReferenceValue). depthStencilFormat must carry a stencil aspect.
  bool stencilTestEnable = false;
  uint8_t stencilReference = 0;
  RIStencilOpDesc stencilFront;
  RIStencilOpDesc stencilBack;

  uint32_t sampleCount = 1;

  uint32_t colorCount = 0;
  RIColorAttachmentDesc colors[MAX_COLOR_ATTACHMENTS];

  uint32_t vertexStreamCount = 0;
  RIVertexStreamDesc vertexStreams[MAX_VERTEX_STREAMS];
  uint32_t vertexAttributeCount = 0;
  RIVertexAttributeDesc vertexAttributes[MAX_VERTEX_ATTRIBUTES];
};

// Metal places vertex-stream buffers at the TOP of the per-stage buffer table
// (indices count down from RI_MTL_MAX_BUFFER_INDEX) so they never collide with
// descriptor-set argument buffers or push constants, which occupy low indices
// (setVertexBuffer(argBuf, 0, setIndex) / setVertexBytes(...) — see RIProgram.cpp).
// Mirrors MoltenVK's mapping. The SAME mapping builds the pipeline's
// MTLVertexDescriptor (RIProgram.cpp) and binds streams (RICmd::bindVertexBuffers),
// so the two always agree.
static constexpr uint32_t RI_MTL_MAX_BUFFER_INDEX = 30; // Metal: 31 slots, 0..30
static inline uint32_t RI_MTL_VertexBufferIndex(uint32_t binding) {
  return RI_MTL_MAX_BUFFER_INDEX - binding;
}

struct RIComputePipelineDesc {
  // The shader's [numthreads(x,y,z)]. Metal requires the host to supply the
  // threadgroup size at dispatch (it is not baked into the MSL kernel the way it
  // is in SPIR-V), so the caller passes it explicitly here. Ignored on Vulkan.
  // 0 => fall back to 8x8x1 on Metal.
  uint16_t numThreads[3] = {0, 0, 0};
};

struct RIRayTracingPipelineDesc {
  // Max ray recursion depth (0 => treated as 1). The impl fills shader
  // stages/groups/layout from the program; on Vulkan it also builds the SBT.
  uint32_t maxRecursionDepth = 1;
  uint32_t flags = 0; // RIRayTracingPipelineFlagBits_e (reserved; 0 today)
};

// Backend-agnostic pipeline-object handle (union, like RIBuffer/RITexture).
// Owned + cached by RIProgram keyed by pipeline hash; built from an
// RIGraphicsPipelineDesc (render) or RIComputePipelineDesc (compute).
struct RIPipeline {
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkPipeline handle;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::RenderPipelineState *render;
      MTL::ComputePipelineState *compute;
      MTL::PrimitiveType primitiveType;
      // Metal carries depth/stencil test in a separate state object (not the
      // render pipeline). Built from the desc's depth+stencil fields, set on the
      // encoder at bind time alongside the stencil reference value.
      MTL::DepthStencilState *depthStencil;
      uint8_t stencilReference;
    } mtl;
#endif
  };
};

// Ray-tracing pipeline handle. Carries the SBT buffer + the four
// strided-device-address regions vkCmdTraceRaysKHR needs so traceRays()
// can dispatch without recomputing them. Vulkan-only for now.
struct RIRayTracingPipeline {
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkPipeline handle;
      VkBuffer sbtBuffer;
      VmaAllocation sbtAlloc;
      VkStridedDeviceAddressRegionKHR raygenRegion;
      VkStridedDeviceAddressRegionKHR missRegion;
      VkStridedDeviceAddressRegionKHR hitRegion;
      VkStridedDeviceAddressRegionKHR callableRegion;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    // Metal has no RT pipeline/SBT: an RT "pipeline" is a compute pipeline state
    // built from the raygen [[kernel]], with [[intersection]] any-hit functions
    // invoked via an intersection function table. traceRays() becomes a compute
    // dispatch (numThreads = the raygen [numthreads]).
    struct {
      MTL::ComputePipelineState *pipeline;
      MTL::IntersectionFunctionTable *intersectionTable;
      uint16_t numThreads[3];
    } mtl;
#endif
  };
};

struct RIBuffer {
  RIBuffer() { memset(this, 0, sizeof(*this)); }

  void dispose(struct RIDevice *device);
  // Backend-neutral buffer creation. Dispatches on the device API to the VK or
  // Metal path below; on success mappedAddress is set for RI_MEMORY_HOST_UPLOAD.
  static struct RIBuffer create(struct RIDevice *device,
                                const struct RIBufferDesc &desc);
#if (DEVICE_IMPL_VULKAN)
  static struct RIBuffer VK_createFromVMA(struct RIDevice *device,
                                            VkBufferCreateInfo *vk,
                                            VmaAllocationCreateInfo *info);
#endif
#if (DEVICE_IMPL_MTL)
  // Single-shot device-backed buffer (no separate memory object in this HAL).
  // hostVisible selects Shared (CPU-coherent) vs Private storage; on success
  // mappedAddress points at the contents for hostVisible buffers.
  static struct RIBuffer MTL_create(struct RIDevice *device, uint64_t size,
                                    bool hostVisible);
#endif
  void setDebugObjectName(struct RIDevice *device, const char *name);
  uint64_t GetDeviceHandle(struct RIDevice *device);
  // Make host writes to a HOST_UPLOAD mapping visible to the GPU. VK:
  // vmaFlushAllocation over [offset, size). Metal: no-op (Shared storage is
  // CPU/GPU-coherent). size == ~0ull flushes the whole allocation.
  void flush(struct RIDevice *device, uint64_t offset = 0, uint64_t size = ~0ull);
  // True when this buffer holds no backend handle (never created, or disposed).
  // Backend-dispatched on renderer->api (mirrors RITexture::isEmpty).
  bool isEmpty(const struct RIDevice *device) const;

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      struct VmaAllocation_T *allocation;
      VkBuffer buffer;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::Buffer *buffer;
    } mtl;
#endif
  };
  void *mappedAddress;
};

struct RITexture {
  RITexture() { memset(this, 0, sizeof(*this)); }
  // Backend-neutral image creation. Dispatches on the device API to the VK
  // (vmaCreateImage) or Metal (newTexture) path. Device-local storage.
  static struct RITexture create(struct RIDevice *device,
                                 const struct RITextureDesc &desc);
  void setDebugObjectName(struct RIDevice *device, const char *name);
  bool isEmpty(const struct RIDevice *device) const;
  void dispose(struct RIDevice *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkImage image;
      struct VmaAllocation_T *allocation;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::Texture *texture;
    } mtl;
#endif
  };
};

struct RITextureView {
  RITextureView() { memset(this, 0, sizeof(*this)); }
  // Backend-neutral view creation over `tex`. VK: vkCreateImageView; Metal:
  // newTextureView. The caller owns the returned view and disposes it.
  static struct RITextureView create(struct RIDevice *device,
                                     const struct RITexture *tex,
                                     const struct RITextureViewDesc &desc);
  // Destroys the image view and zeroes the struct.
  void dispose(struct RIDevice *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkImageView image;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      // Metal texture views are themselves MTL::Texture (newTextureView).
      MTL::Texture *view;
    } mtl;
#endif
  };
};

struct DescriptorBindingID {
  DescriptorBindingID() { memset(this, 0, sizeof(*this)); }
  const char *name;
  hash_t hash;
  static DescriptorBindingID Create(const char *name) {
    struct DescriptorBindingID key;
    key.name = name;
    key.hash = hash_data(HASH_INITIAL_VALUE, name, strlen(name));
    return key;
  }
};

static inline struct DescriptorBindingID
CreateDescriptorBindingID(const char *name) {
  struct DescriptorBindingID key;
  key.name = name;
  key.hash = hash_data(HASH_INITIAL_VALUE, name, strlen(name));
  return key;
}

struct RIAccelStructure;
struct RISampler;

// Backend sampler object. The only descriptor-referenced resource that owns a
// backend handle (created/cached by RIBootstrap::resolve_filter_descriptor).
struct RISampler {
  RISampler() { memset(this, 0, sizeof(*this)); }
  void dispose(struct RIDevice *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSampler sampler;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::SamplerState *sampler;
    } mtl;
#endif
  };
};

// Pure payload: references the RI objects to bind (buffer/texture/view/accel/
// sampler) plus binding params (offset/range/state) and the identity cookie. It
// owns nothing and stores no resolved backend handle — the bind paths pull the
// backend handle from the referenced RI objects via the accessors below.
struct RIDescriptor {
  RIDescriptor() { memset(this, 0, sizeof(*this)); }

  // Backend-neutral descriptor builders: reference the RI object + set the
  // binding params, and assign the caller-supplied `cookie` 1:1. `cookie` is the
  // descriptor-set cache key (RIProgram) — the caller MUST pass a stable,
  // collision-free id from an upper layer (iResourceBase::GetUniqueCookie() for a
  // resource, a content hash for a UBO, or hash_random() for a one-shot
  // engine-owned resource); never a reallocatable handle. cookie must be non-zero
  // (0 reads as empty via isEmpty()). `state` selects the VK image layout
  // (SHADER_READ_ONLY vs GENERAL); Metal ignores it. The `device` param is unused
  // (no resolution happens here) but kept for call-site stability.
  static RIDescriptor uniformBuffer(struct RIDevice *device,
                                    struct RIBuffer *buffer, uint64_t offset,
                                    uint64_t range, hash_t cookie);
  static RIDescriptor storageBuffer(struct RIDevice *device,
                                    struct RIBuffer *buffer, uint64_t offset,
                                    uint64_t range, hash_t cookie);
  static RIDescriptor sampledImage(struct RIDevice *device,
                                   struct RITextureView *view, hash_t cookie,
                                   enum RIResourceState_e state =
                                       RI_RESOURCE_STATE_SHADER_RESOURCE);
  static RIDescriptor storageImage(struct RIDevice *device,
                                   struct RITextureView *view, hash_t cookie);
  static RIDescriptor accelerationStructure(struct RIDevice *device,
                                            struct RIAccelStructure *as,
                                            hash_t cookie);
  static RIDescriptor sampler(struct RIDevice *device, struct RISampler *sampler,
                              hash_t cookie);

  bool isEmpty() const { return cookie == 0; }

  // Backend handle accessors — pull from the referenced RI objects at bind time.
  // Defined out-of-line in RIRenderer.cpp once all RI types are complete.
#if (DEVICE_IMPL_VULKAN)
  VkImageView vkImageView() const;
  VkBuffer vkBuffer() const;
  VkSampler vkSampler() const;
  VkAccelerationStructureKHR vkAccel() const;
  VkImageLayout vkLayout() const;
#endif
#if (DEVICE_IMPL_MTL)
  MTL::Resource *mtlResource() const;
  MTL::SamplerState *mtlSampler() const;
#endif

  // unique id / descriptor-set cache key (0 == empty)
  hash_t cookie;
  // Backend-neutral descriptor type (RIDescriptorType_e); mapped to the
  // VkDescriptorType / Metal binding kind at bind via ri_*_BindlessDescriptorType.
  uint8_t type;
  // Referenced RI objects — the descriptor owns none of them.
  struct RIBuffer *buffer;
  struct RITexture *texture;
  struct RITextureView *view;
  struct RIAccelStructure *accelStructure;
  struct RISampler *samplerRef;
  // Buffer binding range (Vulkan; Metal binds at offset 0 today).
  uint64_t offset;
  uint64_t range;
  // Image layout selector (Vulkan); Metal ignores.
  enum RIResourceState_e state;
};

struct RIAccelStructure {
  RIAccelStructure() { memset(this, 0, sizeof(*this)); }

  // Creates VkAccelerationStructureKHR backed by desc->storage at
  // desc->storageOffset. Caller must allocate desc->storage with
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, sized at least
  // desc->storageSize bytes.
  int init(struct RIDevice *device, const struct RIAccelStructureDesc *desc);
  void dispose(struct RIDevice *device);
  uint64_t getDeviceAddress(struct RIDevice *device) const;
  void setDebugObjectName(struct RIDevice *device, const char *name);

  // True when this acceleration structure holds no live backend handle.
  // Backend-dispatched on renderer->api (see RITexture::isEmpty).
  bool isEmpty(const struct RIRenderer *renderer) const;

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e snapshot
  //uint64_t buildScratchSize;
  //uint64_t updateScratchSize;
  //uint64_t storageOffset;
  //struct RIBuffer storage; // caller-owned backing buffer
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkAccelerationStructureKHR handle;
      VkDeviceAddress deviceAddress;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::AccelerationStructure *handle;
    } mtl;
#endif
  };
};

// Deferred-destroy handle for the per-frame freelist: a by-value copy of the
// owning RI struct, drained with std::visit -> dispose(device) once the frame
// slot's ring fence has signaled (see RIBootstrap::BeginActiveSet).
using RIFreeHandle = std::variant<struct RIBuffer, struct RITexture,
                                  struct RITextureView, struct RIAccelStructure>;

struct RIAccelStructureDesc {
  RIAccelStructureDesc() { memset(this, 0, sizeof(*this)); }

  // Query backing-storage and scratch sizes before RIAccelStructure::init;
  // any out-pointer may be NULL. For BLAS, geometries describes the geometry
  // layout used to compute sizes; the actual vertex/index buffer addresses
  // don't need to be valid until the build command.
  void getMemoryReqs(struct RIDevice *device, uint64_t *outStorageSize,
                     uint64_t *outBuildScratchSize,
                     uint64_t *outUpdateScratchSize) const;

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e
  uint32_t geometryOrInstanceNum; // BLAS: geometry count, TLAS: max instance count
  const struct RIAccelGeometryDesc *geometries; // BLAS only; NULL for TLAS
  struct RIBuffer *storage;
  uint64_t storageOffset;
  uint64_t storageSize; // from getMemoryReqs
};

struct RIBuildBlasDesc {
  RIBuildBlasDesc() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure *dst;
  struct RIAccelStructure *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  const struct RIAccelGeometryDesc *geometries;
  uint32_t geometryNum;
  struct RIBuffer *scratchBuffer;
  uint64_t scratchOffset;

};

struct RIBuildTlasDesc {
  RIBuildTlasDesc() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure *dst;
  struct RIAccelStructure *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  uint32_t instanceNum;
  struct RIBuffer *instanceBuffer; // RIAccelInstanceDesc-serialized entries
  uint64_t instanceOffset;
  struct RIBuffer *scratchBuffer;
  uint64_t scratchOffset;
  // Metal only: the BLASes that instances reference by blasIndex
  // (instancedAccelerationStructures). Vulkan ignores these (it references by
  // device address baked into the instance buffer).
  struct RIAccelStructure *const *instanceBlases;
  uint32_t instanceBlasNum;
};

struct RIRect {
  RIRect() { memset(this, 0, sizeof(*this)); }
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
};

struct RIViewport {
  RIViewport() { memset(this, 0, sizeof(*this)); }
  float x;
  float y;
  float width;
  float height;
  float depthMin;
  float depthMax;
  bool originBottomLeft; // expects "isViewportOriginBottomLeftSupported"
};

// Backend-neutral dynamic-rendering attachment ops, mirroring
// VkAttachmentLoadOp / VkAttachmentStoreOp and Metal's MTL::LoadAction /
// MTL::StoreAction. Used by RICmd::beginRendering so render code no longer
// reaches through to vkCmdBeginRendering directly.
enum RIAttachmentLoadOp_e {
  RI_ATTACHMENT_LOAD_OP_LOAD,
  RI_ATTACHMENT_LOAD_OP_CLEAR,
  RI_ATTACHMENT_LOAD_OP_DONT_CARE,
};

enum RIAttachmentStoreOp_e {
  RI_ATTACHMENT_STORE_OP_STORE,
  RI_ATTACHMENT_STORE_OP_DONT_CARE,
};

struct RIClearValue {
  float color[4];
  float depth;
  uint32_t stencil;
};

// One color or depth/stencil attachment for RICmd::beginRendering. `view`
// references the RITextureView abstraction (vk.image / mtl.view), so the same
// call site works on either backend.
struct RIRenderingAttachment {
  struct RITextureView *view;
  uint8_t loadOp;  // RIAttachmentLoadOp_e
  uint8_t storeOp; // RIAttachmentStoreOp_e
  // Depth attachment only: bind as DEPTH_READ_ONLY_OPTIMAL (depth-tested but not
  // written) instead of DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Ignored by Metal, which
  // expresses read-only depth through the pipeline's depth-stencil state.
  bool readOnly;
  // Depth/stencil attachment: when the view's format carries a stencil aspect,
  // set hasStencil to also bind it as a stencil attachment with its own
  // load/store (a pass may load depth but clear stencil). clearValue.stencil
  // supplies the clear. When hasStencil, the depth aspect binds as
  // DEPTH_ATTACHMENT_OPTIMAL and the stencil aspect as STENCIL_ATTACHMENT_OPTIMAL.
  bool hasStencil;
  uint8_t stencilLoadOp;  // RIAttachmentLoadOp_e
  uint8_t stencilStoreOp; // RIAttachmentStoreOp_e
  struct RIClearValue clearValue;
};

struct RIBeginRenderingDesc {
  struct RIRect renderArea;
  uint32_t colorCount;
  const struct RIRenderingAttachment *colors;
  const struct RIRenderingAttachment *depthStencil; // nullable
};

// One buffer->texture copy region for RICmd::copyBufferToTexture. Carries the
// staging layout in both representations so the backend dispatch stays free of
// format-props logic: Vulkan consumes bufferRowLength/bufferImageHeight (texels),
// Metal consumes bytesPerRow/bytesPerImage. The caller (the resource uploader)
// already has the format props to fill both.
struct RIBufferTextureCopyDesc {
  RIDeviceSize bufferOffset;
  uint32_t bufferRowLength;   // texels (Vulkan VkBufferImageCopy)
  uint32_t bufferImageHeight; // texels (Vulkan VkBufferImageCopy)
  uint32_t bytesPerRow;       // bytes  (Metal copyFromBuffer)
  uint32_t bytesPerImage;     // bytes  (Metal copyFromBuffer)
  uint32_t mipLevel;
  uint32_t arrayLayer;
  int32_t x, y, z;
  uint32_t width, height, depth;
};

// One image->image copy region for RICmd::copyImage. Source and destination
// extents are equal — Metal's blit encoder copies (copyFromTexture) and does
// not scale, so this is a 1:1 region copy with independent src/dst origins and
// subresources. The caller owns the surrounding layout barriers.
struct RIImageCopyDesc {
  uint32_t srcMipLevel;
  uint32_t srcArrayLayer;
  int32_t srcX, srcY, srcZ;
  uint32_t dstMipLevel;
  uint32_t dstArrayLayer;
  int32_t dstX, dstY, dstZ;
  uint32_t width, height, depth;
};

struct RIPool {
  RIPool() { memset(this, 0, sizeof(*this)); }
  // Creates the command pool on the queue's family.
  void init(struct RIDevice *device, struct RIQueue *queue);
  // Resets every command buffer allocated from the pool.
  void reset(struct RIDevice *device);
  void dispose(struct RIDevice *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueue queue;
      VkCommandPool pool;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    // Metal has no command-pool object; command buffers come from the queue.
    struct {
      MTL::CommandQueue *queue;
    } mtl;
#endif
  };
};

struct RICmd {
  RICmd() { memset(this, 0, sizeof(*this)); }

  // Allocates the command buffer from the pool.
  void init(struct RIDevice *device, struct RIPool *pool);
  // Begins/ends recording (one-time-submit).
  void begin(struct RIDevice *device);
  void end(struct RIDevice *device);
  // Returns the command buffer to its pool and clears the handles.
  void dispose(struct RIDevice *device);

  // [mtl] Explicit Metal command-encoder lifecycle. Metal permits a single open
  // encoder per command buffer, encoders are stateful (pipeline + descriptors
  // bind to the encoder), and barriers are no-ops on Metal — so the renderer
  // opens the encoder it needs and closes it at work-type transitions (where a
  // vk/d3d12 barrier sits) rather than relying on auto-switching. mtl_encoderEnd
  // ends + releases the active encoder; the open calls are idempotent and the
  // caller is responsible for ending a conflicting encoder first. All no-ops on
  // non-Metal builds.
  void mtl_encoderCompute(); // open a compute encoder
  void mtl_encoderBlit();    // open a blit (copy) encoder
  void mtl_encoderAccel();   // open an acceleration-structure encoder
  void mtl_encoderEnd();     // end + release the active encoder

  // [mtl] Make a list of acceleration structures resident (Read) on the active
  // compute encoder. The Metal RT pass traverses the TLAS through an argument
  // buffer, which requires its referenced BLASes to be resident. No-op on other
  // backends or when no compute encoder is open.
  void mtl_useAccelStructuresResident(struct RIAccelStructure *const *list,
                                      uint32_t count);

  // Leaf dispatch/draw command methods. Pipeline binding is done separately
  // (RIProgram::bindPipeline / bindComputePipeline / bindRayTracingPipeline);
  // these are the "go" calls that issue the actual work. Core Vulkan 1.0 —
  // no RIDevice needed because there's no fn-pointer indirection.
  void dispatch(struct RIRenderer *renderer, uint32_t groupCountX,
                uint32_t groupCountY, uint32_t groupCountZ);
  void dispatchIndirect(struct RIRenderer *renderer, struct RIBuffer *buffer,
                        RIDeviceSize offset);
  void draw(struct RIRenderer *renderer, uint32_t vertexCount,
            uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance);
  void drawIndexed(struct RIRenderer *renderer, uint32_t indexCount,
                   uint32_t instanceCount, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t firstInstance);
  void drawIndirect(struct RIRenderer *renderer, struct RIBuffer *buffer,
                    RIDeviceSize offset, uint32_t drawCount, uint32_t stride);
  void drawIndexedIndirect(struct RIRenderer *renderer, struct RIBuffer *buffer,
                           RIDeviceSize offset, uint32_t drawCount,
                           uint32_t stride);

  // [vk/mtl] Buffer-to-buffer copy. Vulkan records vkCmdCopyBuffer; Metal opens
  // a blit encoder (mtl_encoderBlit) and calls copyFromBuffer. On Metal the
  // caller must have closed any conflicting render/compute encoder first via
  // mtl_encoderEnd() (barriers are no-ops on Metal).
  void copyBuffer(struct RIRenderer *renderer, struct RIBuffer *src,
                  RIDeviceSize srcOffset, struct RIBuffer *dst,
                  RIDeviceSize dstOffset, RIDeviceSize size);

  // [vk/mtl] Buffer-to-texture copy of a single subresource region. Vulkan records
  // vkCmdCopyBufferToImage (dst assumed in TRANSFER_DST layout); Metal opens a blit
  // encoder and calls copyFromBuffer:toTexture:. The desc carries the staging layout
  // in both texel (Vulkan) and byte (Metal) form so this stays free of format props.
  void copyBufferToTexture(struct RIRenderer *renderer, struct RIBuffer *src,
                           struct RITexture *dst,
                           const struct RIBufferTextureCopyDesc &desc);

  // [vk/mtl] Image-to-image copy of a single 1:1 region (no scaling). Vulkan
  // records vkCmdCopyImage (src in TRANSFER_SRC, dst in TRANSFER_DST layout);
  // Metal opens a blit encoder (mtl_encoderBlit) and calls
  // copyFromTexture:toTexture:. The caller owns the surrounding barriers
  // (no-ops on Metal) and on Metal must have closed any conflicting
  // render/compute encoder first via mtl_encoderEnd().
  void copyImage(struct RIRenderer *renderer, struct RITexture *src,
                 struct RITexture *dst, const struct RIImageCopyDesc &desc);

  // [vk/mtl] Clear a storage image to a color value. Assumes the full color
  // subresource (mip 0, layer 0) in GENERAL layout (RI_RESOURCE_STATE_CLEAR_STORAGE);
  // caller owns the surrounding barriers. Vulkan records vkCmdClearColorImage.
  void clearStorageImage(struct RIRenderer *renderer, struct RITexture *image,
                         const float color[4]);

  // [vk/d3d12] Dynamic-rendering scope (vkCmdBeginRendering/EndRendering;
  // D3D12 shares this model). Metal is deliberately NOT handled here — its
  // stateful-encoder model differs, so Metal uses mtl_encoderDraw /
  // mtl_encoderEnd instead (kept as separate APIs to avoid conflation).
  void vk_d3d12_beginRendering(struct RIRenderer *renderer,
                               const struct RIBeginRenderingDesc &desc);
  void vk_d3d12_endRendering(struct RIRenderer *renderer);

  // [mtl] Open a render encoder for a batch of draw calls from the attachment
  // descriptors (Metal's analogue of vk_d3d12_beginRendering). End it with
  // mtl_encoderEnd(). No-op on non-Metal builds.
  void mtl_encoderDraw(const struct RIBeginRenderingDesc &desc);

  void setViewport(struct RIRenderer *renderer,
                   const struct RIViewport &viewport);
  void setScissor(struct RIRenderer *renderer, const struct RIRect &scissor);

  // True when this command buffer holds a live backend command buffer to record
  // into — the backend-neutral form of the old `cmds[0].vk.cmd != VK_NULL_HANDLE`
  // test. Dispatches on renderer->is_target_selected; defined out-of-line in
  // RIRenderer.cpp (where RIRenderer is a complete type). Used by consumers that
  // opportunistically ride the active frame's primary cmd buffer (screen capture,
  // GUI-to-screen).
  bool isOpen(struct RIRenderer *renderer) const;

  // Acceleration-structure build commands; numDescs structures are submitted
  // in a single backend call. Caller-supplied scratchBuffer must include
  // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, with scratchOffset aligned to
  // minAccelerationStructureScratchOffsetAlignment. Input vertex/index/
  // instance buffers must include
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
  void buildBlas(struct RIDevice *device,
                 const struct RIBuildBlasDesc *descs, uint32_t numDescs);
  void buildTlas(struct RIDevice *device,
                 const struct RIBuildTlasDesc *descs, uint32_t numDescs);

  // Emit pipeline barriers from RI resource-state transitions (see
  // RIBarrier.h). All groups are batched into a single backend barrier
  // command (vkCmdPipelineBarrier2); any count may be zero. The template
  // parameters MemN/BufN/TexN are the stack capacities reserved for the
  // backend barrier scratch arrays (compile-time sized); a capacity of 0
  // moves that group to the heap instead, for dynamically sized batches.
  // vk_d3d12_-prefixed: a no-op on Metal, which tracks hazards automatically
  // (the renderer instead ends/reopens encoders at these transition points).
  template <uint32_t MemN, uint32_t BufN, uint32_t TexN>
  void vk_d3d12_resourceBarrier(uint32_t memoryBarrierNum,
                       const struct RIMemoryBarrier *memoryBarriers,
                       uint32_t bufferBarrierNum,
                       const struct RIBufferBarrier *bufferBarriers,
                       uint32_t textureBarrierNum,
                       const struct RITextureBarrier *textureBarriers) {
    if (memoryBarrierNum + bufferBarrierNum + textureBarrierNum == 0)
      return;

#if (DEVICE_IMPL_VULKAN)
    ScratchBuffer<VkMemoryBarrier2, MemN> memScratch;
    ScratchBuffer<VkBufferMemoryBarrier2, BufN> bufScratch;
    ScratchBuffer<VkImageMemoryBarrier2, TexN> imgScratch;
    VkMemoryBarrier2 *mem = memScratch.get(memoryBarrierNum);
    VkBufferMemoryBarrier2 *buf = bufScratch.get(bufferBarrierNum);
    VkImageMemoryBarrier2 *img = imgScratch.get(textureBarrierNum);

    for (uint32_t i = 0; i < memoryBarrierNum; i++) {
      const struct RIMemoryBarrier &src = memoryBarriers[i];
      VkMemoryBarrier2 &dst = mem[i];
      dst = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
    }

    for (uint32_t i = 0; i < bufferBarrierNum; i++) {
      const struct RIBufferBarrier &src = bufferBarriers[i];
      VkBufferMemoryBarrier2 &dst = buf[i];
      dst = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
      dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.buffer = src.buffer->vk.buffer;
      dst.offset = src.offset;
      dst.size = src.size ? src.size : VK_WHOLE_SIZE;
    }

    for (uint32_t i = 0; i < textureBarrierNum; i++) {
      const struct RITextureBarrier &src = textureBarriers[i];
      VkImageMemoryBarrier2 &dst = img[i];
      dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
      dst.oldLayout = ri_vk_RIResourceStateToImageLayout(src.before);
      dst.newLayout = ri_vk_RIResourceStateToImageLayout(src.after);
      dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.image = src.texture->vk.image;
      dst.subresourceRange = VkImageSubresourceRange{
          ri_vk_RIBarrierAspectToVK(src.aspect),
          src.baseMip,
          src.mipCount ? src.mipCount : VK_REMAINING_MIP_LEVELS,
          src.baseLayer,
          src.layerCount ? src.layerCount : VK_REMAINING_ARRAY_LAYERS,
      };
    }

    VkDependencyInfo dependencyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.memoryBarrierCount = memoryBarrierNum;
    dependencyInfo.pMemoryBarriers = mem;
    dependencyInfo.bufferMemoryBarrierCount = bufferBarrierNum;
    dependencyInfo.pBufferMemoryBarriers = buf;
    dependencyInfo.imageMemoryBarrierCount = textureBarrierNum;
    dependencyInfo.pImageMemoryBarriers = img;
    vkCmdPipelineBarrier2(vk.cmd, &dependencyInfo);
#endif
  }

  // Single-barrier conveniences (vk_d3d12_-prefixed: no-op on Metal).
  void vk_d3d12_memoryBarrier(const struct RIMemoryBarrier &barrier) {
    vk_d3d12_resourceBarrier<1, 0, 0>(1, &barrier, 0, NULL, 0, NULL);
  }
  void vk_d3d12_bufferBarrier(const struct RIBufferBarrier &barrier) {
    vk_d3d12_resourceBarrier<0, 1, 0>(0, NULL, 1, &barrier, 0, NULL);
  }
  void vk_d3d12_textureBarrier(const struct RITextureBarrier &barrier) {
    vk_d3d12_resourceBarrier<0, 0, 1>(0, NULL, 0, NULL, 1, &barrier);
  }
  // Fixed-capacity texture-batch convenience; N is the stack capacity.
  template <uint32_t N>
  void vk_d3d12_textureBarriers(uint32_t num,
                                const struct RITextureBarrier *barriers) {
    vk_d3d12_resourceBarrier<0, 0, N>(0, NULL, 0, NULL, num, barriers);
  }

  // Bind a single index buffer. Takes an RIBuffer* (the RI abstraction)
  // rather than a backend handle so the same call site survives a future
  // DX12 backend.
  void bindIndexBuffer(struct RIRenderer *renderer, struct RIBuffer *buffer,
                       RIDeviceSize offset, enum RIIndexType_e indexType);

  // Bind `count` vertex buffers. The template parameter N is only the stack
  // capacity reserved for the backend handle scratch array (compile-time
  // sized, no heap); `count` is the actual number bound and must be <= N.
  // `buffers` is a raw RIBuffer* array of length `count` (a null entry
  // binds nothing); `offsets` is a parallel byte-offset array. e.g. for a
  // fixed 5-stream layout where all 5 are live:
  // cmd->bindVertexBuffers<5>(0, 5, bufs). RIBuffer* keeps the call site
  // backend-agnostic for the planned DX12 path.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers,
                         const RIDeviceSize *offsets) {
    assert(count <= N);
#if (DEVICE_IMPL_VULKAN)
    VkBuffer vkBufs[N];
    for (uint32_t i = 0; i < count; ++i)
      vkBufs[i] = buffers[i] ? buffers[i]->vk.buffer : VK_NULL_HANDLE;
    vkCmdBindVertexBuffers(vk.cmd, firstBinding, count, vkBufs, offsets);
#endif
#if (DEVICE_IMPL_MTL)
    // Streams bind at the top of the buffer table (RI_MTL_VertexBufferIndex) —
    // the same mapping the pipeline's MTLVertexDescriptor uses. A null entry
    // binds nothing.
    assert(mtl.render && "bindVertexBuffers requires an open render encoder");
    for (uint32_t i = 0; i < count; ++i)
      if (buffers[i])
        mtl.render->setVertexBuffer(buffers[i]->mtl.buffer,
                                    (NS::UInteger)offsets[i],
                                    RI_MTL_VertexBufferIndex(firstBinding + i));
#endif
  }

  // Convenience overload binding `count` streams at offset 0.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers) {
    const RIDeviceSize offsets[N] = {};
    bindVertexBuffers<N>(firstBinding, count, buffers, offsets);
  }

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkCommandPool pool;
      VkCommandBuffer cmd;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::CommandBuffer *cmd;
      MTL::RenderCommandEncoder *render;
      MTL::ComputeCommandEncoder *compute;
      MTL::BlitCommandEncoder *blit;
      MTL::AccelerationStructureCommandEncoder *accel;
      // Primitive topology of the currently bound graphics pipeline; Metal
      // draws need it explicitly (set by RIProgram::bindPipeline). Defaults to
      // triangles.
      MTL::PrimitiveType primitiveType;
      // Threadgroup size ([numthreads]) of the currently bound compute pipeline;
      // Metal supplies it at dispatch (set by RIProgram::bindComputePipeline
      // from RIComputePipelineDesc::numThreads). 0 => fall back to 8x8x1.
      uint16_t threadsPerThreadgroup[3];
      // Index-buffer state stashed by bindIndexBuffer. Metal has no separate
      // index-bind call; drawIndexedPrimitives takes the buffer/offset/type
      // directly, so we carry the last-bound binding here.
      MTL::Buffer *indexBuffer;
      uint64_t indexBufferOffset;
      MTL::IndexType indexType;
    } mtl;
#endif
  };
};

// Compile-time maximums; runtime poolCount/cmdPerPool are passed to
// RICommandRingBuffer::init.
#define RI_COMMAND_RING_POOL_COUNT 8
#define RI_COMMAND_RING_CMD_PER_POOL 32

struct RICommandRingElement {
  RICommandRingElement() { memset(this, 0, sizeof(*this)); }
  // Blocks until the element's fence signals (no-op without sync primitives).
  void wait(struct RIDevice *device);
  // Submits the element's command buffers to `queue` with no wait/signal
  // semaphores, signalling the element's fence (VK) / event slot (MTL). For
  // one-shot init work — pair with queue.waitIdle() or this->wait().
  void submit(struct RIDevice *device, struct RIQueue *queue);
  struct RICmd *cmds;
  uint32_t numCmds;
  struct RIPool *pool;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSemaphore semaphore;
      VkFence fence;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    // vk semaphore+fence → a Metal timeline event signalled to signalValue.
    // signalValue is the value to host-wait (the slot's last-signalled value, set
    // at acquire); signalValueSlot points back into the ring's values[] so the
    // submit can increment+persist the next signalled value (see acquire()).
    struct {
      MTL::SharedEvent *event;
      uint64_t signalValue;
      uint64_t *signalValueSlot;
    } mtl;
#endif
  };
};

template <uint32_t MaxPoolCount = RI_COMMAND_RING_POOL_COUNT,
          uint32_t CmdPerPool = RI_COMMAND_RING_CMD_PER_POOL>
struct RICommandRingBuffer {
  RICommandRingBuffer() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_POOL_COUNT = MaxPoolCount;
  static constexpr uint32_t CMD_PER_POOL = CmdPerPool;

  // Defined at the bottom of this header once RIDevice is complete
  // (same precedent as RISwapchain::dispose).
  void init(struct RIDevice *device, struct RIQueue *queue,
            uint32_t poolCount, uint32_t cmdPerPool, bool syncPrimitives);
  void dispose(struct RIDevice *device);
  // Rotates to the next pool and rewinds the cmd/fence cursors.
  void advance();
  // Claims numCmds command buffers plus a fence slot from the current pool
  // and advances the cursors.
  struct RICommandRingElement acquire(struct RIDevice *device,
                                        uint32_t numCmds);

  uint32_t poolIndex;
  uint32_t cmdIndex;
  uint32_t fenceIndex;

  uint32_t poolCount;
  uint32_t cmdPerPool;
  bool syncPrimitive;

  struct RIPool pools[MaxPoolCount];
  struct RICmd cmds[MaxPoolCount][CmdPerPool];

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkFence fences[MaxPoolCount][CmdPerPool];
      VkSemaphore semaphores[MaxPoolCount][CmdPerPool];
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::SharedEvent *events[MaxPoolCount][CmdPerPool];
      uint64_t values[MaxPoolCount][CmdPerPool];
    } mtl;
#endif
  };
};

struct RIQueue {
  RIQueue() { memset(this, 0, sizeof(*this)); }
  void waitIdle(struct RIDevice *device);
  // Defined out-of-line in RIRenderer.cpp: RIRenderer is an incomplete type here
  // (defined later in this header) so the backend is dispatched at runtime on
  // renderer->api once the type is complete.
  uint32_t getFlags(const struct RIRenderer *renderer) const;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueueFlags queueFlags;
      uint16_t queueFamilyIdx;
      uint16_t slotIdx;
      VkQueue queue;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    // Metal queues are general-purpose; flags holds the RI_QUEUE_* bits.
    struct {
      MTL::CommandQueue *queue;
      uint32_t flags;
    } mtl;
#endif
  };
};

template <uint32_t MaxImageCount = RI_MAX_SWAPCHAIN_IMAGES>
struct RISwapchain {
  RISwapchain() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_IMAGE_COUNT = MaxImageCount;

  // Destroys the per-image semaphores, the swapchain and the surface (the
  // swapchain images themselves are owned by the swapchain). Defined at the
  // bottom of this header once RIDevice/RIRenderer are complete.
  void dispose(struct RIDevice *device);

  struct RIQueue *presentQueue;
  uint16_t imageCount;
  uint16_t width;
  uint16_t height;
  uint32_t format; // RI_Format_e
  struct RITexture textures[MaxImageCount];
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t frameIndex;
      uint32_t textureIndex;
      uint64_t presentID;
      VkSwapchainKHR swapchain;
      VkSurfaceKHR surface;
      VkImage images[MaxImageCount];
      VkSemaphore imageAcquireSem[MaxImageCount];
      VkSemaphore finishSem[MaxImageCount];
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      CA::MetalLayer *layer;
      CA::MetalDrawable *drawable;
      uint32_t textureIndex;
    } mtl;
#endif
  };
};

struct RIRenderer {
  RIRenderer() { memset(this, 0, sizeof(*this)); }
  // Loads the loader (volk), creates the instance and optional debug
  // messenger. Returns RI_SUCCESS/RI_FAIL.
  int init(const struct RIBackendInit *init);
  int enumerateAdapters(struct RIPhysicalAdapter *adapters,
                        uint32_t *numAdapters);
  // Destroys the debug messenger and the instance. Instance-level — takes no
  // device (the device is normally gone by the time this runs); loader
  // teardown (volkFinalize) stays with ShutdownRIRenderer.
  void dispose();
  uint8_t api; // RIDeviceAPI_e

  inline bool is_target_selected(uint8_t targetApi) const {
    if(DEVICE_MULTI_BACKEND) {
      if(DEVICE_IMPL_VULKAN && targetApi == RI_DEVICE_API_VK) return true;
      if(DEVICE_IMPL_MTL && targetApi == RI_DEVICE_API_MTL) return true;
      if(DEVICE_IMPL_D3D12 && targetApi == RI_DEVICE_API_D3D12) return true; 
    }
    assert(targetApi == api); // In single-backend builds, the target API must match the renderer's API.
    return true;
  }
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkInstance instance;
      VkDebugUtilsMessengerEXT debugMessageUtils;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    // Metal has no instance object; adapters enumerate via MTL::CopyAllDevices.
    struct {
      uint32_t reserved;
    } mtl;
#endif
  };

};



struct RIBackendInit {
  uint8_t api; // RIDeviceAPI_e
  const char *applicationName;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t enableValidationLayer : 1;
      size_t numFilterLayers;
      // Was `const char* filterLayers[];` — a C99 flexible-array member.
      // MSVC rejects FAMs inside an anonymous union when the enclosing
      // struct is stack-allocated (error C2466 in Graphics.cpp::Init).
      // No call site currently writes to this field; if filter-layer
      // support is added later, allocate the array externally and point
      // here.
      const char *const *filterLayers;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      uint32_t enableValidation : 1;
    } mtl;
#endif
  };
};

struct RIPhysicalAdapter {
  RIPhysicalAdapter() { memset(this, 0, sizeof(*this)); }
  char name[256];
  uint64_t luid;
  uint64_t videoMemorySize;
  uint64_t systemMemorySize;
  uint32_t deviceId;
  uint8_t vendor;      // RIVendor_e
  uint8_t presetLevel; // RIPresetLevel_e
  uint8_t type;        // RIAdapterType_e

  // Viewports
  uint32_t viewportMaxNum;
  int32_t viewportBoundsRange[2];

  // Attachments
  uint16_t attachmentMaxDim;
  uint16_t attachmentLayerMaxNum;
  uint16_t colorAttachmentMaxNum;

  // Multi-sampling
  uint8_t colorSampleMaxNum;
  uint8_t depthSampleMaxNum;
  uint8_t stencilSampleMaxNum;
  uint8_t zeroAttachmentsSampleMaxNum;
  uint8_t textureColorSampleMaxNum;
  uint8_t textureIntegerSampleMaxNum;
  uint8_t textureDepthSampleMaxNum;
  uint8_t textureStencilSampleMaxNum;
  uint8_t storageTextureSampleMaxNum;

  // Resource dimensions
  uint16_t texture1DMaxDim;
  uint16_t texture2DMaxDim;
  uint16_t texture3DMaxDim;
  uint16_t textureArrayLayerMaxNum;
  uint32_t typedBufferMaxDim;

  // Memory
  uint64_t deviceUploadHeapSize; // ReBAR
  uint32_t memoryAllocationMaxNum;
  uint32_t samplerAllocationMaxNum;
  uint32_t constantBufferMaxRange;
  uint32_t storageBufferMaxRange;
  uint32_t bufferTextureGranularity;
  uint64_t bufferMaxSize;

  // Memory alignment
  uint32_t uploadBufferTextureRowAlignment;
  uint32_t uploadBufferOffsetAlignment;
  uint32_t bufferShaderResourceOffsetAlignment;
  uint32_t constantBufferOffsetAlignment;
  // uint32_t scratchBufferOffsetAlignment;
  // uint32_t shaderBindingTableAlignment;

  // Pipeline layout
  // D3D12 only: rootConstantSize + descriptorSetNum * 4 + rootDescriptorNum * 8
  // <= 256 (see "FitPipelineLayoutSettingsIntoDeviceLimits")
  uint32_t pipelineLayoutDescriptorSetMaxNum;
  uint32_t pipelineLayoutRootConstantMaxSize;
  uint32_t pipelineLayoutRootDescriptorMaxNum;

  // Descriptor set
  uint32_t descriptorSetSamplerMaxNum;
  uint32_t descriptorSetConstantBufferMaxNum;
  uint32_t descriptorSetStorageBufferMaxNum;
  uint32_t descriptorSetTextureMaxNum;
  uint32_t descriptorSetStorageTextureMaxNum;

  // Shader resources
  uint32_t perStageDescriptorSamplerMaxNum;
  uint32_t perStageDescriptorConstantBufferMaxNum;
  uint32_t perStageDescriptorStorageBufferMaxNum;
  uint32_t perStageDescriptorTextureMaxNum;
  uint32_t perStageDescriptorStorageTextureMaxNum;
  uint32_t perStageResourceMaxNum;

  // Vertex shader
  uint32_t vertexShaderAttributeMaxNum;
  uint32_t vertexShaderStreamMaxNum;
  uint32_t vertexShaderOutputComponentMaxNum;

  // Tessellation shaders
  float tessControlShaderGenerationMaxLevel;
  uint32_t tessControlShaderPatchPointMaxNum;
  uint32_t tessControlShaderPerVertexInputComponentMaxNum;
  uint32_t tessControlShaderPerVertexOutputComponentMaxNum;
  uint32_t tessControlShaderPerPatchOutputComponentMaxNum;
  uint32_t tessControlShaderTotalOutputComponentMaxNum;
  uint32_t tessEvaluationShaderInputComponentMaxNum;
  uint32_t tessEvaluationShaderOutputComponentMaxNum;

  // Geometry shader
  uint32_t geometryShaderInvocationMaxNum;
  uint32_t geometryShaderInputComponentMaxNum;
  uint32_t geometryShaderOutputComponentMaxNum;
  uint32_t geometryShaderOutputVertexMaxNum;
  uint32_t geometryShaderTotalOutputComponentMaxNum;

  // Fragment shader
  uint32_t fragmentShaderInputComponentMaxNum;
  uint32_t fragmentShaderOutputAttachmentMaxNum;
  uint32_t fragmentShaderDualSourceAttachmentMaxNum;

  // Compute shader
  uint32_t computeShaderSharedMemoryMaxSize;
  uint32_t computeShaderWorkGroupMaxNum[3];
  uint32_t computeShaderWorkGroupInvocationMaxNum;
  uint32_t computeShaderWorkGroupMaxDim[3];

  // Ray tracing
  uint32_t rayTracingShaderGroupIdentifierSize;
  uint32_t rayTracingShaderTableMaxStride;
  uint32_t rayTracingShaderRecursionMaxDepth;
  uint32_t rayTracingGeometryObjectMaxNum;
  uint32_t accelerationStructureScratchOffsetAlignment;

  // Mesh shaders
  // uint32_t meshControlSharedMemoryMaxSize;
  // uint32_t meshControlWorkGroupInvocationMaxNum;
  // uint32_t meshControlPayloadMaxSize;
  // uint32_t meshEvaluationOutputVerticesMaxNum;
  // uint32_t meshEvaluationOutputPrimitiveMaxNum;
  // uint32_t meshEvaluationOutputComponentMaxNum;
  // uint32_t meshEvaluationSharedMemoryMaxSize;
  // uint32_t meshEvaluationWorkGroupInvocationMaxNum;

  // Precision bits
  uint32_t viewportPrecisionBits;
  uint32_t subPixelPrecisionBits;
  uint32_t subTexelPrecisionBits;
  uint32_t mipmapPrecisionBits;

  // Other
  uint64_t timestampFrequencyHz;
  uint32_t drawIndirectMaxNum;
  float samplerLodBiasMin;
  float samplerLodBiasMax;
  float samplerAnisotropyMax;
  int32_t texelOffsetMin;
  uint32_t texelOffsetMax;
  int32_t texelGatherOffsetMin;
  uint32_t texelGatherOffsetMax;
  uint32_t clipDistanceMaxNum;
  uint32_t cullDistanceMaxNum;
  uint32_t combinedClipAndCullDistanceMaxNum;
  // uint8_t shadingRateAttachmentTileSize;
  // uint8_t shaderModel; // major * 10 + minor

  // Tiers (0 - unsupported)
  // 1 - 1/2 pixel uncertainty region and does not support post-snap degenerates
  // 2 - reduces the maximum uncertainty region to 1/256 and requires post-snap
  // degenerates not be culled 3 - maintains a maximum 1/256 uncertainty region
  // and adds support for inner input coverage, aka "SV_InnerCoverage"
  // uint8_t conservativeRasterTier;

  // 1 - a single sample pattern can be specified to repeat for every pixel
  // ("locationNum / sampleNum" must be 1 in "CmdSetSampleLocations") 2 - four
  // separate sample patterns can be specified for each pixel in a 2x2 grid
  // ("locationNum / sampleNum" can be up to 4 in "CmdSetSampleLocations")
  // uint8_t sampleLocationsTier;

  // 1 - DXR 1.0: full raytracing functionality, except features below
  // 2 - DXR 1.1: adds - ray query, "CmdDispatchRaysIndirect", "GeometryIndex()"
  // intrinsic, additional ray flags & vertex formats
  uint8_t rayTracingTier;

  // 1 - shading rate can be specified only per draw
  // 2 - adds: per primitive shading rate, per "shadingRateAttachmentTileSize"
  // shading rate, combiners, "SV_ShadingRate" support
  // uint8_t shadingRateTier;

  // 1 - unbound arrays with dynamic indexing
  // 2 - D3D12 dynamic resources:
  // https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
  uint8_t bindlessTier;

  // Features
  uint32_t isTextureFilterMinMaxSupported : 1;
  uint32_t isLogicFuncSupported : 1;
  uint32_t isDepthBoundsTestSupported : 1;
  uint32_t isDrawIndirectCountSupported : 1;
  uint32_t isIndependentFrontAndBackStencilReferenceAndMasksSupported : 1;
  // uint32_t isLineSmoothingSupported : 1;
  uint32_t isCopyQueueTimestampSupported : 1;
  // uint32_t isMeshShaderPipelineStatsSupported : 1;
  uint32_t isEnchancedBarrierSupported : 1; // aka - can "Layout" be ignored?
  uint32_t isMemoryTier2Supported
      : 1; // a memory object can support resources from all 3 categories
           // (buffers, attachments, all other textures)
  uint32_t isDynamicDepthBiasSupported : 1;
  // uint32_t isAdditionalShadingRatesSupported : 1;
  uint32_t isViewportOriginBottomLeftSupported : 1;
  uint32_t isRegionResolveSupported : 1;

  // Shader features
  uint32_t isShaderNativeI16Supported : 1;
  uint32_t isShaderNativeF16Supported : 1;
  uint32_t isShaderNativeI32Supported : 1;
  uint32_t isShaderNativeF32Supported : 1;
  uint32_t isShaderNativeI64Supported : 1;
  uint32_t isShaderNativeF64Supported : 1;
  uint32_t isShaderAtomicsI16Supported : 1;
  // uint32_t isShaderAtomicsF16Supported : 1;
  uint32_t isShaderAtomicsI32Supported : 1;
  // uint32_t isShaderAtomicsF32Supported : 1;
  uint32_t isShaderAtomicsI64Supported : 1;
  // uint32_t isShaderAtomicsF64Supported : 1;

  // Emulated features
  uint32_t isDrawParametersEmulationEnabled : 1;

  //// Extensions (unexposed are always supported)
  // uint32_t isSwapChainSupported : 1;	// swapchain Support
  uint32_t isRayQuerySupported
      : 1; // VK_KHR_ray_query / DXR 1.1 inline ray queries
  // uint32_t isMeshShaderSupported : 1; // meshshader support

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkPhysicalDevice physicalDevice;

      uint32_t isSwapChainSupported : 1; // swapchain Support
      uint32_t isBufferDeviceAddressSupported : 1;
      uint32_t isAMDDeviceCoherentMemorySupported : 1;
      uint32_t isPresentIDSupported : 1;
      // uint32_t YCbCrExtension : 1;
      // uint32_t FillModeNonSolid : 1;
      // uint32_t KHRRayQueryExtension : 1;
      // uint32_t AMDGCNShaderExtension : 1;
      // uint32_t AMDDrawIndirectCountExtension : 1;
      // uint32_t AMDShaderInfoExtension : 1;
      // uint32_t DescriptorIndexingExtension : 1;
      // uint32_t DynamicRenderingExtension : 1;
      // uint32_t ShaderSampledImageArrayDynamicIndexingSupported : 1;
      // uint32_t BufferDeviceAddressSupported : 1;
      // uint32_t DrawIndirectCountExtension : 1;
      // uint32_t DedicatedAllocationExtension : 1;
      // uint32_t DebugMarkerExtension : 1;
      // uint32_t MemoryReq2Extension : 1;
      // uint32_t FragmentShaderInterlockExtension : 1;
      // uint32_t BufferDeviceAddressExtension : 1;
      uint32_t accelerationStructureExtension : 1;
      uint32_t rayTracingPipelineExtension : 1;
      uint32_t rayQueryExtension : 1;
      // uint32_t ShaderAtomicInt64Extension : 1;
      // uint32_t BufferDeviceAddressFeature : 1;
      // uint32_t ShaderFloatControlsExtension : 1;
      // uint32_t Spirv14Extension : 1;
      uint32_t deferredHostOperationsExtension : 1;
      // uint32_t DeviceFaultExtension : 1;
      // uint32_t DeviceFaultSupported : 1;
      // uint32_t ASTCDecodeModeExtension : 1;
      // uint32_t DeviceMemoryReportExtension : 1;
      // uint32_t AMDBufferMarkerExtension : 1;
      // uint32_t AMDDeviceCoherentMemoryExtension : 1;
      // uint32_t AMDDeviceCoherentMemorySupported : 1;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    // The adapter is the Metal device itself.
    struct {
      MTL::Device *device;
    } mtl;
#endif
  };
};

struct RIDevice {
  RIDevice() { memset(this, 0, sizeof(*this)); }
  // Creates the logical device, queues and VMA allocator on the adapter
  // selected in init->physicalAdapter (RIDeviceDesc lives in RIRenderer.h).
  int init(struct RIRenderer *renderer, struct RIDeviceDesc *init);
  void dispose();
  struct RIPhysicalAdapter physicalAdapter;
  struct RIRenderer *renderer;
  struct RIQueue queues[RI_QUEUE_LEN];
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t maintenance5Features : 1;
      uint32_t conservaitveRasterTier : 1;
      uint32_t swapchainMutableFormat : 1;
      uint32_t memoryBudget : 1;
      VkDevice device;
      VmaAllocator vmaAllocator;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::Device *device;
      MTL::CommandQueue *defaultQueue;
      // Shared heap for bindless material/image textures (RI_TEXTURE_FLAG_
      // BINDLESS_HEAP). One useHeap() makes them all resident for argument-
      // buffer access instead of useResource per texture. Heap exhaustion is a
      // fatal error (raise kBindlessTextureHeapBytes) — there is no fallback,
      // since a device-allocated bindless texture would not be resident for the
      // ray-trace pass. A pointer so the RIDevice() memset zero-inits it safely.
      MTL::Heap *textureHeap;
    } mtl;
#endif
  };
};

static inline bool IsRICmdValid(struct RIRenderer *renderer,
                                struct RICmd *cmd) {
  switch (renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return cmd->vk.pool && cmd->vk.cmd;
#endif
#if (DEVICE_IMPL_MTL)
  case RI_DEVICE_API_MTL:
    return cmd->mtl.cmd != nullptr;
#endif
  default:
    return false;
  }
}

static inline bool IsRIBufferValid(struct RIRenderer *renderer,
                                   const struct RIBuffer *handle) {
  switch (renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return handle && handle->vk.buffer != NULL;
#endif
#if (DEVICE_IMPL_MTL)
  case RI_DEVICE_API_MTL:
    return handle && handle->mtl.buffer != nullptr;
#endif
  default:
    return false;
  }
}

static inline bool IsRITextureValid(struct RIRenderer *renderer,
                                    const struct RITexture *handle) {
  switch (renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return handle && handle->vk.image != NULL;
#endif
#if (DEVICE_IMPL_MTL)
  case RI_DEVICE_API_MTL:
    return handle && handle->mtl.texture != nullptr;
#endif
  default:
    return false;
  }
}

inline bool RITexture::isEmpty(const struct RIDevice *device) const {
  switch (device->renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return vk.image == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_MTL)
  case RI_DEVICE_API_MTL:
    return mtl.texture == nullptr;
#endif
  default:
    assert(false && "unhandled backend");
    return true;
  }
}

inline bool RIBuffer::isEmpty(const struct RIDevice *device) const {
  switch (device->renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return vk.buffer == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_MTL)
  case RI_DEVICE_API_MTL:
    return mtl.buffer == nullptr;
#endif
  default:
    assert(false && "unhandled backend");
    return true;
  }
}

inline bool RIAccelStructure::isEmpty(const struct RIRenderer *renderer) const {
  switch (renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return vk.handle == VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_MTL)
  case RI_DEVICE_API_MTL:
    return mtl.handle == nullptr;
#endif
  default:
    assert(false && "unhandled backend");
    return true;
  }
}

template <uint32_t MaxImageCount>
inline void RISwapchain<MaxImageCount>::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_MTL)
  if (device->renderer->api == RI_DEVICE_API_MTL) {
    // The CA::MetalLayer is owned by the window; only release a drawable we
    // still hold a ref to (acquired but not presented).
    if (mtl.drawable) {
      mtl.drawable->release();
      mtl.drawable = nullptr;
    }
    return;
  }
#endif
#if (DEVICE_IMPL_VULKAN)
  for (uint32_t p = 0; p < MaxImageCount; p++) {
    if (vk.imageAcquireSem[p])
      vkDestroySemaphore(device->vk.device, vk.imageAcquireSem[p], NULL);
    if (vk.finishSem[p])
      vkDestroySemaphore(device->vk.device, vk.finishSem[p], NULL);
  }
  if (vk.swapchain)
    vkDestroySwapchainKHR(device->vk.device, vk.swapchain, NULL);
  if (vk.surface)
    vkDestroySurfaceKHR(device->renderer->vk.instance, vk.surface, NULL);
#endif
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer<MaxPoolCount, CmdPerPool>::init(
    struct RIDevice *device, struct RIQueue *queue, uint32_t poolCount,
    uint32_t cmdPerPool, bool syncPrimitives) {
  assert(poolCount > 0 && poolCount <= MaxPoolCount);
  assert(cmdPerPool > 0 && cmdPerPool <= CmdPerPool);
  memset(this, 0, sizeof(*this));
  this->poolCount = poolCount;
  this->cmdPerPool = cmdPerPool;
  this->syncPrimitive = syncPrimitives;

  poolIndex = 0;
  cmdIndex = 0;
  fenceIndex = 0;

  for (uint32_t poolIdx = 0; poolIdx < poolCount; poolIdx++) {
    pools[poolIdx].init(device, queue);
    for (uint32_t cmdIdx = 0; cmdIdx < cmdPerPool; cmdIdx++) {
      cmds[poolIdx][cmdIdx].init(device, &pools[poolIdx]);
#if (DEVICE_IMPL_VULKAN)
      if (syncPrimitives && device->renderer->api == RI_DEVICE_API_VK) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_WrapResult(vkCreateSemaphore(device->vk.device, &semaphoreCreateInfo,
                                        NULL, &vk.semaphores[poolIdx][cmdIdx]));

        VkFenceCreateInfo fenceCreateInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_WrapResult(vkCreateFence(device->vk.device, &fenceCreateInfo, NULL,
                                    &vk.fences[poolIdx][cmdIdx]));
      }
#endif
#if (DEVICE_IMPL_MTL)
      if (syncPrimitives && device->renderer->api == RI_DEVICE_API_MTL) {
        // vk semaphore+fence -> a Metal timeline event signalled to a
        // monotonically increasing value per submission.
        mtl.events[poolIdx][cmdIdx] = device->mtl.device->newSharedEvent();
        mtl.values[poolIdx][cmdIdx] = 0;
      }
#endif
    }
  }
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void
RICommandRingBuffer<MaxPoolCount, CmdPerPool>::dispose(struct RIDevice *device) {
  for (uint32_t poolIdx = 0; poolIdx < poolCount; poolIdx++) {
    for (uint32_t cmdIdx = 0; cmdIdx < cmdPerPool; cmdIdx++) {
      cmds[poolIdx][cmdIdx].dispose(device);
#if (DEVICE_IMPL_VULKAN)
      if (syncPrimitive && device->renderer->api == RI_DEVICE_API_VK) {
        vkDestroySemaphore(device->vk.device, vk.semaphores[poolIdx][cmdIdx], NULL);
        vkDestroyFence(device->vk.device, vk.fences[poolIdx][cmdIdx], NULL);
      }
#endif
#if (DEVICE_IMPL_MTL)
      if (syncPrimitive && device->renderer->api == RI_DEVICE_API_MTL) {
        if (mtl.events[poolIdx][cmdIdx]) {
          mtl.events[poolIdx][cmdIdx]->release();
          mtl.events[poolIdx][cmdIdx] = nullptr;
        }
      }
#endif
    }
    pools[poolIdx].dispose(device);
  }
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer<MaxPoolCount, CmdPerPool>::advance() {
  poolIndex = (poolIndex + 1) % poolCount;
  cmdIndex = 0;
  fenceIndex = 0;
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline struct RICommandRingElement
RICommandRingBuffer<MaxPoolCount, CmdPerPool>::acquire(
    struct RIDevice *device, uint32_t numCmds) {
  struct RICommandRingElement result;
  memset(&result, 0, sizeof(struct RICommandRingElement));

  assert(numCmds <= cmdPerPool);
  assert(numCmds + cmdIndex <= cmdPerPool);

  result.cmds = &cmds[poolIndex][cmdIndex];
  result.numCmds = numCmds;
  result.pool = &pools[poolIndex];
#if (DEVICE_IMPL_VULKAN)
  if (syncPrimitive && device->renderer->api == RI_DEVICE_API_VK) {
    result.vk.semaphore = vk.semaphores[poolIndex][fenceIndex];
    result.vk.fence = vk.fences[poolIndex][fenceIndex];
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (syncPrimitive && device->renderer->api == RI_DEVICE_API_MTL) {
    result.mtl.event = mtl.events[poolIndex][fenceIndex];
    // Last-signalled value (host-wait target); the submit increments the slot
    // through signalValueSlot and signals the new value.
    result.mtl.signalValue = mtl.values[poolIndex][fenceIndex];
    result.mtl.signalValueSlot = &mtl.values[poolIndex][fenceIndex];
  }
#endif

  fenceIndex += 1;
  cmdIndex += numCmds;

  return result;
}

#endif
