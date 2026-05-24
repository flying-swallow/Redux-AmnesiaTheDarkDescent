#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialResource.h"
#include "graphics/MaterialType.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "scene/Viewport.h"
#include "graphics/Renderable.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer_RI.h"
#include "math/Frustum.h"
#include "math/Math.h"

#include "resources/Resources.h"
#include "scene/Light.h"
#include "scene/LightBox.h"
#include "scene/LightSpot.h"
#include "scene/ParticleEmitter.h"
#include "scene/RenderableContainer.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace hpl {

namespace detail {

// sRGB → linear transfer (IEC 61966-2-1). Inverse of the encode the SRGB
// swapchain applies on write, ussed to bring artist-authored cColor.rgb light
// values into the linear-space lighting math the shaders now run in.
static inline float sRGBToLinear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}


static struct RIBuffer_s CreateBindlessSlotBuffer(RIDevice_s *device,
                                                  uint32_t slotCount,
                                                  size_t elementStride,
                                                  VkBufferUsageFlags usage,
                                                  bool deviceLocalOnly = false) {
  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  VkBufferCreateInfo bufferCreateInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  VK_ConfigureBufferQueueFamilies(&bufferCreateInfo, device->queues,
                                  RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);
  bufferCreateInfo.size = (VkDeviceSize)slotCount * elementStride;
  bufferCreateInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo = {};
  if (deviceLocalOnly) {
    // Pure device-local heap. Caller must seed contents via RI.uploader
    // (RI_ResourceBeginCopyBuffer / EndCopyBuffer) — out.mappedAddress is null.
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  } else {
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  }

  VmaAllocationInfo allocationInfo = {};
  struct RIBuffer_s out;
  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bufferCreateInfo,
                                &allocInfo, &out.vk.buffer, &out.vk.allocation,
                                &allocationInfo));
  out.mappedAddress = deviceLocalOnly ? nullptr : allocationInfo.pMappedData;
  return out;
}

} // namespace detail

namespace {

// Holder for the static portion of the "SurfelGBuffer.3d"
// VkGraphicsPipelineCreateInfo. Owns every sub-struct so the pointer
// chain stays valid as long as the holder lives. Non-copyable /
// non-movable - the pNext / pXxxState pointers would dangle.
struct GBufferMRTPipelineDesc {
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkFormat colorFormat;
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachment;
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  GBufferMRTPipelineDesc(RI_Format_e visibilityFormat, RI_Format_e depthFormat) {
    // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs,
    // so the pipeline declares zero vertex input bindings and attributes.
    vertexInputState = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInputState.vertexBindingDescriptionCount = 0;
    vertexInputState.pVertexBindingDescriptions = nullptr;
    vertexInputState.vertexAttributeDescriptionCount = 0;
    vertexInputState.pVertexAttributeDescriptions = nullptr;

    inputAssemblyState = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    rasterizationState = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizationState.lineWidth = 1.0f;

    dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
    dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
    dynamicState.pDynamicStates = dynamicStates;

    // Single MRT — packed TriangleHit (uint4). The Slang psMain writes only
    // SV_TARGET0; downstream consumers (visibility_shade.frag, surfel passes)
    // decode the uint4 directly.
    colorFormat = RIFormatToVK(visibilityFormat);
    pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipelineRendering.colorAttachmentCount = 1;
    pipelineRendering.pColorAttachmentFormats = &colorFormat;
    pipelineRendering.depthAttachmentFormat = RIFormatToVK(depthFormat);

    viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    multisampleState = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    depthStencilState = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = VK_TRUE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencilState.minDepthBounds = 0.0f;
    depthStencilState.maxDepthBounds = 1.0f;

    // uint MRT — blendEnable must be VK_FALSE (uint formats don't carry
    // VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT). Factors stay identity.
    blendAttachment = {
        VK_FALSE,        VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
        VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
        VK_BLEND_OP_ADD,
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    colorBlendState = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachment;

    createInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    createInfo.pNext = &pipelineRendering;
    createInfo.pVertexInputState = &vertexInputState;
    createInfo.pInputAssemblyState = &inputAssemblyState;
    createInfo.pRasterizationState = &rasterizationState;
    createInfo.pDynamicState = &dynamicState;
    createInfo.pViewportState = &viewportState;
    createInfo.pMultisampleState = &multisampleState;
    createInfo.pDepthStencilState = &depthStencilState;
    createInfo.pColorBlendState = &colorBlendState;

    hash = hash_u32(HASH_INITIAL_VALUE, visibilityFormat);
    hash = hash_u32(hash, depthFormat);
  }

  GBufferMRTPipelineDesc(const GBufferMRTPipelineDesc &) = delete;
  GBufferMRTPipelineDesc &operator=(const GBufferMRTPipelineDesc &) = delete;
};


// Pipeline descriptor for the particle (translucent) pass. One instance per
// blend mode — the hardware blend factors come from the legacy
// translucencyBlendTable mapping in RendererDeferred. Depth test is on but
// depth write is off so particles sort against opaque geometry without
// occluding each other in the wrong order. Cull mode is NONE because
// particle billboards may face the camera either way; legacy renderer
// behaves the same. No vertex input bindings — VS pulls via BDA from
// opaque*Handles[].
struct ParticlePipelineDesc {
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkFormat colorFormats[1];
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachment;
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    BLEND_LAST_ENUM = 5,
  };

  ParticlePipelineDesc(RI_Format_e swapchainFormat, RI_Format_e depthFormat,
                       BlendMode mode) {
    vertexInputState = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInputState.vertexBindingDescriptionCount = 0;
    vertexInputState.pVertexBindingDescriptions = nullptr;
    vertexInputState.vertexAttributeDescriptionCount = 0;
    vertexInputState.pVertexAttributeDescriptions = nullptr;

    inputAssemblyState = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    rasterizationState = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_NONE;
    rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizationState.lineWidth = 1.0f;

    dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
    dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
    dynamicState.pDynamicStates = dynamicStates;

    colorFormats[0] = RIFormatToVK(swapchainFormat);
    pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipelineRendering.colorAttachmentCount = 1;
    pipelineRendering.pColorAttachmentFormats = colorFormats;
    pipelineRendering.depthAttachmentFormat = RIFormatToVK(depthFormat);

    viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    multisampleState = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    depthStencilState = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = VK_FALSE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencilState.minDepthBounds = 0.0f;
    depthStencilState.maxDepthBounds = 1.0f;

    // Match the legacy translucencyBlendTable mapping
    // (RendererDeferred.cpp:3948-3954). The FS applies per-pixel color
    // transforms that prepare its output for these hardware factors.
    blendAttachment = {};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (mode) {
    case BLEND_ADD:
      blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      break;
    case BLEND_MUL:
      blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
      blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      break;
    case BLEND_MULX2:
      blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
      blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
      blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      break;
    case BLEND_ALPHA:
      blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blendAttachment.dstColorBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstAlphaBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    case BLEND_PREMUL_ALPHA:
      blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstColorBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstAlphaBlendFactor =
          VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    default:
      break;
    }
    colorBlendState = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachment;

    createInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    createInfo.pNext = &pipelineRendering;
    createInfo.pVertexInputState = &vertexInputState;
    createInfo.pInputAssemblyState = &inputAssemblyState;
    createInfo.pRasterizationState = &rasterizationState;
    createInfo.pDynamicState = &dynamicState;
    createInfo.pViewportState = &viewportState;
    createInfo.pMultisampleState = &multisampleState;
    createInfo.pDepthStencilState = &depthStencilState;
    createInfo.pColorBlendState = &colorBlendState;

    hash = hash_u32(HASH_INITIAL_VALUE, swapchainFormat);
    hash = hash_u32(hash, depthFormat);
    hash = hash_u32(hash, (uint32_t)mode);
  }

  ParticlePipelineDesc(const ParticlePipelineDesc &) = delete;
  ParticlePipelineDesc &operator=(const ParticlePipelineDesc &) = delete;
};


} // namespace

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources, 0),
      m_diffuseBindless(kObjectSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_textureBindless(kTextureSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_textureCubeBindless(kTextureSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_materialBindless(kMaterialSlotCapacity, RI_NUMBER_FRAMES_FLIGHT) {
  {
    {
      std::vector<RIBindlessDescriptorSet::Binding> bindings = {};
      // Stage mask shared by every binding the SurfelGI RT pipeline touches —
      // raygen/any-hit/closest-hit/miss all need bindless texture+vertex
      // access for the alpha test, camera-ray reconstruction, and (in Stage E)
      // material shading inside the path-tracer.
      const VkShaderStageFlags kRtSharedStages =
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR |
          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
          VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
      // textures_2d[] — sampled by gbuffer (FS), visibility_shade (FS), and
      // the SurfelGI RT pipeline's any-hit alpha test + closest-hit albedo.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingTextures2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          kTextureSlotCapacity, kRtSharedStages,
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
      // textures_cube[] — point-light gobos + env maps; also sampled by
      // the RT pipeline's miss shader (env-light contribution, Stage E).
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingTexturesCube, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          kTextureSlotCapacity, kRtSharedStages,
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
      // opaque*Handles bindings 3..8 — vertex pulling for gbuffer VS,
      // bindless triangle fetch in visibility_shade FS, and barycentric
      // hit fetches in the SurfelGI RT pipeline.
      for (uint32_t i = 0; i < 6; ++i) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            kBindingOpaquePositionHandles + i,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
      }
      // materialSampler — paired with textures_2d at every sample site.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingMaterialSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
          kRtSharedStages, 0});
      // SurfelGI SSBOs (bindings 10..19, 27..28). Reachable from compute,
      // ray-tracing, and fragment stages — visibility_shade.frag samples the
      // resulting indirect texture, the update/raytrace passes use compute,
      // and the VBuffer rgen / surfel_rt.rgen write/read these bindings via
      // the ray-tracing pipeline (Stages B, E).
      const uint32_t kSurfelCellBindings[] = {
          kBindingSurfelCounter,      kBindingSurfelBuffer,
          kBindingSurfelGeometry,     kBindingSurfelValidIndex,
          kBindingSurfelDirtyIndex,  kBindingSurfelFreeIndex,
          kBindingSurfelRecycle,      kBindingSurfelRayResult,
          kBindingCellInfo,           kBindingCellToSurfel,
          kBindingSurfelRefCounter,  kBindingSurfelReservation,
          kBindingSurfelBounds,
          kBindingBindlessSlotGeneration, kBindingSurfelSlotGeneration,
      };
      const VkShaderStageFlags kSurfelStageFlags =
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
          VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
          VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
      for (uint32_t b : kSurfelCellBindings) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kSurfelStageFlags, 0});
      }
      // Scene-object + opaque-material tables (bindings 20..21). Read by the
      // gbuffer pipeline (VS / FS), by visibility_shade.frag, and by the
      // SurfelGI RT pipeline at every hit-shader site.
      const uint32_t kSceneTableBindings[] = {
          kBindingSceneObjects,
          kBindingOpaqueMaterial,
      };
      for (uint32_t b : kSceneTableBindings) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
      }
      // Point/spot/box-light SSBOs (bindings 22, 29, 30). visibility_shade
      // reads all three; the Stage E path-tracer also reads them for NEE.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingPointLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          kRtSharedStages, 0});
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingSpotLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          kRtSharedStages, 0});
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingBoxLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          kRtSharedStages, 0});

      // gSurfelDepthSampler stays on set 0 — it's an immutable sampler
      // that never collides with the in-flight frame. All other surfel
      // images (gPackedHitInfo / gIrradianceMap / gSurfelDepthMap /
      // gSurfelDepth) plus the TLAS now live on set 1 and get pushed
      // per-dispatch via RIProgram::bindDescriptors (the same path
      // gPerFrame uses), since RIProgram allocates set 1 from a
      // frame-rotated pool that doesn't collide with in-flight frames.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingSurfelDepthSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
              VK_SHADER_STAGE_RAYGEN_BIT_KHR |
              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
              VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
          0});

      VkDescriptorPoolSize poolSizes[3] = {};
      // Sampled-image budget covers textures_2d[] + textures_cube[].
      poolSizes[0] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          kTextureSlotCapacity * 2};
      // Storage-buffer pool budget: 6 opaque*Handles + 15 surfel/cell bindings
      // (kSurfelCellBindings, incl. kBindingSurfelBounds + the two slot-generation
      // buffers) + 2 scene/material + 3 light SSBOs = 26. Round up to 27 for one
      // slot of slack.
      poolSizes[1] =
          VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 27};
      // Two samplers: gMaterialSampler + gSurfelDepthSampler.
      poolSizes[2] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 2};

      m_bindlessSet.initialize(&RI.device, bindings, poolSizes);
    }

    const VkDescriptorSetLayout externalLayouts[] = {
        m_bindlessSet.vk.m_bindlessSetLayout};
    {
      // Slang gbuffer pass: one .spv with two named entry points
      // (vsMain / psMain). slangc was invoked with
      // -fvk-use-entrypoint-name so the names survive into SPIR-V and the
      // RIProgram loader can request them through pName.
      auto gbuffer_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelGBuffer.3d.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, gbuffer_bin,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, gbuffer_bin,
                                 "psMain"}};
      m_gbuffer.initialize(&RI.device, stages, externalLayouts);
    }

    // SurfelGI compute / RT programs are introduced in stages B–F of the
    // port. The old surfel_prepare / surfel_update / surfel_raytrace /
    // surfel_integrate / surfel_generation_pass programs are gone because
    // their on-disk .comp sources no longer compile against the new shared
    // structs in forward_shared.h; replacements live alongside the new
    // VBuffer (Stage B), update (Stage D), ray-trace (Stage E), and
    // integrate / generation passes (Stage F).
    // SurfelVBuffer — single Slang .spv with four [shader(...)]-attributed
    // entry points (rayGen / miss / closeHit / anyHit). slangc was invoked
    // with -fvk-use-entrypoint-name so the names survive into SPIR-V; share
    // one blob across all four ModuleStage entries, same pattern as
    // m_surfelUpdate*.
    {
      auto vb_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelVBuffer.rt.spv");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN,      vb_bin, "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS,        vb_bin, "miss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, vb_bin, "closeHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT,     vb_bin, "anyHit"}};
      m_surfelVBuffer.initialize(&RI.device, stages, externalLayouts);
    }
    auto loadComputeProgram = [&](RIProgram &prog, const char *name) {
      auto bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    // Slang-compiled compute. Same load path, but the entry-point name in
    // the SPV is the Slang function name (slangc is invoked with
    // -fvk-use-entrypoint-name), so we pass it through to ModuleStage.
    auto loadSlangCompute = [&](RIProgram &prog, const char *name,
                                const char *entryPoint) {
      auto bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    // SurfelPreparePass: the only buffer it touches is gSurfelCounter
    // (kBindingSurfelCounter on the bindless set 0), so the dispatch
    // site is unchanged — only the loaded SPV + entry point differ.
    loadSlangCompute(m_surfelPrepare, "SurfelPreparePass.cs.spv", "csMain");
    // Frees surfels anchored to geometry destroyed mid-level (dispatched only
    // when VertexBuffer destructions have queued retired bindless slots).
    loadSlangCompute(m_surfelClearByInstance, "SurfelClearByInstance.cs.spv",
                     "csMain");
    // Cell-clearing + ref-counter-clearing are now folded into the
    // SurfelPreparePass + SurfelUpdatePass Slang side; the dedicated
    // GLSL clear passes were removed in the GLSL wipe.
    // SurfelUpdatePass — single .spv with three [numthreads]-marked entry
    // points (collectCellInfo / accumulateCellInfo / updateCellToSurfelBuffer).
    // Load the blob once and reuse the std::span<char> across three
    // initializations; the vector must stay in scope through all three
    // `initialize()` calls because ModuleStage holds a non-owning view.
    {
      auto upd_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelUpdatePass.cs.spv");
      auto initFromBlob = [&](RIProgram &prog, const char *entryPoint) {
        std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
            RIProgram::PROGRAM_STAGE_COMPUTE, upd_bin, entryPoint}};
        prog.initialize(&RI.device, stages, externalLayouts);
      };
      initFromBlob(m_surfelUpdateCollect,    "collectCellInfo");
      initFromBlob(m_surfelUpdateAccumulate, "accumulateCellInfo");
      initFromBlob(m_surfelUpdateScatter,    "updateCellToSurfelBuffer");
    }
    // SurfelRayTrace — single Slang .spv with four [shader(...)] entry points
    // (rayGen / scatterMiss / scatterCloseHit / scatterAnyHit). Shadow rays
    // use inline RayQuery in the same shader so no second miss / anyhit
    // group is needed — keeps the SBT inside RIProgram's single-ray-type
    // hit-group layout.
    {
      auto rt_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelRayTrace.rt.spv");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN,      rt_bin, "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS,        rt_bin, "scatterMiss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, rt_bin, "scatterCloseHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT,     rt_bin, "scatterAnyHit"}};
      m_surfelRT.initialize(&RI.device, stages, externalLayouts);
    }
    loadSlangCompute(m_surfelIntegrate,  "SurfelIntegratePass.cs.spv",   "csMain");
    loadSlangCompute(m_surfelGenerate,   "SurfelGenerationPass.cs.spv",  "csMain");
    // SurfelGIRender — now a graphics pass writing into the pogo buffer's
    // color attachment (was a compute pass writing into the swapchain as
    // a storage image). The post-effect chain ping-pongs through pogo
    // and a tail blit copies the final pogo half into the swapchain;
    // keeping the surfel composite as a fragment pass means the same
    // COLOR_ATTACHMENT ↔ FRAGMENT_SHADER barrier helpers in RIPogoBuffer
    // cover the entire chain.
    LoadSlangGraphics(&RI.device, m_surfelGIRender, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "SurfelGIRenderPass.frag.spv", "vsMain", "psMain",
                      externalLayouts);
    // Tail blit: copies the post-effect chain's final pogo "read" half
    // into the swapchain image. Lives in HybridRenderer (not in the
    // composite) so it can target the swapchain directly. No bindless
    // needed — set 0 holds just sampler + Texture2D.
    LoadSlangGraphics(&RI.device, m_postEffectBlit, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_blit.frag.spv");
    {
      // Slang-compiled (amnesia/slang/ParticlePass) — entry-point names go in
      // the SPV as-is because slangc is invoked with -fvk-use-entrypoint-name.
      auto p_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.vert.spv");
      auto p_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, p_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, p_frag, "psMain"}};
      m_particle.initialize(&RI.device, stages, externalLayouts);
    }
    // Decal pass: no Slang port yet — legacy GLSL was wiped, dispatch
    // site below skips decals until the port lands.

    const VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // surfelValid→surfelDirty ping-pong copy
    m_diffuseObjectBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(UniformObject), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaquePositionHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage);
    m_opaqueTangentHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage);
    m_opaqueNormalHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage);
    m_opaqueUv0Handles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage);
    m_opaqueColorHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage);
    m_opaqueIndexHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage);

    RISegmentAllocDesc_s indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = (uint16_t)kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, indirectDesc.maxElements, sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // === SurfelGI SSBOs (bindless.resource.glsl set=0, bindings 10..19, 27..28) ===
    // Sizes come from forward_shared.h:
    //   surfelCounter         : kSurfelCounterSlotCount × uint32
    //   surfel/geometry/etc   : kTotalSurfelLimit × element
    //   surfelRayResult       : kRayBudget × SurfelRayResult (~460 MB)
    //   cellInfo / reservation: kCellCount × element  (15.6M cells × 8B/4B)
    //   cellToSurfel          : kCellToSurfelCapacity × uint32  (75 MB)
    // The reference layout adds m_surfelGeometryBuffer (cached uint4 triangle
    // hit per surfel) and m_surfelRayResultBuffer (replaces the old SurfelRay
    // type with a larger SurfelRayResult). m_cellCounterBuffer is gone — its
    // contents fold into surfelCounter[kSurfelCounterCell].
    // Lock host sizeof against the Slang ArrayStride for every SSBO struct the
    // host allocates by sizeof(). Mismatches here silently undersize the
    // bindless buffer; robust-access turns the out-of-bounds writes into no-ops
    // and reads return zeros — which manifested as Cell=0/ReqRay=0 because every
    // dirty surfel's data came back as zero.
    //
    // The shaders compile with `-fvk-use-scalar-layout` (cmake/shaders.cmake),
    // so the GPU strides are the natural/packed sizes (float3 = 12B, no 16B
    // struct rounding) and match the host's plain-C layout. These are NOT the
    // std430 values (which would be 128/32/64). Re-measure after any struct or
    // layout-flag change: `slangc … -target spirv-assembly | grep ArrayStride`.
    static_assert(sizeof(Surfel)            == 104, "Surfel host size != Slang scalar ArrayStride (104); re-check struct layout / -fvk-use-scalar-layout");
    static_assert(sizeof(SurfelBounds)      == 28,  "SurfelBounds host size != Slang scalar ArrayStride (28)");
    static_assert(sizeof(SurfelRayResult)   == 48,  "SurfelRayResult host size != Slang scalar ArrayStride (48)");
    static_assert(sizeof(CellInfo)          == 8,   "CellInfo host size != Slang scalar ArrayStride (8)");
    static_assert(sizeof(SurfelRecycleInfo) == 6,   "SurfelRecycleInfo host size != Slang scalar ArrayStride (6)");

    m_surfelCounterBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kSurfelCounterSlotCount, sizeof(uint32_t), kStorage);
    m_surfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(Surfel), kStorage);
    m_surfelGeometryBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t) * 4u, kStorage);
    m_surfelValidBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelDirtyIndexBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelFreeBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelRecycleBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(SurfelRecycleInfo), kStorage);
    m_surfelRayResultBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kRayBudget, sizeof(SurfelRayResult), kStorage);
    m_surfelRefCounterBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelReservationBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kCellCount, sizeof(uint32_t), kStorage);
    m_cellInfoBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kCellCount, sizeof(CellInfo), kStorage);
    m_cellToSurfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kCellToSurfelCapacity, sizeof(uint32_t), kStorage);
    // Compact cull record per surfel; written by collectCellInfo before the
    // generation pass reads it, so no defensive zeroing is needed (every index
    // pulled from a cell list has already passed through collect this frame).
    m_surfelBoundsBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(SurfelBounds), kStorage);

    // Slot-reuse generation buffers (see m_bindlessSlotGenerationBuffer). Both
    // start at 0; the host bumps a slot's generation to a unique nonzero value
    // the first time it's assigned (below in Draw), so a real surfel always
    // captures a matching value and 0 can never alias a live slot.
    m_bindlessSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(uint32_t), kStorage);
    m_surfelSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    std::memset(m_bindlessSlotGenerationBuffer.mappedAddress, 0,
                (size_t)kObjectSlotCapacity * sizeof(uint32_t));
    std::memset(m_surfelSlotGenerationBuffer.mappedAddress, 0,
                (size_t)kTotalSurfelLimit * sizeof(uint32_t));


    // Light SSBOs (point/spot/box). Unlike the bindless slot buffers above
    // these are device-local — the per-frame fill in Draw() stages through
    // RI.uploader rather than memcpy'ing into mapped memory the GPU may
    // still be reading from a prior frame.
    {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN,
                                      queueFamilies, RI_QUEUE_LEN);
      bci.usage =
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO;

      bci.size = (VkDeviceSize)kPointSlotLightCapacity * sizeof(PointLight);
      VK_WrapResult(vmaCreateBuffer(
          RI.device.vk.vmaAllocator, &bci, &aci, &m_pointLightBuffer.vk.buffer,
          &m_pointLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)kSpotSlotLightCapacity * sizeof(SpotLight);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_spotLightBuffer.vk.buffer,
                                    &m_spotLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)kBoxSlotLightCapacity * sizeof(BoxLight);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_boxLightBuffer.vk.buffer,
                                    &m_boxLightBuffer.vk.allocation, nullptr));
    }

    // Surfel-generation output image — one storage texture per swapchain
    // image. RGBA16F so HDR radiance survives; SAMPLED so a future
    // composite pass can read it back. View aspect color, full mip 0.
    // Sized at FULL swapchain resolution: surfel_generation_pass dispatches
    // at imageRes = viewportSize and writes every pixel, so every texel of
    // this image is initialised each frame. visibility_shade.frag samples
    // uv01 across the full [0,1] range and reads valid radiance.
    m_surfelResultWidth  = RI.swapchain.width;
    m_surfelResultHeight = RI.swapchain.height;
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      imgInfo.extent = {m_surfelResultWidth, m_surfelResultHeight, 1};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      // TRANSFER_DST is needed for the per-frame vkCmdClearColorImage in
      // Draw() — the generation pass only writes pixels with surfel
      // contribution, so the rest must be explicitly zeroed each frame.
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_surfelResultTexture[i].vk.image,
                                   &m_surfelResultTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_surfelResultTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_surfelResultView[i].vk.image));
    }

    // Stage B packed visibility — RGBA32UI storage image written by the
    // surfel_vbuffer RT pipeline and sampled by Stage D / F passes. Same
    // swapchain-sized footprint as m_surfelResultTexture.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R32G32B32A32_UINT;
      imgInfo.extent = {m_surfelResultWidth, m_surfelResultHeight, 1};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_packedHitInfoTexture[i].vk.image,
                                   &m_packedHitInfoTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_packedHitInfoTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R32G32B32A32_UINT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_packedHitInfoView[i].vk.image));
    }

    // Surfel-ray irradiance atlas — single-channel R16F, 4096x4096 fits
    // kTotalSurfelLimit surfels at 6x6 cells each (the shader computes
    // tile pos as `surfelIndex % (W/6), surfelIndex / (W/6)`). SAMPLED so the
    // raytrace shader's ray-guiding branch can read it; STORAGE so a future
    // accumulation pass can write into it. Untouched here — stays at the
    // SHADER_READ_ONLY_OPTIMAL layout after the first transition below.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16_SFLOAT;
      imgInfo.extent = {4096u, 4096u, 1u};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_surfelIrradianceTexture[i].vk.image,
                                   &m_surfelIrradianceTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_surfelIrradianceTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_surfelIrradianceView[i].vk.image));
    }

    // Surfel-ray depth atlas — RGBA16F storing the (E[z], E[z^2]) Chebyshev
    // pair per octahedral tile texel for each surfel (only .rg are populated;
    // .ba left zero). Same 4096x4096 footprint as the irradiance atlas so the
    // integrate shader's tile-index math (surfelIndex % (W/6), surfelIndex /
    // (W/6)) resolves identically for both atlases. STORAGE for integrate
    // writes, SAMPLED for integrate's own readback.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      imgInfo.extent = {4096u, 4096u, 1u};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_surfelDepthTexture[i].vk.image,
                                   &m_surfelDepthTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_surfelDepthTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_surfelDepthView[i].vk.image));
    }

    m_materialBindless.reset(kMaterialSlotCapacity);
    m_opaqueMaterialBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kMaterialSlotCapacity, sizeof(DiffuseMaterial), kStorage,
        /*deviceLocalOnly*/ true);

    // Default linear/wrap sampler for all bindless texture fetches. The
    // engine's filter cache (RIBootstrap::resolve_filter_descriptor) hands
    // back a finalized RIDescriptor_s with a non-zero cookie, which is
    // exactly what bindDescriptors needs.
    m_materialSampler = RI.resolve_filter_descriptor(
        eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
        eTextureFilter_Trilinear);

    {
      const VkDeviceSize kOpaqueHandleRange =
          kObjectSlotCapacity * sizeof(VkDeviceAddress);
      const struct {
        uint32_t binding;
        RIBuffer_s *buffer;
        VkDeviceSize range;
      } ssbos[] = {
          {kBindingOpaquePositionHandles, &m_opaquePositionHandles,
           kOpaqueHandleRange},
          {kBindingOpaqueTangentHandles, &m_opaqueTangentHandles,
           kOpaqueHandleRange},
          {kBindingOpaqueNormalHandles, &m_opaqueNormalHandles,
           kOpaqueHandleRange},
          {kBindingOpaqueUv0Handles, &m_opaqueUv0Handles, kOpaqueHandleRange},
          {kBindingOpaqueColorHandles, &m_opaqueColorHandles,
           kOpaqueHandleRange},
          {kBindingOpaqueIndexHandles, &m_opaqueIndexHandles,
           kOpaqueHandleRange},
          {kBindingSurfelCounter, &m_surfelCounterBuffer,
           kSurfelCounterSlotCount * sizeof(uint32_t)},
          {kBindingSurfelBuffer, &m_surfelBuffer,
           kTotalSurfelLimit * sizeof(Surfel)},
          {kBindingSurfelGeometry, &m_surfelGeometryBuffer,
           kTotalSurfelLimit * sizeof(uint32_t) * 4u},
          {kBindingSurfelValidIndex, &m_surfelValidBuffer,
           kTotalSurfelLimit * sizeof(uint32_t)},
          {kBindingSurfelDirtyIndex, &m_surfelDirtyIndexBuffer,
           kTotalSurfelLimit * sizeof(uint32_t)},
          {kBindingSurfelFreeIndex, &m_surfelFreeBuffer,
           kTotalSurfelLimit * sizeof(uint32_t)},
          {kBindingSurfelRecycle, &m_surfelRecycleBuffer,
           kTotalSurfelLimit * sizeof(SurfelRecycleInfo)},
          {kBindingSurfelRayResult, &m_surfelRayResultBuffer,
           kRayBudget * sizeof(SurfelRayResult)},
          {kBindingCellInfo, &m_cellInfoBuffer,
           kCellCount * sizeof(CellInfo)},
          {kBindingCellToSurfel, &m_cellToSurfelBuffer,
           kCellToSurfelCapacity * sizeof(uint32_t)},
          {kBindingSurfelRefCounter, &m_surfelRefCounterBuffer,
           kTotalSurfelLimit * sizeof(uint32_t)},
          {kBindingSurfelReservation, &m_surfelReservationBuffer,
           kCellCount * sizeof(uint32_t)},
          {kBindingSurfelBounds, &m_surfelBoundsBuffer,
           kTotalSurfelLimit * sizeof(SurfelBounds)},
          {kBindingBindlessSlotGeneration, &m_bindlessSlotGenerationBuffer,
           kObjectSlotCapacity * sizeof(uint32_t)},
          {kBindingSurfelSlotGeneration, &m_surfelSlotGenerationBuffer,
           kTotalSurfelLimit * sizeof(uint32_t)},
          {kBindingSceneObjects, &m_diffuseObjectBuffer,
           kObjectSlotCapacity * sizeof(UniformObject)},
          {kBindingOpaqueMaterial, &m_opaqueMaterialBuffer,
           kMaterialSlotCapacity * sizeof(DiffuseMaterial)},
          {kBindingPointLights, &m_pointLightBuffer,
           kPointSlotLightCapacity * sizeof(PointLight)},
          {kBindingSpotLights, &m_spotLightBuffer,
           kSpotSlotLightCapacity * sizeof(SpotLight)},
          {kBindingBoxLights, &m_boxLightBuffer,
           kBoxSlotLightCapacity * sizeof(BoxLight)},
      };

      RIBindlessDescriptorSet::WriteBinding writes[std::size(ssbos) + 2] = {};
      size_t count = 0;
      for (uint32_t i = 0; i < std::size(ssbos); ++i) {
        writes[count].binding = ssbos[i].binding;
        writes[count].arrayElement = 0;
        writes[count].descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[count].descriptor.vk.buffer.buffer = ssbos[i].buffer->vk.buffer;
        writes[count].descriptor.vk.buffer.offset = 0;
        writes[count].descriptor.vk.buffer.range = ssbos[i].range;
        count++;
      }
      writes[count].binding = kBindingMaterialSampler;
      writes[count].arrayElement = 0;
      writes[count].descriptor = *m_materialSampler;
      count++;
      // gSurfelDepthSampler — immutable bilinear clamp sampler, written
      // once at init time. The image views it samples (kBindingSurfelDepthSampled
      // / kBindingSurfelDepth) live on set 1 and are pushed per-dispatch.
      {
        RIDescriptor_s *surfelDepthDesc = RI.resolve_filter_descriptor(
            eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
            eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
        writes[count].binding = kBindingSurfelDepthSampler;
        writes[count].arrayElement = 0;
        writes[count].descriptor = *surfelDepthDesc;
        count++;
      }
      m_bindlessSet.writeDescriptors(&RI.device,
                                     std::span(writes).subspan(0, count));
    }
  }
}

uint32_t cHybridRenderer::resolveTextureSlot(RIBootstrap::FrameContext *cntx,
                                             Image *img, uint32_t frameIndex) {
  if (!img)
    return kInvalidTextureIndex;
  auto texture = img->GetTexture();
  if (!texture)
    return kInvalidTextureIndex;
  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)texture.get());
  auto req = m_textureBindless.request(texture_cookie, frameIndex);
  if (req.exhausted)
    return kInvalidTextureIndex;
  cntx->textureLink.push_back(texture);
  // BindlessPool reports `found == true` only when the same cookie still
  // owns the slot. Fresh allocations and LRU recycles both come back with
  // `found == false`, so that's when we (re)stage the descriptor write at
  // textures_2d[req.id] (set 0, binding 0).
  if (!req.found) {
    RIBindlessDescriptorSet::WriteBinding binding = {};
    binding.binding = 0;
    binding.arrayElement = req.id;
    binding.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptor.vk.image.sampler = VK_NULL_HANDLE;
    binding.descriptor.vk.image.imageView = texture->binding.vk.image.imageView;
    binding.descriptor.vk.image.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_bindlessSet.writeDescriptors(&RI.device, {&binding, 1});
  }
  return req.id;
}

uint32_t cHybridRenderer::resolveCubeTextureSlot(
    RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex) {
  if (!img)
    return kInvalidTextureIndex;
  auto texture = img->GetTexture();
  if (!texture)
    return kInvalidTextureIndex;

  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)texture.get());
  auto req = m_textureCubeBindless.request(texture_cookie, frameIndex);
  if (req.exhausted)
    return kInvalidTextureIndex;
  cntx->textureLink.push_back(texture);
  if (!req.found) {
    RIBindlessDescriptorSet::WriteBinding binding = {};
    binding.binding = kBindingTexturesCube;
    binding.arrayElement = req.id;
    binding.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptor.vk.image.sampler = VK_NULL_HANDLE;
    binding.descriptor.vk.image.imageView = texture->binding.vk.image.imageView;
    binding.descriptor.vk.image.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_bindlessSet.writeDescriptors(&RI.device, {&binding, 1});
  }
  return req.id;
}

uint32_t cHybridRenderer::resolveMaterial(RIBootstrap::FrameContext *cntx,
                                          cMaterial *mat, uint32_t frameIndex) {
  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    return resolveTextureSlot(cntx, mat->GetImage(type), frameIndex);
  };

  // Slot layout must match the DiffuseMaterial_*Texture_ID accessors in
  // amnesia/glsl/per_frame.resource.glsl. One uint32 per texture index.
  DiffuseMaterial gpu = {};
  gpu.type = MATERIAL_TYPE_DIFFUSE;
  gpu.tex[0] = slotFor(eMaterialTexture_Diffuse);
  gpu.tex[1] = slotFor(eMaterialTexture_NMap);
  gpu.tex[2] = slotFor(eMaterialTexture_Alpha);
  gpu.tex[3] = slotFor(eMaterialTexture_Specular);
  gpu.tex[4] = slotFor(eMaterialTexture_Height);
  gpu.tex[5] = slotFor(eMaterialTexture_Illumination);
  gpu.tex[6] = slotFor(eMaterialTexture_DissolveAlpha);
  gpu.tex[7] = slotFor(eMaterialTexture_CubeMapAlpha);
  // Reflection cube map — separate bindless table (textures_cube[]), so
  // resolve via the cube allocator rather than slotFor (which only handles
  // 2D). Lives outside tex[] in the GPU struct to mirror the legacy
  // shader-global cubeMap.
  gpu.cubeMapTextureIndex = resolveCubeTextureSlot(
      cntx, mat->GetImage(eMaterialTexture_CubeMap), frameIndex);
  // Material config bits — single source of truth lives in MaterialResource.
  // The visibility composite reads bit 9 (IsHeightMapSingleChannel) to pick
  // .r vs .a when sampling the heightmap. Without this assignment the field
  // sat at zero and parallax always read .a (constant 1.0 for typical single-
  // channel Amnesia heightmaps), running the full POM loop every fragment and
  // producing severe warping at grazing angles on vertical walls.
  gpu.materialConfig = material::UniformMaterialBlock::CreateMaterailConfigFlags(*mat);
  // Scalars: only the solid path is mapped today. Other variants leave
  // these zero (the forward-diffuse fragment shader doesn't read them on
  // the solid path either).
  const ShaderMaterialData &desc = mat->Descriptor();
  if (desc.m_id == MaterialID::SolidDiffuse) {
    gpu.heightMapScale = desc.m_solid.m_heightMapScale;
    gpu.heightMapBias = desc.m_solid.m_heightMapBias;
    gpu.frenselBias = desc.m_solid.m_frenselBias;
    gpu.frenselPow = desc.m_solid.m_frenselPow;
  }

  hash_t cookie = hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)mat);
  cookie = hash_u64(cookie, (uint64_t)mat->Generation());
  cookie = hash_data(cookie, &gpu, sizeof(gpu));
  auto req = m_materialBindless.request(cookie, frameIndex);
  if (req.exhausted)
    return UINT32_MAX;
  if (req.found)
    return req.id;
  {
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_opaqueMaterialBuffer;
    trans.size = sizeof(DiffuseMaterial);
    trans.offset = (size_t)req.id * sizeof(DiffuseMaterial);
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = trans.vk.current_stage;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, &gpu, sizeof(gpu));
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  return req.id;
}

void cHybridRenderer::Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

  // SurfelGI debug-stats readback. The counter buffer is host-mapped (made
  // via detail::CreateBindlessSlotBuffer); the GPU writes to it from the
  // surfel passes below and we read whatever value the previous frame's
  // dispatches left there. There's no explicit fence — the in-flight-frame
  // count is small enough that the lag is just a frame or two, which is
  // fine for human-eyeball debugging. Log accumulates per-second sums and
  // dumps via the standard engine Log() once each interval rolls over.
  {
    static double sStatsAccum[kSurfelCounterSlotCount] = {0};
    static uint32_t sStatsFrameCount = 0;
    static float    sStatsTimer = 0.0f;
    if (m_surfelCounterBuffer.mappedAddress) {
      const auto *counters = static_cast<const uint32_t *>(
          m_surfelCounterBuffer.mappedAddress);
      for (uint32_t i = 0; i < kSurfelCounterSlotCount; ++i) {
        sStatsAccum[i] += double(counters[i]);
      }
      sStatsFrameCount++;
      sStatsTimer += afFrameTime;
      if (sStatsTimer >= 1.0f && sStatsFrameCount > 0) {
        const double n = double(sStatsFrameCount);
        Log("[SurfelGI] frames=%u  Valid=%.0f Dirty=%.0f Free=%.0f Cell=%.0f "
            "ReqRay=%.0f MissBnc=%.0f  "
            "DbgAlive=%.0f DbgRadPos=%.0f DbgLifePos=%.0f "
            "DbgCellHits=%.0f DbgCellInvalid=%.0f  "
            "ScatHit=%.0f ScatMiss=%.0f NeeNZ=%.0f FinHit=%.0f  "
            "GenSurfMax(avg)=%.0f\n",
            sStatsFrameCount,
            sStatsAccum[kSurfelCounterValid] / n,
            sStatsAccum[kSurfelCounterDirty] / n,
            sStatsAccum[kSurfelCounterFree] / n,
            sStatsAccum[kSurfelCounterCell] / n,
            sStatsAccum[kSurfelCounterRequestedRay] / n,
            sStatsAccum[kSurfelCounterMissBounce] / n,
            sStatsAccum[kSurfelCounterDbgAlive] / n,
            sStatsAccum[kSurfelCounterDbgRadiusPos] / n,
            sStatsAccum[kSurfelCounterDbgLifePos] / n,
            sStatsAccum[kSurfelCounterDbgCellHits] / n,
            sStatsAccum[kSurfelCounterDbgCellInvalid] / n,
            sStatsAccum[kSurfelCounterDbgScatHit] / n,
            sStatsAccum[kSurfelCounterDbgScatMiss] / n,
            sStatsAccum[kSurfelCounterDbgScatNeeNonZero] / n,
            sStatsAccum[kSurfelCounterDbgFinalizeHit] / n,
            sStatsAccum[kSurfelCounterDbgGenSurfelMax] / n);
        for (uint32_t i = 0; i < kSurfelCounterSlotCount; ++i)
          sStatsAccum[i] = 0;
        sStatsFrameCount = 0;
        sStatsTimer = 0.0f;
      }
    }
  }

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  const ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
  ml::float4x4 mainFrustumProjInvMat = mainFrustumProjMat;
  mainFrustumProjInvMat.Invert();
  {
    m_rendererList.BeginAndReset(afFrameTime, apFrustum);
    auto *dynamicContainer =
        apWorld->GetRenderableContainer(eWorldContainerType_Dynamic);
    auto *staticContainer =
        apWorld->GetRenderableContainer(eWorldContainerType_Static);
    dynamicContainer->UpdateBeforeRendering();
    staticContainer->UpdateBeforeRendering();

    auto prepareObjectHandler = [&](iRenderable *pObject) {
      if (!rendering::IsObjectIsVisible(
              pObject, eRenderableFlag_VisibleInNonReflection, {})) {
        return;
      }
      m_rendererList.AddObject(pObject);
    };
    rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection);
    rendering::WalkAndPrepareRenderList(staticContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection);
    m_rendererList.End(
        eRenderListCompileFlag_Diffuse | eRenderListCompileFlag_Translucent |
        eRenderListCompileFlag_Decal | eRenderListCompileFlag_Illumination |
        eRenderListCompileFlag_FogArea);

    // Park every BLAS-backed geometry on this frame's context, unfiltered by
    // frustum/visibility culling. The TLAS (m_tlas) is persistent and only
    // rebuilt on frames that gather visible instances (see "TLAS build"
    // below, guarded by `!tlasInstances.empty()`); a stale m_tlas keeps
    // referencing the BLAS device addresses of geometry that was freed on a
    // map transition, and the surfel RT passes trace it every frame -> a
    // dangling acceleration-structure / vertex-buffer dereference -> GPUVM
    // read fault -> device lost. AttachResourceToCntx pushes the BLAS handle,
    // its storage, and the vertex/index buffers onto accelLink/bufferLink,
    // which defer release by frames-in-flight, so any BLAS the TLAS can still
    // reference outlives the in-flight window even after its owning renderable
    // is destroyed. Only geometry with a built BLAS is parked (the rest can't
    // be in the TLAS).
    std::function<void(iRenderableContainerNode *)> retainGeometryBlas =
        [&](iRenderableContainerNode *node) {
          node->UpdateBeforeUse();
          for (auto *child : node->GetChildNodes())
            retainGeometryBlas(child);
          for (auto *pObject : node->GetObjects()) {
            auto *pVB =
                static_cast<VertexBuffer_RI *>(pObject->GetVertexBuffer());
            if (pVB && pVB->accelStructure())
              pVB->AttachResourceToCntx(cntx);
          }
        };
    retainGeometryBlas(dynamicContainer->GetRoot());
    retainGeometryBlas(staticContainer->GetRoot());
  }

  SceneConstants perFrame{};
  std::memcpy(perFrame.viewMat, mainFrustumViewMat.a, sizeof(perFrame.viewMat));
  std::memcpy(perFrame.invViewMat, mainFrustumViewInvMat.a,
              sizeof(perFrame.invViewMat));
  std::memcpy(perFrame.projMat, mainFrustumProjMat.a, sizeof(perFrame.projMat));
  std::memcpy(perFrame.invProjMat, mainFrustumProjInvMat.a,
              sizeof(perFrame.invProjMat));
  // viewProjMat = proj * view (column-major); fill via direct ml composition
  // when needed. Leaving as identity-stub for now — first pass writes only
  // visibility; lighting in the FS reads viewMat/invViewMat which are correct.
  perFrame.viewportSize[0] = (float)RI.swapchain.width;
  perFrame.viewportSize[1] = (float)RI.swapchain.height;
  perFrame.viewTexel[0] =
      RI.swapchain.width ? 1.0f / (float)RI.swapchain.width : 0.0f;
  perFrame.viewTexel[1] =
      RI.swapchain.height ? 1.0f / (float)RI.swapchain.height : 0.0f;
  perFrame.afT = afFrameTime;
  perFrame.totalFrames = RI.frameIndex;
  perFrame.cameraFov = apFrustum->GetFOV();
  perFrame.fireflyClampThreshold = 10.0f;
  perFrame.zNear = apFrustum->GetNearPlane();
  perFrame.zFar = apFrustum->GetFarPlane();
  // Fog params + worldFogColor + invViewRotationMat default to zero — fine for
  // the first pass; populate when the deferred-fog path needs them.

  // Falcor-style pinhole camera basis. mainFrustumViewInvMat memory is laid
  // out so each 4-float "row" corresponds to one column of the logical
  // (column-vector math) view-inverse — that is, the camera's world-space
  // basis vectors (right, up, back, origin). Slang's -matrix-layout-column-
  // major flag flips the interpretation back to column-vector math GPU-side,
  // so the offsets line up with what the shader expects.
  {
    const float *invV = mainFrustumViewInvMat.a;
    const hpl::float3 rightW{invV[0], invV[1], invV[2]};
    const hpl::float3 upW{invV[4], invV[5], invV[6]};
    const hpl::float3 backW{invV[8], invV[9], invV[10]};
    const hpl::float3 posW{invV[12], invV[13], invV[14]};

    const float aspect = apFrustum->GetAspect();
    const float tanHalfFov = std::tan(0.5f * apFrustum->GetFOV());
    constexpr float focalLength = 1.0f;
    const float uScale = focalLength * tanHalfFov * aspect;
    const float vScale = focalLength * tanHalfFov;

    perFrame.posW = posW;
    perFrame.cameraU = {uScale * rightW.x, uScale * rightW.y, uScale * rightW.z};
    perFrame.cameraV = {vScale * upW.x, vScale * upW.y, vScale * upW.z};
    // cameraW points from the camera through the image-plane center =
    // focalLength * forward. The view-inverse stores back (negative forward)
    // in column 2, so negate.
    perFrame.cameraW = {-focalLength * backW.x, -focalLength * backW.y,
                        -focalLength * backW.z};
    perFrame.jitterX = 0.0f; // TAA not yet wired up; computeRayPinhole's
    perFrame.jitterY = 0.0f; // applyJitter=true is a no-op while these are 0.
  }

  auto solids = m_rendererList.GetSolidObjects();
  auto lights = m_rendererList.GetLights();
  RISegmentReq_s indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(RI.frameIndex, solids.size(), &indirectReq);
  assert(indirectOk);
  auto *indirectDst = reinterpret_cast<VkDrawIndirectCommand *>(
      static_cast<uint8_t *>(m_indirectDrawBuffer.mappedAddress) +
      (size_t)indirectReq.elementOffset * sizeof(VkDrawIndirectCommand));
  uint32_t writtenDraws = 0;

  // TLAS instance accumulator. Sized for the shadow caster set (every shadow
  // caster contributes at most one TLAS instance).
  std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
  tlasInstances.reserve(solids.size());

  size_t num_point_lights = 0;
  for (iLight *pLight : lights) {
    if (pLight->GetLightType() != eLightType_Point)
      continue;
    if (num_point_lights >= kPointSlotLightCapacity) {
      Warning("Point-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    PointLight pl{};
    pl.type = LIGHT_TYPE_POINT;
    const cVector3f pos = pLight->GetWorldPosition();
    pl.position[0] = pos.x;
    pl.position[1] = pos.y;
    pl.position[2] = pos.z;
    pl.radius = pLight->GetRadius();
    const cColor c = pLight->GetDiffuseColor();
    pl.color[0] = c.r;
    pl.color[1] = c.g;
    pl.color[2] = c.b;
    pl.intensity = c.a;
    pl.attenuationTextureIndex = resolveTextureSlot(
        cntx, pLight->GetFalloffImage(), (uint32_t)RI.frameIndex);
    pl.goboTextureIndex = resolveCubeTextureSlot(
        cntx, pLight->GetGoboImage(), (uint32_t)RI.frameIndex);
    const cMatrixf &world = pLight->GetWorldMatrix();
    pl.worldToLightX[0] = world.m[0][0];
    pl.worldToLightX[1] = world.m[0][1];
    pl.worldToLightX[2] = world.m[0][2];
    pl.worldToLightY[0] = world.m[1][0];
    pl.worldToLightY[1] = world.m[1][1];
    pl.worldToLightY[2] = world.m[1][2];
    pl.worldToLightZ[0] = world.m[2][0];
    pl.worldToLightZ[1] = world.m[2][1];
    pl.worldToLightZ[2] = world.m[2][2];
    m_pointLightScratch[num_point_lights++] = pl;
  }
  perFrame.pointLightCount = static_cast<uint32_t>(num_point_lights);

  if (num_point_lights > 0) {
    const size_t uploadBytes = num_point_lights * sizeof(PointLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_pointLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    // After the first frame the buffer was previously read as a storage
    // resource; tell the uploader to barrier from that to TRANSFER_WRITE
    // and back. On the very first frame the buffer is uninitialised, so
    // the src side of the barrier is a no-op against zero contents — safe.
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_pointLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Spot lights. Same upload envelope as point lights — RI.uploader owns the
  // TRANSFER_WRITE ↔ SHADER_READ barrier, so no extra barrier needed here.
  size_t num_spot_lights = 0;
  for (iLight *pLight : lights) {
    if (pLight->GetLightType() != eLightType_Spot)
      continue;
    if (num_spot_lights >= kSpotSlotLightCapacity) {
      Warning("Spot-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    cLightSpot *pSpot = static_cast<cLightSpot *>(pLight);
    SpotLight sl{};
    sl.type = LIGHT_TYPE_SPOT;
    const cVector3f pos = pLight->GetWorldPosition();
    sl.position[0] = pos.x;
    sl.position[1] = pos.y;
    sl.position[2] = pos.z;
    sl.radius = pLight->GetRadius();
    // HPL2 lights shine down -Z in world space. World matrix is column-major
    // with the 3rd column = +Z basis; negate it to get the outward forward.
    const cMatrixf &world = pLight->GetWorldMatrix();
    sl.direction[0] = -world.m[0][2];
    sl.direction[1] = -world.m[1][2];
    sl.direction[2] = -world.m[2][2];
    // Normalize defensively — the spot may carry non-unit scale.
    {
      float len = std::sqrt(sl.direction[0] * sl.direction[0] +
                            sl.direction[1] * sl.direction[1] +
                            sl.direction[2] * sl.direction[2]);
      if (len > 1e-6f) {
        sl.direction[0] /= len;
        sl.direction[1] /= len;
        sl.direction[2] /= len;
      }
    }
    sl.cosOuterAngle = std::cos(pSpot->GetFOV() * 0.5f);
    const cColor c = pLight->GetDiffuseColor();
    // Linear-throughout pipeline: see point-light upload above for rationale.
    sl.color[0] = c.r;
    sl.color[1] = c.g;
    sl.color[2] = c.b;
    sl.intensity = c.a;
    // Legacy two-LUT spotlight falloff (deferred_light_spotlight.frag.fsl):
    //   - GetFalloffImage() = 1D radial attenuation, keyed on (d/r)² in shader
    //   - GetSpotFalloffImage() = 1D cone falloff, keyed on (1-cosT)/(1-cosOA)
    // Either may be missing (kInvalidTextureIndex), in which case the shader
    // falls back to saturate(1-(d/r)²) / smoothstep respectively.
    sl.attenuationTextureIndex = resolveTextureSlot(
        cntx, pLight->GetFalloffImage(), (uint32_t)RI.frameIndex);
    sl.goboTextureIndex = resolveTextureSlot(cntx, pLight->GetGoboImage(),
                                             (uint32_t)RI.frameIndex);
    sl.coneFalloffTextureIndex = resolveTextureSlot(
        cntx, pSpot->GetSpotFalloffImage(), (uint32_t)RI.frameIndex);
    sl.shadowEnabled = pLight->GetCastShadows() ? 1u : 0u;
    // Light-space ViewProj for projecting gobo UVs (and any future shadow UV)
    // into the cone. Transposed to match the GLSL mat4 column-major upload.
    const ml::float4x4 vpF4 =
        cMath::ToFloatTranspose4x4(pSpot->GetViewProjMatrix());
    std::memcpy(sl.viewProjection, vpF4.a, sizeof(sl.viewProjection));
    m_spotLightScratch[num_spot_lights++] = sl;
  }
  perFrame.spotLightCount = static_cast<uint32_t>(num_spot_lights);

  if (num_spot_lights > 0) {
    const size_t uploadBytes = num_spot_lights * sizeof(SpotLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_spotLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_spotLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Box lights. Add-blend only (eLightBoxBlendFunc_Replace is silently
  // treated as Add for now — visibility_shade.frag has no Replace path).
  size_t num_box_lights = 0;
  for (iLight *pLight : lights) {
    if (pLight->GetLightType() != eLightType_Box)
      continue;
    if (num_box_lights >= kBoxSlotLightCapacity) {
      Warning("Box-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    cLightBox *pBox = static_cast<cLightBox *>(pLight);
    BoxLight bl{};
    bl.type = LIGHT_TYPE_BOX;
    bl.blendFunc = (pBox->GetBlendFunc() == eLightBoxBlendFunc_Replace) ? 0u : 1u;
    // Matches RendererDeferred's box-light proxy: AABB at the light's world position, 
    // now correctly supporting entity rotation using a world-to-local rotation matrix.
    const cVector3f center = pLight->GetWorldPosition();
    bl.center[0] = center.x;
    bl.center[1] = center.y;
    bl.center[2] = center.z;
    const cVector3f half = pBox->GetSize() * 0.5f;
    bl.halfSize[0] = half.x;
    bl.halfSize[1] = half.y;
    bl.halfSize[2] = half.z;
    // Legacy `deferred_light_box.frag.fsl` discards alpha entirely, so
    // GetDiffuseColor().a is intentionally not uploaded — artist-authored
    // brightness lives in the rgb channels. Box lights are an additive
    // volume tint (no Lambert BRDF), so they only get sRGB→linear with no
    // π compensation — that's only for the point/spot direct path.
    const cColor c = pLight->GetDiffuseColor();
    bl.color[0] = detail::sRGBToLinear(c.r);
    bl.color[1] = detail::sRGBToLinear(c.g);
    bl.color[2] = detail::sRGBToLinear(c.b);

    const cMatrixf &world = pLight->GetWorldMatrix();
    bl.worldToLightX[0] = world.m[0][0];
    bl.worldToLightX[1] = world.m[0][1];
    bl.worldToLightX[2] = world.m[0][2];
    bl.worldToLightY[0] = world.m[1][0];
    bl.worldToLightY[1] = world.m[1][1];
    bl.worldToLightY[2] = world.m[1][2];
    bl.worldToLightZ[0] = world.m[2][0];
    bl.worldToLightZ[1] = world.m[2][1];
    bl.worldToLightZ[2] = world.m[2][2];

    m_boxLightScratch[num_box_lights++] = bl;
  }
  perFrame.boxLightCount = static_cast<uint32_t>(num_box_lights);

  if (num_box_lights > 0) {
    const size_t uploadBytes = num_box_lights * sizeof(BoxLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_boxLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_boxLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Retire bindless object slots whose vertex buffers were freed since last
  // frame (VertexBuffer_RI dtor -> m_onDestroyed -> m_retiredGiSlots). Stamp
  // their stream handles with the retired sentinel HERE, before the per-draw
  // fan-out below, so a slot reused by a live object this frame still gets its
  // fresh BDA written and wins. SurfelUpdatePass::collectCellInfo recycles any
  // surfel whose cached slot reads the sentinel — so a dangling cached hit can
  // never deref a freed buffer-device-address (GPUVM fault). The sentinel (not
  // 0) keeps a legitimately-zero handle distinguishable from a retired one.
  // Replaces the old per-surfel SurfelClearByInstance scan.
  if (!m_retiredGiSlots.empty()) {
    RIBuffer_s *const retiredHandleBuffers[] = {
        &m_opaquePositionHandles, &m_opaqueTangentHandles,
        &m_opaqueNormalHandles,   &m_opaqueUv0Handles,
        &m_opaqueColorHandles,    &m_opaqueIndexHandles,
    };
    for (uint32_t slot : m_retiredGiSlots)
      for (RIBuffer_s *hb : retiredHandleBuffers)
        reinterpret_cast<VkDeviceAddress *>(hb->mappedAddress)[slot] =
            HPL_RETIRED_GEOMETRY_HANDLE;
    m_retiredGiSlots.clear();
  }

  for (iRenderable *pObject : solids) {
    cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
    iVertexBuffer *pVB = pObject->GetVertexBuffer();
    cMaterial *pMat = pObject->GetMaterial();
    if (!pVB || !pMat)
      continue;

    uint32_t materialSlot = 0;
    if (pMat) {
      materialSlot = resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
      if (materialSlot == UINT32_MAX) {
        Warning("Material Slot exhausted");
        materialSlot = 0;
      }
    }

    UniformObject payload{};
    payload.dissolveAmount = pObject->GetCoverageAmount();
    payload.materialID = materialSlot;
    payload.lightLevel = 1.0f;
    payload.illuminationAmount = pObject->GetIlluminationAmount();
    const ml::float4x4 modelF4 =
        cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    const ml::float4x4 uvF4 = cMath::ToFloatTranspose4x4(cMatrixf::Identity);
    std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

    const hash_t payloadHash =
        hash_data(hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)pObject),
                  &payload, sizeof(payload));
    auto req = m_diffuseBindless.request(payloadHash, (uint32_t)RI.frameIndex);
    if (req.exhausted) {
      // TODO: will probably resize the buffer and goto the beginning and
      // reconstruct the data
      Error("bindless pool is exhausted");
      // Drop this draw rather than writing through a sentinel req.id — the
      // downstream payload / handle writes index the bindless buffer at
      // req.id and feed instanceCustomIndex into the TLAS instance.
      continue;
    }

    auto *vb = static_cast<VertexBuffer_RI *>(pVB);
    vb->SubmitToGPU(&RI.primary.cmds[0], &RI.device, cntx);
    // The first time this bindless slot is (re)assigned to this VB
    // (req.found == false), park a destroy handler in the slot's cache state.
    // On the VB's destruction it retires this slot; the clear pass then purges
    // surfels whose cached hit's instanceID == slot before their freed vertex
    // BDA is dereferenced. Capturing `this` is safe: the handler lives in
    // m_diffuseBindless (a renderer member), so it can't fire after the renderer
    // (and m_retiredGiSlots) are gone. On a cache hit the handler is already
    // bound to this VB; on eviction the cache reset the slot's state for us.
    if (!req.found && req.state) {
      *req.state = EventHandler<>(
          [this, slot = req.id]() { m_retiredGiSlots.push_back(slot); });
      req.state->Connect(vb->OnDestroyed());
    }
    if (!req.found) {
      // This slot now hosts a different object (fresh allocation or
      // eviction-reuse). Bump its generation so any surfel still anchored to it
      // — carrying a cached primitiveIndex from the *previous* occupant's mesh —
      // is detected as stale by collectCellInfo before it dereferences the new
      // geometry's vertex/index BDA (the GPUVM-fault path). A unique monotonic
      // value lets us write the slot without reading back write-combined memory.
      static_cast<uint32_t *>(m_bindlessSlotGenerationBuffer.mappedAddress)[req.id] =
          ++m_nextSlotGeneration;
      {
        RIResourceBufferTransaction_s trans = {};
        trans.target = m_diffuseObjectBuffer;
        trans.size = sizeof(UniformObject);
        trans.offset = (size_t)req.id * sizeof(UniformObject);
        trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        trans.vk.post_stage = trans.vk.current_stage;
        trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
        std::memcpy(trans.mapped.data, &payload, sizeof(payload));
        RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
      }
    }

    // Per-stream VkDeviceAddress fan-out into the parallel handle buffers.
    // Missing streams write 0 — shaders branch on non-zero before deref.
    //
    // Rewritten EVERY frame, not just on cache miss: the diffuse cache key is
    // pObject + transform (see m_diffuseBindless.request above), which does NOT
    // capture the VB's device address. SubmitToGPU hands back a brand-new
    // VkBuffer (new device address) on first submit, shadow-data growth, or the
    // CreateCopy sentinel (VertexBuffer_RI.cpp). On a cache *hit* after such a
    // realloc, a once-only write would leave this slot pointing at the freed
    // address; gbuffer.vert / the surfel RT vbuffer chit then deref a non-null
    // dangling pointer -> GPUVM read fault -> device lost. The particle path
    // dodges this by folding RI.frameIndex into its key; opaque draws share a
    // slot across frames, so the handle itself must be refreshed here.
    auto bdaOf = [&](eVertexBufferElement type) -> VkDeviceAddress {
      const auto *element = vb->GetElement(type);
      if (!element || !element->buffer)
        return 0;
      return element->buffer->GetDeviceHandle(&RI.device);
    };

    const VkDeviceAddress addrs[] = {
        bdaOf(eVertexBufferElement_Position),
        bdaOf(eVertexBufferElement_Texture1Tangent),
        bdaOf(eVertexBufferElement_Normal),
        bdaOf(eVertexBufferElement_Texture0),
        bdaOf(eVertexBufferElement_Color0),
        vb->GetIndexRIBuffer()
            ? vb->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
            : 0,
    };
    RIBuffer_s *const handleBuffers[] = {
        &m_opaquePositionHandles, &m_opaqueTangentHandles,
        &m_opaqueNormalHandles,   &m_opaqueUv0Handles,
        &m_opaqueColorHandles,    &m_opaqueIndexHandles,
    };
    for (size_t i = 0; i < std::size(handleBuffers); ++i) {
      auto *slot = reinterpret_cast<VkDeviceAddress *>(
          static_cast<uint8_t *>(handleBuffers[i]->mappedAddress) +
          req.id * sizeof(VkDeviceAddress));
      *slot = addrs[i];
    }

    vb->AttachResourceToCntx(cntx);

    // firstInstance carries the slot id to the VS via gl_InstanceIndex;
    // the VS pulls vertex / index data via BDA from the bindless set 0 SSBOs,
    // so vertexCount is the index count (one VS invocation per index).
    // Only frustum-visible objects emit an indirect draw — shadow-only casters
    // contribute via the TLAS instance below.
    if (writtenDraws < indirectReq.numElements) {
      indirectDst[writtenDraws++] = VkDrawIndirectCommand{
          /*vertexCount   =*/(uint32_t)pVB->GetIndexNum(),
          /*instanceCount =*/1u,
          /*firstVertex   =*/0u,
          /*firstInstance =*/req.id,
      };
    }

    // BLAS was recorded by SubmitToGPU above into the same primary cmd buffer;
    // the accel-build→accel-build barrier below guarantees the TLAS read sees
    // the BLAS writes.
    auto blas = vb->accelStructure();
    if (blas && blas->vk.handle != VK_NULL_HANDLE) {
      // VkAccelerationStructureInstanceKHR::transform is row-major 3x4
      // (matrix[row][col]), translation at matrix[r][3]. payload.modelMat
      // holds column-major storage (GLSL mat4 reading in gbuffer.vert), so
      // index it as [col*4 + row] to extract entries row-by-row.
      VkAccelerationStructureInstanceKHR inst = {};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          inst.transform.matrix[r][c] = payload.modelMat[c * 4 + r];
        }
      }
      inst.instanceCustomIndex = req.id;
      inst.mask = 0xFF;
      inst.instanceShaderBindingTableRecordOffset = 0;
      inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE;
      inst.accelerationStructureReference = blas->vk.deviceAddress;
      tlasInstances.push_back(inst);
    }
  }

  // ---------- TLAS build ----------
  // Walks the BLAS instances accumulated above and emits one TLAS build into
  // the primary cmd buffer. The TLAS isn't bound to any shader yet (phase 4);
  // building it here exercises the path so RenderDoc / validation can verify
  // correctness.
  if (!tlasInstances.empty()) {
    const uint32_t instanceCount = (uint32_t)tlasInstances.size();

    auto destroyBuffer = [](RIBuffer_s *b) {
      if (b->vk.buffer) {
        auto *cntx = RI.GetActiveSet();
        cntx->freelist.push_back(RIFree(b->vk.buffer));
        cntx->freelist.push_back(RIFree(b->vk.allocation));
      }
      delete b;
    };

    // Grow the instance buffer on demand. Old buffer goes onto the active
    // freelist so any in-flight build that referenced it stays valid until
    // frames-in-flight roll.
    if (instanceCount > m_tlasCapacity) {
      uint32_t newCap = m_tlasCapacity ? m_tlasCapacity : 256;
      while (newCap < instanceCount)
        newCap += (newCap >> 1);
      if (m_tlasInstanceBuffer.vk.buffer) {
        cntx->freelist.push_back(RIFree(m_tlasInstanceBuffer.vk.buffer));
        cntx->freelist.push_back(RIFree(m_tlasInstanceBuffer.vk.allocation));
        m_tlasInstanceBuffer = {};
      }
      // Device-local: the instance buffer is a transfer destination written
      // each frame via the resource uploader. A persistent host mapping would
      // race the GPU's TLAS read for the previous frame still in flight.
      uint32_t qf[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN, qf,
                                      RI_QUEUE_LEN);
      bci.size = (VkDeviceSize)newCap * sizeof(VkAccelerationStructureInstanceKHR);
      bci.usage =
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      VK_WrapResult(vmaCreateBufferWithAlignment(
          RI.device.vk.vmaAllocator, &bci, &aci, 16,
          &m_tlasInstanceBuffer.vk.buffer, &m_tlasInstanceBuffer.vk.allocation,
          nullptr));
      m_tlasCapacity = newCap;
    }

    // Stage the instance array through the resource uploader so frame N+1's
    // write doesn't clobber the buffer mid-build for frame N. The uploader
    // owns the previous-use ↔ TRANSFER_WRITE ↔ next-use barrier pair.
    {
      RIResourceBufferTransaction_s trans = {};
      trans.target = m_tlasInstanceBuffer;
      trans.size = (size_t)instanceCount *
                   sizeof(VkAccelerationStructureInstanceKHR);
      trans.offset = 0;
      trans.vk.current_stage =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      trans.vk.current_access =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      trans.vk.post_stage =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      trans.vk.post_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      std::memcpy(trans.mapped.data, tlasInstances.data(), trans.size);
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    }

    // Make BLAS writes visible to the TLAS build. The plan calls this an
    // accel-build→accel-build barrier; one global memory barrier covers all
    // BLASes that SubmitToGPU just emitted.
    VkMemoryBarrier2 blasToTlas = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    blasToTlas.srcStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    blasToTlas.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blasToTlas.dstStageMask =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    blasToTlas.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep1 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep1.memoryBarrierCount = 1;
    dep1.pMemoryBarriers = &blasToTlas;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep1);

    // Size the TLAS for the worst-case instance count we've seen. Re-init when
    // the instance count exceeds what the current TLAS storage was sized for.
    RIAccelStructureDesc_s tlasDesc = {};
    tlasDesc.type = RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL;
    tlasDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
    tlasDesc.geometryOrInstanceNum = instanceCount;

    uint64_t tlasStorageSize = 0;
    uint64_t tlasBuildScratch = 0;
    GetRIAccelStructureMemoryReqs(&RI.device, &tlasDesc, &tlasStorageSize,
                                  &tlasBuildScratch, nullptr);

    if ((m_tlas.vk.handle == VK_NULL_HANDLE) ||
        (m_tlasStorage.vk.buffer == VK_NULL_HANDLE || tlasStorageSize > m_tlasStorageCapacity)) {
      if (m_tlas.vk.handle != VK_NULL_HANDLE) {
        cntx->freelist.push_back(RIFree(m_tlas.vk.handle));
        cntx->freelist.push_back(RIFree(m_tlasStorage.vk.allocation));
        cntx->freelist.push_back(RIFree(m_tlasStorage.vk.buffer));
        m_tlas = {};
      }
      uint32_t qf[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN, qf,
                                      RI_QUEUE_LEN);
      bci.size = tlasStorageSize;
      bci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      m_tlasStorage = RIBuffer_s::VK_createFromVMA(&RI.device, &bci, &aci);
      tlasDesc.storage = &m_tlasStorage;
      tlasDesc.storageOffset = 0;
      tlasDesc.storageSize = tlasStorageSize;
      if (InitRIAccelStructure(&RI.device, &tlasDesc, &m_tlas) != RI_SUCCESS) {
        // Leave m_tlas zeroed; skip the build this frame.
        m_tlas = {};
      }
      m_tlasStorageCapacity = tlasStorageSize;
    }

    if (m_tlas.vk.handle != VK_NULL_HANDLE) {
      // Source TLAS build scratch from the per-frame accel pool. The pool
      // recycles its blocks across frames and uses the oversized one-shot
      // path for builds that exceed blockSize. RIBlockMem_s embeds an
      // RIBuffer_s, so we hand its address straight to the build desc.
      RIBufferScratchAllocReq_s scratchReq = RIAllocBufferFromScratchAlloc(
          &RI.device, &cntx->accelScratchAlloc, tlasBuildScratch);

      RIBuildTlasDesc_s build = {};
      build.dst = &m_tlas;
      build.src = nullptr;
      build.mode = RI_ACCEL_BUILD_MODE_BUILD;
      build.instanceNum = instanceCount;
      build.instanceBuffer = &m_tlasInstanceBuffer;
      build.instanceOffset = 0;
      build.scratchBuffer = &scratchReq.block.buffer;
      build.scratchOffset = scratchReq.bufferOffset;
      CmdBuildRITlas(&RI.device, &RI.primary.cmds[0], &build, 1);

      VkMemoryBarrier2 tlasToShader = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      tlasToShader.srcStageMask =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      tlasToShader.srcAccessMask =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      // Also visible to the Stage B SurfelGI VBuffer RT pipeline (rgen +
      // chit + ahit + miss) — kept in one barrier with the fragment-shader
      // ray-query consumer in visibility_shade.frag.
      tlasToShader.dstStageMask =
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      tlasToShader.dstAccessMask =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep2.memoryBarrierCount = 1;
      dep2.pMemoryBarriers = &tlasToShader;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep2);
    }
  }

  // m_packedHitInfoView / m_surfelIrradianceView / m_surfelDepthView and the
  // freshly built TLAS now live on set 1 and are pushed per-dispatch via
  // RIProgram::bindDescriptors below (see the m_surfelVBuffer / m_surfelRT
  // / m_surfelIntegrate / m_surfelGenerate / m_surfelGIRender call sites).
  // Set 1 is allocated from a frame-rotated pool, so each frame's writes
  // land on an idle descriptor set.

  VkCommandBuffer cmd = RI.primary.cmds[0].vk.cmd;
  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(16);
  auto pushBinding = [&](const char *name, const RIDescriptor_s &desc,
                         uint32_t registerOffset = 0) {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create(name);
    b.registerOffset = registerOffset;
    b.descriptor = desc;
    bindings.push_back(b);
  };

  // Per-dispatch helpers for the set-1 surfel image / TLAS pushes —
  // `gPackedHitInfo` / `gIrradianceMap` / `gSurfelDepthMap` are
  // RWTexture2D (GENERAL layout, storage image), `gSurfelDepth` is the
  // sampled view of the same depth image (still GENERAL since the
  // image stays GENERAL across the frame and GENERAL satisfies both
  // storage + sampled access). `gRtAccel` is the freshly built TLAS.
  // Each helper appends to a local std::vector<DescriptorBinding> so
  // multiple shaders can mix-and-match the subset they need.
  auto pushSurfelStorageImage =
      [&](std::vector<RIProgram::DescriptorBinding> &v, const char *name,
          VkImageView view) {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(name);
        b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
        b.descriptor.vk.image.imageView = view;
        b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        RIFinalizeDescriptor(&RI.device, &b.descriptor);
        v.push_back(b);
      };
  auto pushSurfelSampledImage =
      [&](std::vector<RIProgram::DescriptorBinding> &v, const char *name,
          VkImageView view) {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(name);
        b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
        b.descriptor.vk.image.imageView = view;
        b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        RIFinalizeDescriptor(&RI.device, &b.descriptor);
        v.push_back(b);
      };
  auto pushTlas = [&](std::vector<RIProgram::DescriptorBinding> &v) {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gRtAccel");
    b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    b.descriptor.vk.accelStructure = m_tlas.vk.handle;
    RIFinalizeDescriptor(&RI.device, &b.descriptor);
    v.push_back(b);
  };

  // sceneObjectsBuf / opaqueMaterialBuf now live in the bindless set (set=0
  // bindings 20..21), wired up by bindBindlessDescriptorSet() below — no
  // per-draw pushBinding needed.

  {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gPerFrame");
    RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
    bindings.push_back(b);
  }

  // Scene's rendering was already ended above (before the BLAS/TLAS work).
  // Transition the MRT target (single packed-TriangleHit attachment) and
  // the depth image into their gbuffer-pass layouts. Both use the
  // UNDEFINED-discard pattern: loadOp=CLEAR on both attachments means we
  // never need prior contents preserved, so it doesn't matter what layout
  // the previous frame's last consumer left them in (depth is left in
  // DEPTH_READ_ONLY_OPTIMAL by the translucent/decal flipDepthToReadOnly
  // path below, which the gbuffer's expected DEPTH_ATTACHMENT_OPTIMAL
  // wouldn't otherwise match).
  {
    VkImageMemoryBarrier2 attachmentBarriers[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};

    VkImageMemoryBarrier2 &toColor = attachmentBarriers[0];
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toColor.srcAccessMask = 0;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toColor.image = RI.visibilityTexture[RI.swapchainIndex].vk.image;

    VkImageMemoryBarrier2 &toDepth = attachmentBarriers[1];
    toDepth.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toDepth.srcAccessMask = 0;
    toDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toDepth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    toDepth.image = RI.depthTextures[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = attachmentBarriers;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  VkRenderingAttachmentInfo colorAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  colorAttachment.imageView = RI.visibilityView[RI.swapchainIndex].vk.image;
  colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  // Cleared to all-zero. psMain writes .w=0 for hit; the depth test at the
  // composite gates miss pixels independently, so no miss sentinel needed.
  colorAttachment.clearValue.color = {{0u, 0u, 0u, 0u}};

  VkRenderingAttachmentInfo depthAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  // MRT now owns the per-frame depth clear (Scene no longer pre-clears).
  RI_VK_FillDepthAttachment(&depthAttachment, &RI.depthView[RI.swapchainIndex],
                            /*attachAndClear=*/true);

  // SurfelGI compute passes are temporarily skipped during the
  // /home/m_pol/.claude/plans/can-we-discard-most-crispy-hennessy.md port.
  // The frame still produces a working image because
  // m_surfelResultTexture is cleared to zero below before the composite
  // samples it — visibility_shade reads vec3(0) for indirect and falls
  // back to direct + specular lighting only. Stages B–F will reintroduce
  // the prepare / VBuffer / update / raytrace / integrate / generation
  // dispatches around this scaffold.

  // ----------------------------------------------------------------------
  // Stage D — surfel prepare + cell/ref-counter clear.
  //
  // Runs first each frame: promote the previous frame's ValidSurfel count
  // to DirtySurfel, zero the per-frame counters, and clear the cellInfo /
  // surfelReservation / surfelRefCounter buffers so the update pass can
  // accumulate from scratch.
  //
  // Stage E (raytrace) and Stage F (integrate / generation) aren't here
  // yet, so the surfel pool stays empty across frames — these dispatches
  // currently no-op (DirtySurfel = 0) but they must run to keep the
  // counter state consistent.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelPrepare.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                        kHash, "SurfelPreparePass.cs",
                                        &computeCreate);
    m_surfelPrepare.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], 1u, 1u, 1u);
  }

  // Ping-pong the surfel-index buffers: copy this frame's previously-valid
  // indices into the dirty buffer so surfel_update_collect.comp can walk
  // them. SurfelGI's reference declares gSurfelValidIndexBuffer (RW) and
  // gSurfelDirtyIndexBuffer (R/O) as separate Slang bindings; Falcor's host
  // either aliases or ping-pongs them across frames. We do a straight copy
  // each frame — simplest, and the cost is negligible (≤600 KB).
  //
  // Source data was last written by the previous frame's update_collect /
  // generation_pass / rchit::finalize. The engine frame fence already
  // ordered that work against this frame's command buffer, so we don't
  // need a transition barrier before the copy. The post-copy barrier
  // (combined with the prepare-pass writes above) covers both the copy's
  // TRANSFER_WRITE and prepare's SHADER_WRITE against update_collect's
  // SHADER_READ.
  {
    VkBufferCopy region = {};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = (VkDeviceSize)kTotalSurfelLimit * sizeof(uint32_t);
    vkCmdCopyBuffer(cmd,
                    m_surfelValidBuffer.vk.buffer,
                    m_surfelDirtyIndexBuffer.vk.buffer,
                    1, &region);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_COPY_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  // Cell + ref-counter clears are now done by SurfelPreparePass +
  // SurfelUpdatePass on the Slang side. No standalone clear dispatch.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // Stage B — primary-ray VBuffer.
  //
  // RT pipeline that traces one ray per swapchain pixel through m_tlas and
  // packs the closest-hit (instanceCustomIndex, primitiveID, attribs) into
  // m_packedHitInfoTexture. Stages D/F consume this image; for the current
  // frame the output goes unused so the dispatch's correctness needs to be
  // verified through validation-layer signals (no SBT/descriptor errors,
  // no VK_ERROR_DEVICE_LOST).
  // ----------------------------------------------------------------------
  {
    VkImageMemoryBarrier2 toGeneral = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toGeneral.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toGeneral.srcAccessMask = 0;
    toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    toGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = m_packedHitInfoTexture[RI.swapchainIndex].vk.image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toGeneral;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    VkRayTracingPipelineCreateInfoKHR rtCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    // closesthit in SurfelVBuffer.rt.slang fires one mirror-reflection
    // TraceRay before recording the second hit (Falcor's one-bounce
    // V-buffer trick), so the pipeline needs depth=2: raygen→chit is
    // depth 1, the recursive TraceRay from inside chit lands at depth 2.
    // depth=1 silently leaves hit pixels unwritten and produced "holes"
    // in gPackedHitInfo against the UNDEFINED initial image contents.
    rtCreate.maxPipelineRayRecursionDepth = 2;
    const hash_t kVBufferHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelVBuffer.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                           kVBufferHash, "SurfelVBuffer.rt",
                                           &rtCreate);
    m_surfelVBuffer.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> vbBindings;
    vbBindings.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      vbBindings.push_back(b);
    }
    pushSurfelStorageImage(vbBindings, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushTlas(vbBindings);

    m_surfelVBuffer.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, vbBindings.data(),
        vbBindings.size(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    m_surfelVBuffer.traceRays(&RI.primary.cmds[0], kVBufferHash,
                              RI.swapchain.width, RI.swapchain.height, 1u);
  }

  {
    // m_packedHitInfoTexture is written by the rgen + miss + chit triplet
    // above. Transition it to SHADER_READ_ONLY_OPTIMAL so consumers can
    // Downstream consumers (SurfelEvaluation / SurfelGeneration /
    // SurfelGIRender) read gPackedHitInfo via direct `[pixel]` storage
    // loads on the bindless RWTexture2D — layout must stay GENERAL
    // through the frame, so this is just a memory/execution sync, not
    // a layout transition. The bindless descriptor was written with
    // VK_IMAGE_LAYOUT_GENERAL at frame start.
    VkMemoryBarrier2 vbufferToConsumers = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    vbufferToConsumers.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    vbufferToConsumers.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vbufferToConsumers.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    vbufferToConsumers.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &vbufferToConsumers;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // (Surfels anchored to destroyed geometry are handled before the opaque
  // draw loop above: the retired slots' stream handles are zeroed, so
  // collectCellInfo below recycles those surfels via its free-list branch
  // instead of dereferencing a freed vertex buffer-device-address. Map-load
  // wholesale resets are handled separately by resetSurfelState().)
  // ----------------------------------------------------------------------

  // ----------------------------------------------------------------------
  // Stage D — surfel update (collect → accumulate → scatter).
  //
  // collect: per dirty surfel, refresh pos/normal from gSurfelGeometryBuffer,
  //          allocate ray budget by MSME variance, bump cellInfo counts
  // accumulate: prefix-sum the per-cell counts into cellToSurfelBufferOffset
  // scatter: per valid surfel, write its index into cellToSurfel[] for each
  //          of the 125 neighbour cells it intersects.
  //
  // The geometry buffer (cached uint4 hits) is filled by the Stage F
  // generation pass; until that lands DirtySurfel stays 0 and these
  // dispatches are no-ops.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateCollect.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:collectCellInfo",
        &computeCreate);
    m_surfelUpdateCollect.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_surfelUpdateCollect.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    CmdDispatch(&RI.primary.cmds[0], (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
  }
  // RAW: accumulate reads cellInfo.surfelCount (collect-written) and reads
  // surfelCounter (collect-incremented). Accumulate's own writes are synced by
  // the next barrier — dstAccess=READ is sufficient and avoids an L2 flush of
  // every other SSBO collect touched (valid/free/recycle/rayResult/refCounter).
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateAccumulate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:accumulateCellInfo",
        &computeCreate);
    m_surfelUpdateAccumulate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    const uint32_t groups = (kCellDimension + 3u) / 4u;
    CmdDispatch(&RI.primary.cmds[0], groups, groups, groups);
  }
  // RAW: scatter reads cellInfo.cellToSurfelBufferOffset (accumulate-written)
  // and RMWs cellInfo.surfelCount (accumulate zeroed it). Scatter's writes are
  // synced by the next barrier. Atomic RMWs only need the prior write visible
  // for the read half — dstAccess=READ suffices.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateScatter.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:updateCellToSurfelBuffer",
        &computeCreate);
    m_surfelUpdateScatter.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_surfelUpdateScatter.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    CmdDispatch(&RI.primary.cmds[0], (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // One-shot UNDEFINED -> GENERAL transition for both surfel atlases the
  // first time each swapchain image appears. Subsequent frames skip this —
  // the atlas stays in GENERAL for life so integrate's EMA-blend reads keep
  // working and cross-frame sync goes through the engine frame fence.
  // Must run before Stage E because surfel_rt.rgen's ray-guiding branch
  // imageLoads gSurfelIrradianceMap; the dst stage mask covers both the
  // rgen read and the later integrate read/write.
  if (!m_surfelAtlasesInitialized[RI.swapchainIndex]) {
    VkImageMemoryBarrier2 toGeneral[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (uint32_t i = 0; i < 2; ++i) {
      toGeneral[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
      toGeneral[i].srcAccessMask = 0;
      toGeneral[i].dstStageMask =
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      toGeneral[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      toGeneral[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      toGeneral[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      toGeneral[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toGeneral[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toGeneral[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toGeneral[0].image = m_surfelIrradianceTexture[RI.swapchainIndex].vk.image;
    toGeneral[1].image = m_surfelDepthTexture[RI.swapchainIndex].vk.image;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = toGeneral;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    m_surfelAtlasesInitialized[RI.swapchainIndex] = true;
  }

  // ----------------------------------------------------------------------
  // Stage E — surfel ray-trace.
  //
  // One TraceRay per pending ray slot. The rgen pulls RequestedRay from
  // the counter, derives a tangent-space hemisphere direction per surfel,
  // and iteratively bounces (closest-hit updates the payload, miss / max
  // step / RR / surfel-cache finalize all terminate the path). NEE inside
  // the closest-hit uses an inline ray query so no recursion is needed
  // (maxPipelineRayRecursionDepth = 1).
  //
  // The reference's expected per-surfel ray count is bounded by
  // gMaxRayCount = 64; the dispatched width is kRayBudget = 9.6M, the rgen
  // early-outs past RequestedRay so unused slots cost nothing.
  // ----------------------------------------------------------------------
  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    VkRayTracingPipelineCreateInfoKHR rtCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    rtCreate.maxPipelineRayRecursionDepth = 1;
    const hash_t kRtHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelRT.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                      kRtHash, "SurfelRayTrace.rt",
                                      &rtCreate);
    m_surfelRT.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> rtBnd;
    rtBnd.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      rtBnd.push_back(b);
    }
    pushTlas(rtBnd);
    // Ray-guiding read of the surfel irradiance map. The atlas was
    // transitioned to GENERAL once at first appearance (see
    // m_surfelAtlasesInitialized above) and stays GENERAL across the
    // frame so the integrate pass's prior-frame store is read-visible
    // through the engine frame fence.
    pushSurfelStorageImage(rtBnd, "gIrradianceMap",
                           m_surfelIrradianceView[RI.swapchainIndex].vk.image);
    m_surfelRT.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, rtBnd.data(),
        rtBnd.size(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    m_surfelRT.traceRays(&RI.primary.cmds[0], kRtHash, kRayBudget, 1u, 1u);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  VkRenderingInfo renderingInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderingInfo.renderArea = {{0, 0},
                              {RI.swapchain.width, RI.swapchain.height}};
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = &depthAttachment;
  vkCmdBeginRendering(cmd, &renderingInfo);

  VkViewport vkViewport = {0,
                           (float)RI.swapchain.height,
                           (float)RI.swapchain.width,
                           -(float)RI.swapchain.height,
                           0.0f,
                           1.0f};
  VkRect2D scissor = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
  vkCmdSetViewport(cmd, 0, 1, &vkViewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  if (writtenDraws > 0) {
    GBufferMRTPipelineDesc pipelineDesc(RIBootstrap::VisibilityFormat,
                                        RIBootstrap::DepthFormat);
    m_gbuffer.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                           "SurfelGBuffer.3d", &pipelineDesc.createInfo);
    m_gbuffer.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet, 0);
    m_gbuffer.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                              bindings.data(), bindings.size());
    CmdDrawIndirect(&RI.primary.cmds[0], &m_indirectDrawBuffer,
                    (VkDeviceSize)indirectReq.elementOffset *
                        sizeof(VkDrawIndirectCommand),
                    writtenDraws, (uint32_t)sizeof(VkDrawIndirectCommand));
  }

  vkCmdEndRendering(cmd);

  // Gbuffer output -> SHADER_READ_ONLY for the surfel-generation compute
  // pass (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The surfel result
  // image transitions UNDEFINED -> GENERAL for its first compute write.
  {
    VkImageMemoryBarrier2 toRead[3] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    toRead[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toRead[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toRead[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead[0].image = RI.visibilityTexture[RI.swapchainIndex].vk.image;

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toRead[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toRead[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    toRead[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    toRead[1].image = RI.depthTextures[RI.swapchainIndex].vk.image;

    // Surfel-result image: Stage F's surfel_generation_pass writes into it
    // as a storage image. Discard-on-acquire pattern — the engine frame
    // fence orders any previous sample against this overwrite, and
    // surfel_generation_pass itself unconditionally writes every in-bounds
    // pixel (miss / out-of-cell pixels get vec4(0,0,0,1)), so the prior
    // vkCmdClearColorImage + sync barrier are no longer needed.
    toRead[2].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toRead[2].srcAccessMask = 0;
    toRead[2].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[2].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toRead[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toRead[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead[2].image = m_surfelResultTexture[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 3;
    dep.pImageMemoryBarriers = toRead;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // Stage F — surfel integrate + generation.
  //
  // integrate: per valid surfel, MSME-blend the per-frame raytraced
  //            radiance (from gSurfelRayResultBuffer) into surfel.radiance
  // generate:  per pixel, walk the visibility-buffer cell's surfel list,
  //            output the indirect-lighting term into m_surfelResultTexture
  //            (which visibility_shade.frag samples), and spawn / recycle
  //            surfels based on coverage thresholds.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelIntegrate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                          kHash, "SurfelIntegratePass.cs",
                                          &computeCreate);
    m_surfelIntegrate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    // gSurfelDepthSampler is bindless (set 0). The image views and the
    // CB are gone — params come from Constants.h. gPerFrame, the
    // irradiance atlas, and the depth atlas (RW + sampled views of the
    // same image) push into set 1.
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(4);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gIrradianceMap",
                           m_surfelIrradianceView[RI.swapchainIndex].vk.image);
    pushSurfelStorageImage(bnd, "gSurfelDepthMap",
                           m_surfelDepthView[RI.swapchainIndex].vk.image);
    pushSurfelSampledImage(bnd, "gSurfelDepth",
                           m_surfelDepthView[RI.swapchainIndex].vk.image);
    m_surfelIntegrate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGenerate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                         kHash, "SurfelGenerationPass.cs",
                                         &computeCreate);
    m_surfelGenerate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    // gPerFrame + the set-1 surfel images (gPackedHitInfo, gSurfelDepth
    // sampled view) plus the per-pixel indirect-lighting output `gOutput`
    // at set 2 binding 0.
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(4);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushSurfelSampledImage(bnd, "gSurfelDepth",
                           m_surfelDepthView[RI.swapchainIndex].vk.image);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gOutput");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView =
          m_surfelResultView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bnd.push_back(b);
    }
    m_surfelGenerate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    const uint32_t fullW = RI.swapchain.width;
    const uint32_t fullH = RI.swapchain.height;
    CmdDispatch(&RI.primary.cmds[0], (fullW + 15u) / 16u,
                (fullH + 15u) / 16u, 1u);
  }

  // --------------------------------------------------------------------
  // SurfelGIRenderPass — graphics fullscreen pass.
  //
  // Replaces the legacy visibility_shade composite (and the prior compute
  // version of this same Slang module). Reads gIndirectLighting
  // (m_surfelResultView, written by SurfelGenerationPass above) plus
  // gPackedHitInfo / gPackedHitInfoRaster / TLAS / gPerFrame, and emits
  // its color into the pogo buffer's COLOR_ATTACHMENT side.
  //
  // The post-effect chain ping-pongs through pogo from there; a tail
  // blit lower in this function copies the final pogo "read" half into
  // the swapchain image, which the particle/decal pass then composites
  // on top of.
  // --------------------------------------------------------------------

  RI_PogoBuffer *pogo = &RI.pogoBuffer[RI.swapchainIndex];

  // Barrier 1: gIndirectLighting GENERAL→SHADER_READ_ONLY (compute write
  // is now consumed by a fragment-stage sample) + first-frame pogo init
  // (UNDEFINED → COLOR_ATTACHMENT_OPTIMAL on the attach half, UNDEFINED →
  // SHADER_READ_ONLY_OPTIMAL on the other half, matching the steady-state
  // the toggle helpers expect).
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

    std::vector<VkImageMemoryBarrier2> imageBarriers;
    imageBarriers.reserve(3);

    {
      VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = m_surfelResultTexture[RI.swapchainIndex].vk.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      imageBarriers.push_back(b);
    }

    if (!m_pogoInitialized[RI.swapchainIndex]) {
      const uint32_t readIdx = (pogo->attachmentIndex + 1u) % 2u;
      imageBarriers.push_back(VK_RI_PogoAttachmentMemoryBarrier2(
          pogo->textures[pogo->attachmentIndex].vk.image, /*initial=*/true));
      imageBarriers.push_back(VK_RI_PogoShaderMemoryBarrier2(
          pogo->textures[readIdx].vk.image, /*initial=*/true));
      m_pogoInitialized[RI.swapchainIndex] = true;
    }

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    dep.imageMemoryBarrierCount =
        static_cast<uint32_t>(imageBarriers.size());
    dep.pImageMemoryBarriers = imageBarriers.data();
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Surfel-GI graphics pass — writes color into the pogo attach side.
  {
    VkRenderingAttachmentInfo colorAttach = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttach.imageView =
        pogo->pogoAttachment[pogo->attachmentIndex].vk.image.imageView;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0},
                             {RI.swapchain.width, RI.swapchain.height}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments    = &colorAttach;
    vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

    VkViewport vp = {0.0f,
                     0.0f,
                     static_cast<float>(RI.swapchain.width),
                     static_cast<float>(RI.swapchain.height),
                     0.0f,
                     1.0f};
    vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
    vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

    PostEffectPipelineState surfelState{};
    InitPostEffectPipelineState(surfelState, VK_FORMAT_R8G8B8A8_UNORM,
                                /*alphaBlend=*/false);

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGIRender.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                  "SurfelGIRenderPass",
                                  &surfelState.createInfo);
    m_surfelGIRender.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_GRAPHICS);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(5);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushTlas(bnd);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gIndirectLighting");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView =
          m_surfelResultView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bnd.push_back(b);
    }
    {
      // Rasterized V-buffer fallback — SurfelGBuffer writes
      // RI.visibilityTexture earlier this frame and the toRead[] barriers
      // upstream already transitioned it to SHADER_READ_ONLY_OPTIMAL.
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPackedHitInfoRaster");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView =
          RI.visibilityView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bnd.push_back(b);
    }
    m_surfelGIRender.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_GRAPHICS);

    vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
  }

  // Toggle: just-written attach → SHADER_READ_ONLY (now the "read" half)
  // so downstream post-effects + tail blit can sample it.
  RI_PogoBufferToggle(&RI.device, pogo, &RI.primary.cmds[0]);

  // Post-effect composite (no-op when no active effects).
  //if (viewport && viewport->GetPostEffectComposite() &&
  //    viewport->GetPostEffectComposite()->HasActiveEffects()) {
  //  viewport->GetPostEffectComposite()->Render(
  //      afFrameTime, &RI.primary.cmds[0], pogo, RI.swapchain.width,
  //      RI.swapchain.height, RI.frameIndex);
  //}

  // Tail blit: pogo "read" half → swapchain. First transition swapchain
  // UNDEFINED → COLOR_ATTACHMENT_OPTIMAL.
  {
    VkImageMemoryBarrier2 swapBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    swapBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    swapBarrier.srcAccessMask = 0;
    swapBarrier.dstStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.image = RI.swapchain.vk.images[RI.swapchainIndex];
    swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &swapBarrier;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  {
    VkRenderingAttachmentInfo colorAttach = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttach.imageView = RI.swapchainView[RI.swapchainIndex].vk.image;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0},
                             {RI.swapchain.width, RI.swapchain.height}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments    = &colorAttach;
    vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

    VkViewport vp = {0.0f,
                     0.0f,
                     static_cast<float>(RI.swapchain.width),
                     static_cast<float>(RI.swapchain.height),
                     0.0f,
                     1.0f};
    vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
    vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

    PostEffectPipelineState blitState{};
    InitPostEffectPipelineState(
        blitState, RIFormatToVK((RI_Format_e)RI.swapchain.format),
        /*alphaBlend=*/false);

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
    m_postEffectBlit.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                  "PostEffect.tailBlit",
                                  &blitState.createInfo);

    RIDescriptor_s *samplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    RIProgram::DescriptorBinding bindings[2] = {};
    bindings[0].descriptor = *samplerDesc;
    bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
    bindings[1].descriptor = *RI_PogoBufferShaderResource(pogo);
    bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
    m_postEffectBlit.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, bindings, 2);

    vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
  }

  // Swapchain self-sync: blit's COLOR_ATTACHMENT_OUTPUT writes must
  // complete before the particle pass's COLOR_ATTACHMENT_OUTPUT writes
  // (dynamic rendering has no implicit hand-off). Layout stays
  // COLOR_ATTACHMENT_OPTIMAL.
  {
    VkImageMemoryBarrier2 swapBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    swapBarrier.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swapBarrier.dstStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    swapBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapBarrier.image = RI.swapchain.vk.images[RI.swapchainIndex];
    swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &swapBarrier;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Shared between the decal and particle passes: the depth image arrives in
  // SHADER_READ_ONLY_OPTIMAL from surfel-generate and needs to flip to
  // DEPTH_READ_ONLY_OPTIMAL for either pass's depth-test. Only the first pass
  // that has work to draw issues the barrier; the second one reuses the layout.
  bool depthFlippedForReadOnly = false;
  auto flipDepthToReadOnly = [&]() {
    if (depthFlippedForReadOnly)
      return;
    VkImageMemoryBarrier2 depthBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    depthBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    depthBarrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                 VK_ACCESS_2_SHADER_READ_BIT;
    depthBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = RI.depthTextures[RI.swapchainIndex].vk.image;
    depthBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &depthBarrier;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    depthFlippedForReadOnly = true;
  };


  // --------------------------------------------------------------------
  // Particle (translucent) pass.
  //
  // Port of RendererDeferred's translucency pass restricted to particle
  // emitters. Other translucent material variants (water, refraction,
  // reflection) need additional infrastructure (refraction-source copy,
  // reflection buffers) that the hybrid renderer doesn't have yet, so
  // they're skipped here.
  //
  // Resources reused from the opaque path:
  //   - m_diffuseBindless / m_diffuseObjectBuffer (per-emitter OBJECT slot)
  //   - m_opaque*Handles  (BDA fan-out — particles overload position/uv0/
  //                        color/index with their own VB device addresses)
  //   - m_materialBindless / m_opaqueMaterialBuffer (material slot; only
  //                        the diffuse texture index is read by particle.frag)
  //
  // Sync setup:
  //   (a) Swapchain stays in COLOR_ATTACHMENT_OPTIMAL from the visibility
  //       composite — load to preserve the composite output.
  //   (b) Depth was last transitioned to SHADER_READ_ONLY for surfel-
  //       generate; flipDepthToReadOnly() moves it back to
  //       DEPTH_READ_ONLY_OPTIMAL (shared with the decal pass, idempotent).
  // --------------------------------------------------------------------
  {
    // Collect particle emitters from the translucent list once so we can
    // skip the whole pass (and its barriers/begin-rendering) when empty.
    std::vector<iParticleEmitter *> emitters;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj || pObj->GetRenderType() != eRenderableType_ParticleEmitter)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      const eMaterialBlendMode mode = pMat->GetBlendMode();
      if (mode == eMaterialBlendMode_None ||
          mode >= eMaterialBlendMode_LastEnum)
        continue;
      emitters.push_back(static_cast<iParticleEmitter *>(pObj));
    }

    if (!emitters.empty()) {
      // Drive the particle vertex buffer's CPU-side refresh + GPU upload.
      // UpdateGraphicsForViewport recomputes per-vertex positions/colors and
      // marks the VB dirty; SubmitToGPU re-uploads any dirty streams. Done
      // before the renderpass begins so the resource-uploader barriers don't
      // collide with vkCmdBeginRendering.
      for (iParticleEmitter *pEmitter : emitters) {
        pEmitter->UpdateGraphicsForFrame(afFrameTime);
        pEmitter->UpdateGraphicsForViewport(apFrustum, afFrameTime);
        iVertexBuffer *pVB = pEmitter->GetVertexBuffer();
        if (pVB) {
          auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
          vbri->SubmitToGPU(&RI.primary.cmds[0], &RI.device, cntx);
          vbri->AttachResourceToCntx(cntx);
        }
      }

      flipDepthToReadOnly();

      VkRenderingAttachmentInfo colorAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      colorAttach.imageView = RI.swapchainView[RI.swapchainIndex].vk.image;
      colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingAttachmentInfo depthAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAttach.imageView = RI.depthView[RI.swapchainIndex].vk.image;
      depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea = {{0, 0},
                               {RI.swapchain.width, RI.swapchain.height}};
      renderInfo.layerCount = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments = &colorAttach;
      renderInfo.pDepthAttachment = &depthAttach;
      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f,
                       (float)RI.swapchain.height,
                       (float)RI.swapchain.width,
                       -(float)RI.swapchain.height,
                       0.0f,
                       1.0f};
      VkRect2D sc = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_particle.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet,
                                           0);

      std::vector<RIProgram::DescriptorBinding> particleBindings;
      particleBindings.reserve(1);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        particleBindings.push_back(b);
      }
      m_particle.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                 particleBindings.data(),
                                 particleBindings.size());

      // Map eMaterialBlendMode -> ParticlePipelineDesc::BlendMode +
      // shader-side BLEND_MODE_*. eMaterialBlendMode_None is filtered above.
      auto remapBlend = [](eMaterialBlendMode m) {
        switch (m) {
        case eMaterialBlendMode_Add:
          return ParticlePipelineDesc::BLEND_ADD;
        case eMaterialBlendMode_Mul:
          return ParticlePipelineDesc::BLEND_MUL;
        case eMaterialBlendMode_MulX2:
          return ParticlePipelineDesc::BLEND_MULX2;
        case eMaterialBlendMode_Alpha:
          return ParticlePipelineDesc::BLEND_ALPHA;
        case eMaterialBlendMode_PremulAlpha:
          return ParticlePipelineDesc::BLEND_PREMUL_ALPHA;
        default:
          return ParticlePipelineDesc::BLEND_ADD;
        }
      };

      struct PushBlock {
        uint32_t blendMode;
        float sceneAlpha;
      };

      for (iParticleEmitter *pEmitter : emitters) {
        iVertexBuffer *pVB = pEmitter->GetVertexBuffer();
        cMaterial *pMat = pEmitter->GetMaterial();
        if (!pVB || !pMat)
          continue;
        const int indexCount = pVB->GetIndexNum();
        if (indexCount <= 0)
          continue;

        uint32_t materialSlot =
            resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
        if (materialSlot == UINT32_MAX) {
          Warning("Material Slot exhausted (particle)");
          continue;
        }

        cMatrixf *pMtx = pEmitter->GetModelMatrix(apFrustum);
        UniformObject payload{};
        payload.dissolveAmount = 0.0f;
        payload.materialID = materialSlot;
        payload.lightLevel = 1.0f;
        payload.illuminationAmount = 0.0f;
        const ml::float4x4 modelF4 =
            cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
        std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
        ml::float4x4 invF4 = modelF4;
        invF4.Invert();
        std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
        const ml::float4x4 uvF4 =
            cMath::ToFloatTranspose4x4(cMatrixf::Identity);
        std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

        // Particle VB contents change every frame, so hash a per-emitter
        // identity (pointer + frame counter) rather than the payload contents
        // — payload-hash collisions across frames would skip the BDA refresh
        // even though the VB device addresses might have changed.
        hash_t cookie = hash_u64(HASH_INITIAL_VALUE,
                                 (uint64_t)(uintptr_t)pEmitter);
        cookie = hash_u32(cookie, (uint32_t)RI.frameIndex);
        auto req =
            m_diffuseBindless.request(cookie, (uint32_t)RI.frameIndex);
        if (req.exhausted) {
          Warning("bindless pool exhausted (particle)");
          continue;
        }

        {
          RIResourceBufferTransaction_s trans = {};
          trans.target = m_diffuseObjectBuffer;
          trans.size = sizeof(UniformObject);
          trans.offset = (size_t)req.id * sizeof(UniformObject);
          trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
          trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
          trans.vk.post_stage = trans.vk.current_stage;
          trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
          RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
          std::memcpy(trans.mapped.data, &payload, sizeof(payload));
          RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
        }

        auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
        auto bdaOf = [&](eVertexBufferElement type) -> VkDeviceAddress {
          const auto *element = vbri->GetElement(type);
          if (!element || !element->buffer)
            return 0;
          return element->buffer->GetDeviceHandle(&RI.device);
        };
        const VkDeviceAddress posAddr =
            bdaOf(eVertexBufferElement_Position);
        const VkDeviceAddress uv0Addr =
            bdaOf(eVertexBufferElement_Texture0);
        const VkDeviceAddress colAddr =
            bdaOf(eVertexBufferElement_Color0);
        const VkDeviceAddress idxAddr =
            vbri->GetIndexRIBuffer()
                ? vbri->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
                : 0;

        // Only the four streams the particle VS reads need to be valid;
        // tangent/normal stay zero so any leftover handles from a prior
        // opaque draw at this slot are deref-safe-but-unused.
        auto writeSlot = [&](RIBuffer_s &buf, VkDeviceAddress addr) {
          auto *slot = reinterpret_cast<VkDeviceAddress *>(
              static_cast<uint8_t *>(buf.mappedAddress) +
              req.id * sizeof(VkDeviceAddress));
          *slot = addr;
        };
        writeSlot(m_opaquePositionHandles, posAddr);
        writeSlot(m_opaqueUv0Handles, uv0Addr);
        writeSlot(m_opaqueColorHandles, colAddr);
        writeSlot(m_opaqueIndexHandles, idxAddr);
        writeSlot(m_opaqueNormalHandles, 0);
        writeSlot(m_opaqueTangentHandles, 0);

        const ParticlePipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        ParticlePipelineDesc pipelineDesc((RI_Format_e)RI.swapchain.format,
                                          RIBootstrap::DepthFormat, mode);
        m_particle.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                pipelineDesc.hash, "Particle",
                                &pipelineDesc.createInfo);

        float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha};
        vkCmdPushConstants(RI.primary.cmds[0].vk.cmd,
                           m_particle.getPipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);

        vkCmdDraw(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount, 1u, 0u,
                  req.id);
      }

      vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
    }
  }

  // Restore depth to DEPTH_ATTACHMENT_OPTIMAL before yielding the command
  // buffer: RI_VK_FillDepthAttachment hardcodes that layout, and depth ends
  // here in either SHADER_READ_ONLY_OPTIMAL (surfel-only) or
  // DEPTH_READ_ONLY_OPTIMAL (flipDepthToReadOnly ran for particle/decal).
  {
    const VkImageLayout currentLayout =
        depthFlippedForReadOnly ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkPipelineStageFlags2 srcStage =
        depthFlippedForReadOnly
            ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
            : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    const VkAccessFlags2 srcAccess =
        depthFlippedForReadOnly
            ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
            : (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
               VK_ACCESS_2_SHADER_READ_BIT);

    VkImageMemoryBarrier2 restoreDepth = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    restoreDepth.srcStageMask = srcStage;
    restoreDepth.srcAccessMask = srcAccess;
    restoreDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    restoreDepth.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    restoreDepth.oldLayout = currentLayout;
    restoreDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    restoreDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreDepth.image = RI.depthTextures[RI.swapchainIndex].vk.image;
    restoreDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &restoreDepth;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }
}

cHybridRenderer::~cHybridRenderer() {
  // TLAS handle goes through the device's destroy; the storage buffer +
  // instance buffer share the deferred-free path via their shared_ptr deleter
  // (storage) and explicit queue here (instance). Caller guarantees the device
  // is idle by the time this fires.
  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    vkDestroyAccelerationStructureKHR(RI.device.vk.device, m_tlas.vk.handle,
                                      NULL);
    m_tlas = {};
  }
  if (m_tlasInstanceBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_tlasInstanceBuffer.vk.buffer,
                     m_tlasInstanceBuffer.vk.allocation);
    m_tlasInstanceBuffer = {};
  }
  if (m_pointLightBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_pointLightBuffer.vk.buffer,
                     m_pointLightBuffer.vk.allocation);
    m_pointLightBuffer = {};
  }
  if (m_spotLightBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_spotLightBuffer.vk.buffer,
                     m_spotLightBuffer.vk.allocation);
    m_spotLightBuffer = {};
  }
  if (m_boxLightBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_boxLightBuffer.vk.buffer,
                     m_boxLightBuffer.vk.allocation);
    m_boxLightBuffer = {};
  }
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    if (m_surfelResultView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_surfelResultView[i].vk.image,
                         NULL);
      m_surfelResultView[i] = {};
    }
    if (m_surfelResultTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator, m_surfelResultTexture[i].vk.image, m_surfelResultTexture[i].vk.allocation);
      m_surfelResultTexture[i] = {};
    }
    if (m_packedHitInfoView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_packedHitInfoView[i].vk.image,
                         NULL);
      m_packedHitInfoView[i] = {};
    }
    if (m_packedHitInfoTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_packedHitInfoTexture[i].vk.image,
                      m_packedHitInfoTexture[i].vk.allocation);
      m_packedHitInfoTexture[i] = {};
    }
    if (m_surfelIrradianceView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_surfelIrradianceView[i].vk.image, NULL);
      m_surfelIrradianceView[i] = {};
    }
    if (m_surfelIrradianceTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator, m_surfelIrradianceTexture[i].vk.image, m_surfelIrradianceTexture[i].vk.allocation);
      m_surfelIrradianceTexture[i] = {};
    }
    if (m_surfelDepthView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_surfelDepthView[i].vk.image,
                         NULL);
      m_surfelDepthView[i] = {};
    }
    if (m_surfelDepthTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_surfelDepthTexture[i].vk.image,
                      m_surfelDepthTexture[i].vk.allocation);
      m_surfelDepthTexture[i] = {};
    }
  }
  m_bindlessSet.destroy(&RI.device);
}

} // namespace hpl
