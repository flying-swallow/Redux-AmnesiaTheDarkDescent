#ifndef RI_MTL_H
#define RI_MTL_H

// Metal (metal-cpp) conversion helpers. Parallels RIVK.h for the Vulkan
// backend. Only compiled when the Metal backend is selected; every symbol is
// guarded by DEVICE_IMPL_MTL so this header is a no-op under other backends.

#include "graphics/RIDefines.h"

#if (DEVICE_IMPL_MTL)

#include "graphics/RIFormat.h"
#include <Metal/Metal.hpp>

// RI_Format_e -> MTL::PixelFormat. Returns MTL::PixelFormatInvalid for formats
// with no direct Metal equivalent (e.g. 24-bit RGB, packed RGB32). Mirrors the
// NRI ConversionMTL GetFormatMTL table.
static inline MTL::PixelFormat RIToMTLFormat(uint32_t format) {
  switch ((enum RI_Format_e)format) {
  case RI_FORMAT_R8_UNORM:        return MTL::PixelFormatR8Unorm;
  case RI_FORMAT_R8_SNORM:        return MTL::PixelFormatR8Snorm;
  case RI_FORMAT_R8_UINT:         return MTL::PixelFormatR8Uint;
  case RI_FORMAT_R8_SINT:         return MTL::PixelFormatR8Sint;
  case RI_FORMAT_RG8_UNORM:       return MTL::PixelFormatRG8Unorm;
  case RI_FORMAT_RG8_SNORM:       return MTL::PixelFormatRG8Snorm;
  case RI_FORMAT_RG8_UINT:        return MTL::PixelFormatRG8Uint;
  case RI_FORMAT_RG8_SINT:        return MTL::PixelFormatRG8Sint;
  case RI_FORMAT_BGRA8_UNORM:     return MTL::PixelFormatBGRA8Unorm;
  case RI_FORMAT_BGRA8_SRGB:      return MTL::PixelFormatBGRA8Unorm_sRGB;
  case RI_FORMAT_RGBA8_UNORM:     return MTL::PixelFormatRGBA8Unorm;
  case RI_FORMAT_RGBA8_SNORM:     return MTL::PixelFormatRGBA8Snorm;
  case RI_FORMAT_RGBA8_UINT:      return MTL::PixelFormatRGBA8Uint;
  case RI_FORMAT_RGBA8_SINT:      return MTL::PixelFormatRGBA8Sint;
  case RI_FORMAT_RGBA8_SRGB:      return MTL::PixelFormatRGBA8Unorm_sRGB;
  case RI_FORMAT_R16_UNORM:       return MTL::PixelFormatR16Unorm;
  case RI_FORMAT_R16_SNORM:       return MTL::PixelFormatR16Snorm;
  case RI_FORMAT_R16_UINT:        return MTL::PixelFormatR16Uint;
  case RI_FORMAT_R16_SINT:        return MTL::PixelFormatR16Sint;
  case RI_FORMAT_R16_SFLOAT:      return MTL::PixelFormatR16Float;
  case RI_FORMAT_RG16_UNORM:      return MTL::PixelFormatRG16Unorm;
  case RI_FORMAT_RG16_SNORM:      return MTL::PixelFormatRG16Snorm;
  case RI_FORMAT_RG16_UINT:       return MTL::PixelFormatRG16Uint;
  case RI_FORMAT_RG16_SINT:       return MTL::PixelFormatRG16Sint;
  case RI_FORMAT_RG16_SFLOAT:     return MTL::PixelFormatRG16Float;
  case RI_FORMAT_RGBA16_UNORM:    return MTL::PixelFormatRGBA16Unorm;
  case RI_FORMAT_RGBA16_SNORM:    return MTL::PixelFormatRGBA16Snorm;
  case RI_FORMAT_RGBA16_UINT:     return MTL::PixelFormatRGBA16Uint;
  case RI_FORMAT_RGBA16_SINT:     return MTL::PixelFormatRGBA16Sint;
  case RI_FORMAT_RGBA16_SFLOAT:   return MTL::PixelFormatRGBA16Float;
  case RI_FORMAT_R32_UINT:        return MTL::PixelFormatR32Uint;
  case RI_FORMAT_R32_SINT:        return MTL::PixelFormatR32Sint;
  case RI_FORMAT_R32_SFLOAT:      return MTL::PixelFormatR32Float;
  case RI_FORMAT_RG32_UINT:       return MTL::PixelFormatRG32Uint;
  case RI_FORMAT_RG32_SINT:       return MTL::PixelFormatRG32Sint;
  case RI_FORMAT_RG32_SFLOAT:     return MTL::PixelFormatRG32Float;
  case RI_FORMAT_RGBA32_UINT:     return MTL::PixelFormatRGBA32Uint;
  case RI_FORMAT_RGBA32_SINT:     return MTL::PixelFormatRGBA32Sint;
  case RI_FORMAT_RGBA32_SFLOAT:   return MTL::PixelFormatRGBA32Float;
  case RI_FORMAT_R10_G10_B10_A2_UNORM: return MTL::PixelFormatRGB10A2Unorm;
  case RI_FORMAT_R10_G10_B10_A2_UINT:  return MTL::PixelFormatRGB10A2Uint;
  case RI_FORMAT_R11_G11_B10_UFLOAT:   return MTL::PixelFormatRG11B10Float;
  case RI_FORMAT_R9_G9_B9_E5_UNORM:    return MTL::PixelFormatRGB9E5Float;
  // Depth / stencil. Depth24Unorm_Stencil8 is unsupported on Apple Silicon;
  // callers should prefer Depth32Float_Stencil8 there.
  case RI_FORMAT_D16_UNORM:            return MTL::PixelFormatDepth16Unorm;
  case RI_FORMAT_D32_SFLOAT:           return MTL::PixelFormatDepth32Float;
  case RI_FORMAT_D24_UNORM_S8_UINT:    return MTL::PixelFormatDepth24Unorm_Stencil8;
  case RI_FORMAT_D32_SFLOAT_S8_UINT:   return MTL::PixelFormatDepth32Float_Stencil8;
  // Block-compressed (BC) formats are macOS (Intel) only; Apple GPUs expose
  // ASTC/ETC instead. Mapped for completeness.
  case RI_FORMAT_BC1_RGBA_UNORM:  return MTL::PixelFormatBC1_RGBA;
  case RI_FORMAT_BC1_RGBA_SRGB:   return MTL::PixelFormatBC1_RGBA_sRGB;
  case RI_FORMAT_BC2_RGBA_UNORM:  return MTL::PixelFormatBC2_RGBA;
  case RI_FORMAT_BC2_RGBA_SRGB:   return MTL::PixelFormatBC2_RGBA_sRGB;
  case RI_FORMAT_BC3_RGBA_UNORM:  return MTL::PixelFormatBC3_RGBA;
  case RI_FORMAT_BC3_RGBA_SRGB:   return MTL::PixelFormatBC3_RGBA_sRGB;
  case RI_FORMAT_BC4_R_UNORM:     return MTL::PixelFormatBC4_RUnorm;
  case RI_FORMAT_BC4_R_SNORM:     return MTL::PixelFormatBC4_RSnorm;
  case RI_FORMAT_BC5_RG_UNORM:    return MTL::PixelFormatBC5_RGUnorm;
  case RI_FORMAT_BC5_RG_SNORM:    return MTL::PixelFormatBC5_RGSnorm;
  case RI_FORMAT_BC6H_RGB_UFLOAT: return MTL::PixelFormatBC6H_RGBUfloat;
  case RI_FORMAT_BC6H_RGB_SFLOAT: return MTL::PixelFormatBC6H_RGBFloat;
  case RI_FORMAT_BC7_RGBA_UNORM:  return MTL::PixelFormatBC7_RGBAUnorm;
  case RI_FORMAT_BC7_RGBA_SRGB:   return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
  default:
    return MTL::PixelFormatInvalid;
  }
}

// RISwapchainFormat_e -> the RI_Format_e the CA::MetalLayer should be created
// with. Metal layers only support a small set of drawable pixel formats.
static inline uint32_t RISwapchainFormatToRIFormat(uint8_t swapchainFormat) {
  switch ((enum RISwapchainFormat_e)swapchainFormat) {
  case RI_SWAPCHAIN_BT709_G10_16BIT:    return RI_FORMAT_RGBA16_SFLOAT;
  case RI_SWAPCHAIN_BT709_G22_8BIT:     return RI_FORMAT_BGRA8_UNORM;
  case RI_SWAPCHAIN_BT709_G22_10BIT:    return RI_FORMAT_R10_G10_B10_A2_UNORM;
  case RI_SWAPCHAIN_BT2020_G2084_10BIT: return RI_FORMAT_R10_G10_B10_A2_UNORM;
  default:                              return RI_FORMAT_BGRA8_UNORM;
  }
}

#include "graphics/RITypes.h" // RITopology_e / RIBlendFactor_e / etc.

// RITopology_e -> MTL primitive class (for the render-pipeline descriptor) and
// the per-draw primitive type. Metal splits these: the pipeline takes a
// MTL::PrimitiveTopologyClass, draws take a MTL::PrimitiveType.
static inline MTL::PrimitiveTopologyClass RIToMTLTopologyClass(uint8_t topo) {
  switch ((enum RITopology_e)topo) {
  case RI_TOPOLOGY_POINT_LIST:
    return MTL::PrimitiveTopologyClassPoint;
  case RI_TOPOLOGY_LINE_LIST:
  case RI_TOPOLOGY_LINE_STRIP:
    return MTL::PrimitiveTopologyClassLine;
  default:
    return MTL::PrimitiveTopologyClassTriangle;
  }
}

static inline MTL::PrimitiveType RIToMTLPrimitiveType(uint8_t topo) {
  switch ((enum RITopology_e)topo) {
  case RI_TOPOLOGY_POINT_LIST:
    return MTL::PrimitiveTypePoint;
  case RI_TOPOLOGY_LINE_LIST:
    return MTL::PrimitiveTypeLine;
  case RI_TOPOLOGY_LINE_STRIP:
    return MTL::PrimitiveTypeLineStrip;
  case RI_TOPOLOGY_TRIANGLE_STRIP:
    return MTL::PrimitiveTypeTriangleStrip;
  default:
    return MTL::PrimitiveTypeTriangle;
  }
}

static inline MTL::BlendFactor RIToMTLBlendFactor(uint8_t f) {
  switch ((enum RIBlendFactor_e)f) {
  case RI_BLEND_ZERO: return MTL::BlendFactorZero;
  case RI_BLEND_ONE: return MTL::BlendFactorOne;
  case RI_BLEND_SRC_COLOR: return MTL::BlendFactorSourceColor;
  case RI_BLEND_ONE_MINUS_SRC_COLOR: return MTL::BlendFactorOneMinusSourceColor;
  case RI_BLEND_DST_COLOR: return MTL::BlendFactorDestinationColor;
  case RI_BLEND_ONE_MINUS_DST_COLOR: return MTL::BlendFactorOneMinusDestinationColor;
  case RI_BLEND_SRC_ALPHA: return MTL::BlendFactorSourceAlpha;
  case RI_BLEND_ONE_MINUS_SRC_ALPHA: return MTL::BlendFactorOneMinusSourceAlpha;
  case RI_BLEND_DST_ALPHA: return MTL::BlendFactorDestinationAlpha;
  case RI_BLEND_ONE_MINUS_DST_ALPHA: return MTL::BlendFactorOneMinusDestinationAlpha;
  case RI_BLEND_CONSTANT_COLOR: return MTL::BlendFactorBlendColor;
  case RI_BLEND_ONE_MINUS_CONSTANT_COLOR: return MTL::BlendFactorOneMinusBlendColor;
  case RI_BLEND_CONSTANT_ALPHA: return MTL::BlendFactorBlendAlpha;
  case RI_BLEND_ONE_MINUS_CONSTANT_ALPHA: return MTL::BlendFactorOneMinusBlendAlpha;
  case RI_BLEND_SRC_ALPHA_SATURATE: return MTL::BlendFactorSourceAlphaSaturated;
  case RI_BLEND_SRC1_COLOR: return MTL::BlendFactorSource1Color;
  case RI_BLEND_ONE_MINUS_SRC1_COLOR: return MTL::BlendFactorOneMinusSource1Color;
  case RI_BLEND_SRC1_ALPHA: return MTL::BlendFactorSource1Alpha;
  case RI_BLEND_ONE_MINUS_SRC1_ALPHA: return MTL::BlendFactorOneMinusSource1Alpha;
  }
  return MTL::BlendFactorZero;
}

static inline MTL::BlendOperation RIToMTLBlendOp(uint8_t op) {
  switch ((enum RIBlendOp_e)op) {
  case RI_BLEND_OP_ADD: return MTL::BlendOperationAdd;
  case RI_BLEND_OP_SUBTRACT: return MTL::BlendOperationSubtract;
  case RI_BLEND_OP_REVERSE_SUBTRACT: return MTL::BlendOperationReverseSubtract;
  case RI_BLEND_OP_MIN: return MTL::BlendOperationMin;
  case RI_BLEND_OP_MAX: return MTL::BlendOperationMax;
  }
  return MTL::BlendOperationAdd;
}

static inline MTL::ColorWriteMask RIToMTLColorWriteMask(uint8_t mask) {
  MTL::ColorWriteMask m = MTL::ColorWriteMaskNone;
  if (mask & RI_COLOR_WRITE_R) m |= MTL::ColorWriteMaskRed;
  if (mask & RI_COLOR_WRITE_G) m |= MTL::ColorWriteMaskGreen;
  if (mask & RI_COLOR_WRITE_B) m |= MTL::ColorWriteMaskBlue;
  if (mask & RI_COLOR_WRITE_A) m |= MTL::ColorWriteMaskAlpha;
  return m;
}

// RI_Format_e -> MTL::VertexFormat for vertex-attribute layouts (distinct from
// the pixel-format enum). Covers the formats vertex streams actually use.
static inline MTL::VertexFormat RIToMTLVertexFormat(uint32_t format) {
  switch ((enum RI_Format_e)format) {
  case RI_FORMAT_R32_SFLOAT: return MTL::VertexFormatFloat;
  case RI_FORMAT_RG32_SFLOAT: return MTL::VertexFormatFloat2;
  case RI_FORMAT_RGB32_SFLOAT: return MTL::VertexFormatFloat3;
  case RI_FORMAT_RGBA32_SFLOAT: return MTL::VertexFormatFloat4;
  case RI_FORMAT_R32_UINT: return MTL::VertexFormatUInt;
  case RI_FORMAT_RG32_UINT: return MTL::VertexFormatUInt2;
  case RI_FORMAT_RGB32_UINT: return MTL::VertexFormatUInt3;
  case RI_FORMAT_RGBA32_UINT: return MTL::VertexFormatUInt4;
  case RI_FORMAT_R32_SINT: return MTL::VertexFormatInt;
  case RI_FORMAT_RG32_SINT: return MTL::VertexFormatInt2;
  case RI_FORMAT_RGB32_SINT: return MTL::VertexFormatInt3;
  case RI_FORMAT_RGBA32_SINT: return MTL::VertexFormatInt4;
  case RI_FORMAT_RGBA8_UNORM: return MTL::VertexFormatUChar4Normalized;
  case RI_FORMAT_RGBA8_UINT: return MTL::VertexFormatUChar4;
  case RI_FORMAT_RG16_SFLOAT: return MTL::VertexFormatHalf2;
  case RI_FORMAT_RGBA16_SFLOAT: return MTL::VertexFormatHalf4;
  default: return MTL::VertexFormatInvalid;
  }
}

// Convention for RIDescriptor::mtl.type — set by the descriptor creator so
// bindDescriptors knows how to lay it out in the argument buffer. (Metal has no
// VkDescriptorType; this is the minimal set the engine binds.)
enum RIMTLDescriptorType_e {
  RI_MTL_DESC_NONE = 0,
  RI_MTL_DESC_TEXTURE,
  RI_MTL_DESC_SAMPLER,
  RI_MTL_DESC_BUFFER,
  RI_MTL_DESC_ACCELERATION_STRUCTURE,
};

static inline MTL::DataType RIMTLDescriptorDataType(uint32_t type) {
  switch ((enum RIMTLDescriptorType_e)type) {
  case RI_MTL_DESC_TEXTURE: return MTL::DataTypeTexture;
  case RI_MTL_DESC_SAMPLER: return MTL::DataTypeSampler;
  case RI_MTL_DESC_ACCELERATION_STRUCTURE:
    return MTL::DataTypePrimitiveAccelerationStructure;
  default: return MTL::DataTypePointer; // buffers bound by address
  }
}

// eTextureWrap (GraphicsTypes.h) -> MTL::SamplerAddressMode. Passed as int to
// avoid pulling the engine header in; values match the eTextureWrap enum order
// (0=Repeat,1=Clamp,2=ClampToEdge,3=ClampToBorder).
static inline MTL::SamplerAddressMode RIToMTLSamplerAddress(int wrap) {
  switch (wrap) {
  case 0: return MTL::SamplerAddressModeRepeat;
  case 1: return MTL::SamplerAddressModeClampToEdge;
  case 2: return MTL::SamplerAddressModeClampToEdge;
  case 3: return MTL::SamplerAddressModeClampToBorderColor;
  default: return MTL::SamplerAddressModeClampToEdge;
  }
}

static inline MTL::CompareFunction RIToMTLCompareFunc(uint8_t f) {
  switch ((enum RICompareFunc_e)f) {
  case RI_COMPARE_NONE:
  case RI_COMPARE_ALWAYS: return MTL::CompareFunctionAlways;
  case RI_COMPARE_NEVER: return MTL::CompareFunctionNever;
  case RI_COMPARE_EQUAL: return MTL::CompareFunctionEqual;
  case RI_COMPARE_NOT_EQUAL: return MTL::CompareFunctionNotEqual;
  case RI_COMPARE_LESS: return MTL::CompareFunctionLess;
  case RI_COMPARE_LESS_EQUAL: return MTL::CompareFunctionLessEqual;
  case RI_COMPARE_GREATER: return MTL::CompareFunctionGreater;
  case RI_COMPARE_GREATER_EQUAL: return MTL::CompareFunctionGreaterEqual;
  }
  return MTL::CompareFunctionAlways;
}

static inline MTL::StencilOperation RIToMTLStencilOp(uint8_t op) {
  switch ((enum RIStencilOp_e)op) {
  case RI_STENCIL_OP_KEEP: return MTL::StencilOperationKeep;
  case RI_STENCIL_OP_ZERO: return MTL::StencilOperationZero;
  case RI_STENCIL_OP_REPLACE: return MTL::StencilOperationReplace;
  case RI_STENCIL_OP_INCREMENT_CLAMP: return MTL::StencilOperationIncrementClamp;
  case RI_STENCIL_OP_DECREMENT_CLAMP: return MTL::StencilOperationDecrementClamp;
  case RI_STENCIL_OP_INVERT: return MTL::StencilOperationInvert;
  case RI_STENCIL_OP_INCREMENT_WRAP: return MTL::StencilOperationIncrementWrap;
  case RI_STENCIL_OP_DECREMENT_WRAP: return MTL::StencilOperationDecrementWrap;
  }
  return MTL::StencilOperationKeep;
}

// ---------------------------------------------------------------------------
// Acceleration structures (ray tracing). Parallels the RIVK.h accel helpers.
// ---------------------------------------------------------------------------

// RI_Format_e -> MTL::AttributeFormat for acceleration-structure vertex data.
// Distinct enum from MTL::VertexFormat (RIToMTLVertexFormat) used by vertex
// streams; the AS triangle descriptor takes an AttributeFormat.
static inline MTL::AttributeFormat RIToMTLAttributeFormat(uint32_t format) {
  switch ((enum RI_Format_e)format) {
  case RI_FORMAT_RGB32_SFLOAT:  return MTL::AttributeFormatFloat3;
  case RI_FORMAT_RG32_SFLOAT:   return MTL::AttributeFormatFloat2;
  case RI_FORMAT_RGBA32_SFLOAT: return MTL::AttributeFormatFloat4;
  case RI_FORMAT_RG16_SFLOAT:   return MTL::AttributeFormatHalf2;
  case RI_FORMAT_RGBA16_SFLOAT: return MTL::AttributeFormatHalf4;
  default:                      return MTL::AttributeFormatInvalid;
  }
}

static inline MTL::IndexType RIToMTLIndexType(enum RIIndexType_e t) {
  return (t == RI_INDEX_TYPE_16) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
}

// RITextureUsageBits_e -> MTL::TextureUsage. TRANSFER bits are no-ops on Metal
// (blit works on any texture). MUTABLE_FORMAT maps to the PixelFormatView usage
// so newTextureView can reinterpret the format.
static inline MTL::TextureUsage RIToMTLTextureUsage(uint32_t usage,
                                                    uint32_t flags) {
  MTL::TextureUsage u = MTL::TextureUsageUnknown;
  if (usage & RI_USAGE_SHADER_RESOURCE)         u |= MTL::TextureUsageShaderRead;
  if (usage & RI_USAGE_SHADER_RESOURCE_STORAGE) u |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
  if (usage & RI_USAGE_COLOR_ATTACHMENT)        u |= MTL::TextureUsageRenderTarget;
  if (usage & RI_USAGE_DEPTH_STENCIL_ATTACHMENT) u |= MTL::TextureUsageRenderTarget;
  if (flags & RI_TEXTURE_FLAG_MUTABLE_FORMAT)   u |= MTL::TextureUsagePixelFormatView;
  return u;
}

// (RITextureType_e type, layer count, cube flag) -> MTL::TextureType for image
// creation.
static inline MTL::TextureType RIToMTLTextureType(uint8_t type,
                                                  uint32_t layerNum,
                                                  bool cube) {
  if (cube)
    return layerNum > 6 ? MTL::TextureTypeCubeArray : MTL::TextureTypeCube;
  switch ((enum RITextureType_e)type) {
  case RI_TEXTURE_3D:
    return MTL::TextureType3D;
  case RI_TEXTURE_1D:
    return layerNum > 1 ? MTL::TextureType1DArray : MTL::TextureType1D;
  case RI_TEXTURE_2D:
  default:
    return layerNum > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
  }
}

// RITextureViewType_e -> MTL::TextureType for newTextureView.
static inline MTL::TextureType
RIToMTLTextureViewType(enum RITextureViewType_e v) {
  switch (v) {
  case RI_VIEWTYPE_SHADER_RESOURCE_1D:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D:
    return MTL::TextureType1D;
  case RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY:
    return MTL::TextureType1DArray;
  case RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY:
    return MTL::TextureType2DArray;
  case RI_VIEWTYPE_SHADER_RESOURCE_CUBE:
    return MTL::TextureTypeCube;
  case RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY:
    return MTL::TextureTypeCubeArray;
  case RI_VIEWTYPE_SHADER_RESOURCE_3D:
  case RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D:
    return MTL::TextureType3D;
  default:
    return MTL::TextureType2D;
  }
}

// RIAccelStructureBuildBits_e -> MTL::AccelerationStructureUsage. Metal has no
// COMPACTION / DATA_ACCESS usage bits; those are dropped.
static inline MTL::AccelerationStructureUsage RIToMTLAccelUsage(uint32_t flags) {
  MTL::AccelerationStructureUsage u = MTL::AccelerationStructureUsageNone;
  if (flags & RI_ACCEL_BUILD_ALLOW_UPDATE)
    u |= MTL::AccelerationStructureUsageRefit;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_BUILD)
    u |= MTL::AccelerationStructureUsagePreferFastBuild;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_TRACE)
    u |= MTL::AccelerationStructureUsagePreferFastIntersection;
  if (flags & RI_ACCEL_BUILD_MINIMIZE_MEMORY)
    u |= MTL::AccelerationStructureUsageMinimizeMemory;
  return u;
}

static inline MTL::AccelerationStructureInstanceOptions
RIToMTLAccelInstanceOptions(uint32_t flags) {
  MTL::AccelerationStructureInstanceOptions o =
      MTL::AccelerationStructureInstanceOptionNone;
  if (flags & RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE)
    o |= MTL::AccelerationStructureInstanceOptionDisableTriangleCulling;
  if (flags & RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING)
    o |= MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise;
  if (flags & RI_ACCEL_INSTANCE_FORCE_OPAQUE)
    o |= MTL::AccelerationStructureInstanceOptionOpaque;
  if (flags & RI_ACCEL_INSTANCE_FORCE_NON_OPAQUE)
    o |= MTL::AccelerationStructureInstanceOptionNonOpaque;
  return o;
}

// Build an MTL geometry descriptor from an RIAccelGeometryDesc. Returns an
// autoreleased object (caller must NOT release it; drained by the enclosing
// NS::AutoreleasePool). Returns NULL for unsupported geometry. Mirrors
// RI_VK_FillGeometry.
static inline MTL::AccelerationStructureGeometryDescriptor *
RI_MTL_MakeGeometry(const struct RIAccelGeometryDesc *src) {
  switch (src->type) {
  case RI_ACCEL_GEOMETRY_TYPE_TRIANGLES: {
    const struct RIAccelTrianglesDesc *tri = &src->triangles;
    MTL::AccelerationStructureTriangleGeometryDescriptor *t =
        MTL::AccelerationStructureTriangleGeometryDescriptor::descriptor();
    t->setVertexBuffer(tri->vertexBuffer ? tri->vertexBuffer->mtl.buffer
                                         : nullptr);
    t->setVertexBufferOffset(tri->vertexOffset);
    t->setVertexFormat(RIToMTLAttributeFormat((uint32_t)tri->vertexFormat));
    t->setVertexStride(tri->vertexStride);
    if (tri->indexBuffer) {
      t->setIndexBuffer(tri->indexBuffer->mtl.buffer);
      t->setIndexBufferOffset(tri->indexOffset);
      t->setIndexType(RIToMTLIndexType(tri->indexType));
      t->setTriangleCount(tri->indexNum / 3);
    } else {
      t->setTriangleCount(tri->vertexNum / 3);
    }
    t->setOpaque((src->flags & RI_ACCEL_GEOMETRY_OPAQUE) != 0);
    return t;
  }
  case RI_ACCEL_GEOMETRY_TYPE_AABBS:
    // The engine only builds triangle BLASes; AABB geometry on Metal is not
    // wired up yet.
    assert(false && "AABB acceleration geometry not implemented on Metal");
    return nullptr;
  }
  return nullptr;
}

#endif // DEVICE_IMPL_MTL
#endif // RI_MTL_H
