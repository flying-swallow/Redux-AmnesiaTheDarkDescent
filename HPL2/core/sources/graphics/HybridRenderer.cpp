#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialResource.h"
#include "graphics/MaterialType.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/Renderable.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer_RI.h"
#include "math/Frustum.h"
#include "math/Math.h"

#include "resources/Resources.h"
#include "scene/Light.h"
#include "scene/LightBox.h"
#include "scene/LightSpot.h"
#include "scene/RenderableContainer.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace hpl {

namespace detail {

static struct RIBuffer_s CreateBindlessSlotBuffer(RIDevice_s *device,
                                                  uint32_t slotCount,
                                                  size_t elementStride,
                                                  VkBufferUsageFlags usage) {
  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  VkBufferCreateInfo bufferCreateInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  VK_ConfigureBufferQueueFamilies(&bufferCreateInfo, device->queues,
                                  RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);
  bufferCreateInfo.size = (VkDeviceSize)slotCount * elementStride;
  bufferCreateInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo = {};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
  allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

  VmaAllocationInfo allocationInfo = {};
  struct RIBuffer_s out;
  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bufferCreateInfo,
                                &allocInfo, &out.vk.buffer, &out.vk.allocation,
                                &allocationInfo));
  out.mappedAddress = allocationInfo.pMappedData;
  return out;
}

} // namespace detail

namespace {

// Holder for the static portion of the "hybrid.gbuffer_mrt"
// VkGraphicsPipelineCreateInfo. Owns every sub-struct so the pointer
// chain stays valid as long as the holder lives. Non-copyable /
// non-movable - the pNext / pXxxState pointers would dangle.
struct GBufferMRTPipelineDesc {
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkFormat colorFormats[2];
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachments[2];
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  GBufferMRTPipelineDesc(RI_Format_e normalFormat, RI_Format_e visibilityFormat,
                         RI_Format_e depthFormat) {
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

    colorFormats[0] = RIFormatToVK(normalFormat);
    colorFormats[1] = RIFormatToVK(visibilityFormat);
    pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pipelineRendering.colorAttachmentCount = 2;
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
    depthStencilState.depthWriteEnable = VK_TRUE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencilState.minDepthBounds = 0.0f;
    depthStencilState.maxDepthBounds = 1.0f;

    // Both gbuffer outputs are written as plain SRC=ONE/DST=ZERO copies — no
    // actual blending. The visibility attachment (uint) doesn't support
    // VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, so blendEnable must be
    // VK_FALSE for it. Disabling on the normal attachment too keeps the
    // behavior identical (factors were already an identity copy) and drops
    // the dependency on COLOR_ATTACHMENT_BLEND_BIT.
    blendAttachments[0] = {VK_FALSE,
                           VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO,
                           VK_BLEND_OP_ADD,
                           VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO,
                           VK_BLEND_OP_ADD,
                           VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT};
    blendAttachments[1] = {
        VK_FALSE,        VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
        VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
        VK_BLEND_OP_ADD, VK_COLOR_COMPONENT_R_BIT};
    colorBlendState = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlendState.attachmentCount = 2;
    colorBlendState.pAttachments = blendAttachments;

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

    hash = hash_u32(HASH_INITIAL_VALUE, normalFormat);
    hash = hash_u32(hash, depthFormat);
  }

  GBufferMRTPipelineDesc(const GBufferMRTPipelineDesc &) = delete;
  GBufferMRTPipelineDesc &operator=(const GBufferMRTPipelineDesc &) = delete;
};

// Pipeline descriptor for the final visibility-buffer composite. Fullscreen
// triangle, zero vertex inputs (procedurally generated in the vertex
// shader from gl_VertexIndex), one color target = swapchain image, no
// depth/stencil, no blending. Same lifetime contract as
// GBufferMRTPipelineDesc — keep alive across the pipeline create call.
struct VisibilityShadePipelineDesc {
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

  explicit VisibilityShadePipelineDesc(RI_Format_e swapchainFormat) {
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
    pipelineRendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    pipelineRendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    multisampleState = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    depthStencilState = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencilState.depthTestEnable = VK_FALSE;
    depthStencilState.depthWriteEnable = VK_FALSE;
    depthStencilState.minDepthBounds = 0.0f;
    depthStencilState.maxDepthBounds = 1.0f;

    blendAttachment = {VK_FALSE,
                       VK_BLEND_FACTOR_ONE,
                       VK_BLEND_FACTOR_ZERO,
                       VK_BLEND_OP_ADD,
                       VK_BLEND_FACTOR_ONE,
                       VK_BLEND_FACTOR_ZERO,
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

    hash = hash_u32(HASH_INITIAL_VALUE, swapchainFormat);
  }

  VisibilityShadePipelineDesc(const VisibilityShadePipelineDesc &) = delete;
  VisibilityShadePipelineDesc &
  operator=(const VisibilityShadePipelineDesc &) = delete;
};

} // namespace

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources, 0),
      m_diffuseBindless(OBJECT_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT),
      m_textureBindless(TEXTURE_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT),
      m_materialBindless(MATERIAL_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT) {
  {
    {
      std::vector<RIBindlessDescriptorSet::Binding> bindings = {};
      // textures_2d[] — sampled by gbuffer (FS) and surfel_raytrace (CS) for
      // bindless material albedo fetches.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          BINDING_TEXTURES_2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          TEXTURE_SLOT_CAPACITY,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
              VK_SHADER_STAGE_COMPUTE_BIT,
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
      // opaque*Handles bindings 3..8 — vertex pulling for gbuffer VS and BDA
      // hit fetches in surfel_raytrace CS.
      for (uint32_t i = 0; i < 6; ++i) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            BINDING_OPAQUE_POSITION_HANDLES + i,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                VK_SHADER_STAGE_COMPUTE_BIT,
            0});
      }
      // materialSampler — combined with textures_2d at FS sites and at the
      // surfel_raytrace CS albedo fetch.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          BINDING_MATERIAL_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
              VK_SHADER_STAGE_COMPUTE_BIT,
          0});
      // Surfel-GI SSBOs (bindings 10..16) + cell-grid SSBOs (bindings 17..19).
      // Used by compute (surfel_*.comp) and sampled in fragment shading once
      // the GI path lights surfaces.
      const uint32_t kSurfelCellBindings[] = {
          BINDING_SURFEL_COUNTER, BINDING_SURFEL_BUFFER, BINDING_SURFEL_ALIVE,
          BINDING_SURFEL_DEAD,    BINDING_SURFEL_DIRTY,  BINDING_SURFEL_RECYCLE,
          BINDING_SURFEL_RAY,     BINDING_CELL_BUFFER,   BINDING_CELL_COUNTER,
          BINDING_CELL_TO_SURFEL,
      };
      for (uint32_t b : kSurfelCellBindings) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      }
      // Scene-object + opaque-material tables (bindings 20..21). Read by the
      // gbuffer pipeline (VS reads sceneObjects, FS reads opaqueMaterial) and
      // by surfel_raytrace CS (both, for hit-data + albedo fetches).
      const uint32_t kSceneTableBindings[] = {
          BINDING_SCENE_OBJECTS,
          BINDING_OPAQUE_MATERIAL,
      };
      for (uint32_t b : kSceneTableBindings) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                VK_SHADER_STAGE_COMPUTE_BIT,
            0});
      }
      // Point/spot/box-light SSBOs (bindings 22, 29, 30). Compute reads
      // pointLights[] during surfel raytrace; fragment reads all three in
      // visibility_shade.frag.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          BINDING_POINT_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          BINDING_SPOT_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          BINDING_BOX_LIGHTS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});

      VkDescriptorPoolSize poolSizes[3] = {};
      poolSizes[0] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          TEXTURE_SLOT_CAPACITY};
      // 6 opaque*Handles + 7 surfel + 3 cell + 2 scene/material
      //   + 3 light (point/spot/box) + 1 surfel-lobe (Phase 4) = 22.
      poolSizes[1] =
          VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 22};
      poolSizes[2] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1};

      m_bindlessSet.initialize(&RI.device, bindings, poolSizes);
    }

    const VkDescriptorSetLayout externalLayouts[] = {
        m_bindlessSet.vk.m_bindlessSetLayout};
    {
      auto vert_stage = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "gbuffer.vert.spv");
      auto frag_stage = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "gbuffer.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT,
                                 frag_stage}};
      m_gbuffer.initialize(&RI.device, stages, externalLayouts);
    }

    {
      auto comp_prepare = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "surfel_prepare.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_prepare}};
      m_surfelPrepare.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_generate = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "surfel_generation_pass.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_generate}};
      m_surfelGenerate.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_update = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "surfel_update.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_update}};
      m_surfelUpdate.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_cellInfo = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "cellInfo_update_pass.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_cellInfo}};
      m_cellInfoUpdate.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_cellToSurfel = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "cellToSurfel_update_pass.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_cellToSurfel}};
      m_cellToSurfelUpdate.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_raytrace = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "surfel_raytrace.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_raytrace}};
      m_surfelRaytrace.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_integrate = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "surfel_integrate.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, comp_integrate}};
      m_surfelIntegrate.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto vis_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "visibility_shade.vert.spv");
      auto vis_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "visibility_shade.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vis_vert},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, vis_frag}};
      m_visibilityShade.initialize(&RI.device, stages, externalLayouts);
    }

    const VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    m_diffuseObjectBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(ObjectGPUData), kStorage);
    m_opaquePositionHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(VkDeviceAddress), kStorage);
    m_opaqueTangentHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(VkDeviceAddress), kStorage);
    m_opaqueNormalHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(VkDeviceAddress), kStorage);
    m_opaqueUv0Handles = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(VkDeviceAddress), kStorage);
    m_opaqueColorHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(VkDeviceAddress), kStorage);
    m_opaqueIndexHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, OBJECT_SLOT_CAPACITY, sizeof(VkDeviceAddress), kStorage);

    RISegmentAllocDesc_s indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = (uint16_t)OBJECT_SLOT_CAPACITY;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, indirectDesc.maxElements, sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Surfel-GI SSBOs (bindless.resource.glsl set=0 bindings 10..16). Sizes
    // come from forward_shared.h; element strides come from the matching C++
    // twin structs declared in the same header.
    m_surfelCounterBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, 1, sizeof(SurfelCounter), kStorage);
    m_surfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACITY, sizeof(Surfel), kStorage);
    m_surfelAliveBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACITY, sizeof(uint32_t), kStorage);
    m_surfelDeadBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACITY, sizeof(uint32_t), kStorage);
    m_surfelDirtyBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACITY, sizeof(uint32_t), kStorage);
    m_surfelRecycleBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACITY, sizeof(SurfelRecycleInfo), kStorage);
    m_surfelRayBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, MAX_RAY_COUNT, sizeof(SurfelRay), kStorage);

    // Seed the surfel free-list: every slot starts dead. surfel_generation_pass
    // consumes m_surfelDeadBuffer from the back (kMaxSurfelCount - i - 1), so
    // the stack is [0..N-1] and deadSurfelCnt = N marks the top of the stack.
    {
      auto *dead = static_cast<uint32_t *>(m_surfelDeadBuffer.mappedAddress);
      for (uint32_t i = 0; i < SURFEL_MAX_CAPACITY; ++i) {
        dead[i] = i;
      }
      auto *counter =
          static_cast<SurfelCounter *>(m_surfelCounterBuffer.mappedAddress);
      counter->aliveSurfelCnt = 0;
      counter->deadSurfelCnt = SURFEL_MAX_CAPACITY;
      counter->dirtySurfelCnt = 0;
      counter->surfelRayCnt = 0;
    }

    // Cell-grid SSBOs (bindless.resource.glsl set=0 bindings 17..19). Sizing
    // from forward_shared.h: TOTAL_CELL_COUNT cells of CellInfo (uniform cube
    // CELL_COUNT + 6 frustum-face regions of CELL_FRUSTUM_LAYERS log-spaced
    // slices), a single CellCounter, and a flat CELL_TO_SURFEL_CAPACITY-entry
    // uint table. Sizing only the central cube would OOB on any worldPos
    // farther than d/2 (=48u) from the camera — getFlattenCellIndexNonUniform
    // returns indices in [CELL_COUNT, TOTAL_CELL_COUNT) for those pixels.
    m_cellInfoBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, TOTAL_CELL_COUNT, sizeof(CellInfo), kStorage);
    m_cellCounterBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, 1, sizeof(CellCounter), kStorage);
    m_cellToSurfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, CELL_TO_SURFEL_CAPACITY, sizeof(uint32_t), kStorage);

    {
      auto *cellCnt =
          static_cast<CellCounter *>(m_cellCounterBuffer.mappedAddress);
      cellCnt->totalCellCount = TOTAL_CELL_COUNT;
      cellCnt->aliveSurfelInCell = 0;
    }

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

      bci.size = (VkDeviceSize)POINT_SLOT_LIGHT_CAPACITY * sizeof(PointLight);
      VK_WrapResult(vmaCreateBuffer(
          RI.device.vk.vmaAllocator, &bci, &aci, &m_pointLightBuffer.vk.buffer,
          &m_pointLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)SPOT_SLOT_LIGHT_CAPACITY * sizeof(SpotLight);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_spotLightBuffer.vk.buffer,
                                    &m_spotLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)BOX_SLOT_LIGHT_CAPACITY * sizeof(BoxLight);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_boxLightBuffer.vk.buffer,
                                    &m_boxLightBuffer.vk.allocation, nullptr));
    }

    // Surfel-generation output image — one storage texture per swapchain
    // image. RGBA16F so HDR radiance survives; SAMPLED so a future
    // composite pass can read it back. View aspect color, full mip 0.
    // Sized at HALF the swapchain resolution because surfel_generation_pass
    // dispatches at imageRes = viewportSize/2 and writes imageStore at
    // imageCoords ∈ [0, halfW)×[0, halfH). A full-res allocation would leave
    // 3/4 of the texture uninitialised, and visibility_shade.frag samples at
    // uv01 across the full [0,1] range — so it would read mostly garbage and
    // surfelIndirect would appear to contribute nothing. The linear sampler
    // in the composite still upsamples back to swapchain resolution.
    m_surfelResultWidth = (RI.swapchain.width + 1u) / 2u;
    m_surfelResultHeight = (RI.swapchain.height + 1u) / 2u;
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
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

    // Surfel-ray irradiance atlas — single-channel R16F, 4096x4096 fits
    // SURFEL_MAX_CAPACITY surfels at 6x6 cells each (the shader computes
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

    m_materialBindless.reset(MATERIAL_SLOT_CAPACITY);
    m_opaqueMaterialBuffer =
        detail::CreateBindlessSlotBuffer(&RI.device, MATERIAL_SLOT_CAPACITY,
                                         sizeof(DiffuseMaterialGPU), kStorage);

    // Default linear/wrap sampler for all bindless texture fetches. The
    // engine's filter cache (RIBootstrap::resolve_filter_descriptor) hands
    // back a finalized RIDescriptor_s with a non-zero cookie, which is
    // exactly what bindDescriptors needs.
    m_materialSampler = RI.resolve_filter_descriptor(
        eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
        eTextureFilter_Trilinear);

    {
      const VkDeviceSize kOpaqueHandleRange =
          OBJECT_SLOT_CAPACITY * sizeof(VkDeviceAddress);
      const struct {
        uint32_t binding;
        RIBuffer_s *buffer;
        VkDeviceSize range;
      } ssbos[] = {
          {BINDING_OPAQUE_POSITION_HANDLES, &m_opaquePositionHandles,
           kOpaqueHandleRange},
          {BINDING_OPAQUE_TANGENT_HANDLES, &m_opaqueTangentHandles,
           kOpaqueHandleRange},
          {BINDING_OPAQUE_NORMAL_HANDLES, &m_opaqueNormalHandles,
           kOpaqueHandleRange},
          {BINDING_OPAQUE_UV0_HANDLES, &m_opaqueUv0Handles, kOpaqueHandleRange},
          {BINDING_OPAQUE_COLOR_HANDLES, &m_opaqueColorHandles,
           kOpaqueHandleRange},
          {BINDING_OPAQUE_INDEX_HANDLES, &m_opaqueIndexHandles,
           kOpaqueHandleRange},
          {BINDING_SURFEL_COUNTER, &m_surfelCounterBuffer,
           sizeof(SurfelCounter)},
          {BINDING_SURFEL_BUFFER, &m_surfelBuffer,
           SURFEL_MAX_CAPACITY * sizeof(Surfel)},
          {BINDING_SURFEL_ALIVE, &m_surfelAliveBuffer,
           SURFEL_MAX_CAPACITY * sizeof(uint32_t)},
          {BINDING_SURFEL_DEAD, &m_surfelDeadBuffer,
           SURFEL_MAX_CAPACITY * sizeof(uint32_t)},
          {BINDING_SURFEL_DIRTY, &m_surfelDirtyBuffer,
           SURFEL_MAX_CAPACITY * sizeof(uint32_t)},
          {BINDING_SURFEL_RECYCLE, &m_surfelRecycleBuffer,
           SURFEL_MAX_CAPACITY * sizeof(SurfelRecycleInfo)},
          {BINDING_SURFEL_RAY, &m_surfelRayBuffer,
           MAX_RAY_COUNT * sizeof(SurfelRay)},
          {BINDING_CELL_BUFFER, &m_cellInfoBuffer,
           TOTAL_CELL_COUNT * sizeof(CellInfo)},
          {BINDING_CELL_COUNTER, &m_cellCounterBuffer, sizeof(CellCounter)},
          {BINDING_CELL_TO_SURFEL, &m_cellToSurfelBuffer,
           CELL_TO_SURFEL_CAPACITY * sizeof(uint32_t)},
          {BINDING_SCENE_OBJECTS, &m_diffuseObjectBuffer,
           OBJECT_SLOT_CAPACITY * sizeof(ObjectGPUData)},
          {BINDING_OPAQUE_MATERIAL, &m_opaqueMaterialBuffer,
           MATERIAL_SLOT_CAPACITY * sizeof(DiffuseMaterialGPU)},
          {BINDING_POINT_LIGHTS, &m_pointLightBuffer,
           POINT_SLOT_LIGHT_CAPACITY * sizeof(PointLight)},
          {BINDING_SPOT_LIGHTS, &m_spotLightBuffer,
           SPOT_SLOT_LIGHT_CAPACITY * sizeof(SpotLight)},
          {BINDING_BOX_LIGHTS, &m_boxLightBuffer,
           BOX_SLOT_LIGHT_CAPACITY * sizeof(BoxLight)},
      };

      RIBindlessDescriptorSet::WriteBinding writes[std::size(ssbos) + 1] = {};
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
      writes[count].binding = BINDING_MATERIAL_SAMPLER;
      writes[count].arrayElement = 0;
      writes[count].descriptor = *m_materialSampler;
      count++;
      m_bindlessSet.writeDescriptors(&RI.device,
                                     std::span(writes).subspan(0, count));
    }
  }
}

uint32_t cHybridRenderer::resolveTextureSlot(RIBootstrap::FrameContext *cntx,
                                             Image *img, uint32_t frameIndex) {
  if (!img)
    return INVALID_TEXTURE_INDEX;
  auto texture = img->GetTexture();
  if (!texture)
    return INVALID_TEXTURE_INDEX;

  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)texture.get());
  auto req = m_textureBindless.request(texture_cookie, frameIndex);
  if (req.exhausted)
    return INVALID_TEXTURE_INDEX;
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

uint32_t cHybridRenderer::resolveMaterial(RIBootstrap::FrameContext *cntx,
                                          cMaterial *mat, uint32_t frameIndex) {
  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    return resolveTextureSlot(cntx, mat->GetImage(type), frameIndex);
  };

  // Slot layout must match the DiffuseMaterial_*Texture_ID accessors in
  // amnesia/glsl/per_frame.resource.glsl. One uint32 per texture index.
  DiffuseMaterialGPU gpu = {};
  gpu.tex[0] = slotFor(eMaterialTexture_Diffuse);
  gpu.tex[1] = slotFor(eMaterialTexture_NMap);
  gpu.tex[2] = slotFor(eMaterialTexture_Alpha);
  gpu.tex[3] = slotFor(eMaterialTexture_Specular);
  gpu.tex[4] = slotFor(eMaterialTexture_Height);
  gpu.tex[5] = slotFor(eMaterialTexture_Illumination);
  gpu.tex[6] = slotFor(eMaterialTexture_DissolveAlpha);
  gpu.tex[7] = slotFor(eMaterialTexture_CubeMapAlpha);
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
  std::memcpy(static_cast<uint8_t *>(m_opaqueMaterialBuffer.mappedAddress) +
                  (size_t)req.id * sizeof(DiffuseMaterialGPU),
              &gpu, sizeof(gpu));

  return req.id;
}

void cHybridRenderer::Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

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
  }

  PerFrameConstants perFrame{};
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
    if (num_point_lights >= POINT_SLOT_LIGHT_CAPACITY) {
      Warning("Point-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    PointLight pl{};
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
    if (num_spot_lights >= SPOT_SLOT_LIGHT_CAPACITY) {
      Warning("Spot-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    cLightSpot *pSpot = static_cast<cLightSpot *>(pLight);
    SpotLight sl{};
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
    sl.color[0] = c.r;
    sl.color[1] = c.g;
    sl.color[2] = c.b;
    sl.intensity = c.a;
    // attenuationTextureIndex is reserved for a future LUT-driven radial
    // falloff (legacy used a 1D attenuationLightMap + 1D falloffMap pair).
    // The shader currently uses the analytic getRangeAttenuation × smooth
    // cone factor and never samples this slot, so leave it invalid.
    sl.attenuationTextureIndex = INVALID_TEXTURE_INDEX;
    sl.goboTextureIndex = resolveTextureSlot(cntx, pLight->GetGoboImage(),
                                             (uint32_t)RI.frameIndex);
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
    if (num_box_lights >= BOX_SLOT_LIGHT_CAPACITY) {
      Warning("Box-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    cLightBox *pBox = static_cast<cLightBox *>(pLight);
    BoxLight bl{};
    // Matches RendererDeferred's box-light proxy: world-aligned AABB at the
    // light's world position, ignoring any entity rotation. center+halfSize
    // makes the shader's containment test a single subtract + compare.
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
    // brightness lives in the rgb channels.
    const cColor c = pLight->GetDiffuseColor();
    bl.color[0] = c.r;
    bl.color[1] = c.g;
    bl.color[2] = c.b;
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

    ObjectGPUData payload{};
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
    if (!req.found) {
      std::memcpy(static_cast<uint8_t *>(m_diffuseObjectBuffer.mappedAddress) +
                      req.id * sizeof(ObjectGPUData),
                  &payload, sizeof(payload));

      // Per-stream VkDeviceAddress fan-out into the parallel handle buffers.
      // Missing streams write 0 — shaders branch on non-zero before deref.
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
      tlasToShader.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      tlasToShader.dstAccessMask =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep2.memoryBarrierCount = 1;
      dep2.pMemoryBarriers = &tlasToShader;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep2);
    }
  }

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

  // sceneObjectsBuf / opaqueMaterialBuf now live in the bindless set (set=0
  // bindings 20..21), wired up by bindBindlessDescriptorSet() below — no
  // per-draw pushBinding needed.

  {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("perFrame");
    RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
    bindings.push_back(b);
  }

  // Scene's rendering was already ended above (before the BLAS/TLAS work).
  // Transition the MRT targets and begin the MRT pass.
  {
    VkImageMemoryBarrier2 toColor[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (uint32_t i = 0; i < 2; ++i) {
      toColor[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
      toColor[i].srcAccessMask = 0;
      toColor[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      toColor[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      toColor[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      toColor[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      toColor[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toColor[0].image = RI.normalTexture[RI.swapchainIndex].vk.image;
    toColor[1].image = RI.visibilityTexture[RI.swapchainIndex].vk.image;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = toColor;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  VkRenderingAttachmentInfo colorAttachments[2] = {
      {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
      {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
  colorAttachments[0].imageView = RI.normalView[RI.swapchainIndex].vk.image;
  colorAttachments[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachments[0].clearValue.color = {{0.f, 0.f, 0.f, 0.f}};
  colorAttachments[1].imageView = RI.visibilityView[RI.swapchainIndex].vk.image;
  colorAttachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachments[1].clearValue.color.uint32[0] = 0;

  VkRenderingAttachmentInfo depthAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  // MRT now owns the per-frame depth clear (Scene no longer pre-clears).
  RI_VK_FillDepthAttachment(&depthAttachment, &RI.depthView[RI.swapchainIndex],
                            /*attachAndClear=*/true);

  // Surfel-grid prep — clears cellBuffer + a couple of counters at the top of
  // each frame, ahead of any future surfel/cell compute passes. Reads/writes
  // the bindless surfel+cell SSBOs (set=0 bindings 10..19); no push constants,
  // no per-pass descriptors. Must run outside any vkCmdBeginRendering scope.
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kSurfelPrepareHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelPrepare.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kSurfelPrepareHash,
        "hybrid.surfel_prepare", &computeCreate);
    m_surfelPrepare.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);
    // Cover the full grid (uniform cube + 6 frustum-face regions) — the
    // frustum cells need clearing every frame too, otherwise stale
    // surfelCount/surfelOffset values stick around and corrupt next frame's
    // cellInfo_update_pass offset accumulation.
    CmdDispatch(&RI.primary.cmds[0], (TOTAL_CELL_COUNT + 31u) / 32u, 1, 1);
  }

  VkRenderingInfo renderingInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderingInfo.renderArea = {{0, 0},
                              {RI.swapchain.width, RI.swapchain.height}};
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 2;
  renderingInfo.pColorAttachments = colorAttachments;
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
    GBufferMRTPipelineDesc pipelineDesc(RIBootstrap::NormalFormat,
                                        RIBootstrap::VisibilityFormat,
                                        RIBootstrap::DepthFormat);
    m_gbuffer.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                           "hybrid.gbuffer_mrt", &pipelineDesc.createInfo);
    m_gbuffer.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet, 0);
    m_gbuffer.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                              bindings.data(), bindings.size());
    CmdDrawIndirect(&RI.primary.cmds[0], &m_indirectDrawBuffer,
                    (VkDeviceSize)indirectReq.elementOffset *
                        sizeof(VkDrawIndirectCommand),
                    writtenDraws, (uint32_t)sizeof(VkDrawIndirectCommand));
  }

  vkCmdEndRendering(cmd);

  // Gbuffer outputs -> SHADER_READ_ONLY for the surfel-generation compute
  // pass (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The surfel result
  // image transitions UNDEFINED -> GENERAL for its first compute write.
  {
    VkImageMemoryBarrier2 toRead[4] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (uint32_t i = 0; i < 2; ++i) {
      toRead[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      toRead[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      toRead[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      toRead[i].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      toRead[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toRead[0].image = RI.normalTexture[RI.swapchainIndex].vk.image;
    toRead[1].image = RI.visibilityTexture[RI.swapchainIndex].vk.image;

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[2].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toRead[2].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toRead[2].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[2].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead[2].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    toRead[2].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead[2].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    toRead[2].image = RI.depthTextures[RI.swapchainIndex].vk.image;

    // Surfel-result image: first frame transitions from UNDEFINED; later
    // frames are fine to redeclare UNDEFINED here too because the engine
    // frame fence guarantees the previous use has completed before we
    // overwrite, and we discard prior contents (the shader writes every
    // touched texel from scratch).
    toRead[3].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toRead[3].srcAccessMask = 0;
    toRead[3].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[3].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toRead[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toRead[3].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead[3].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead[3].image = m_surfelResultTexture[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 4;
    dep.pImageMemoryBarriers = toRead;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // One-shot UNDEFINED -> GENERAL transition for both surfel atlases the
  // first time each swapchain image appears. Subsequent frames skip this —
  // the atlas stays in GENERAL for life so integrate's EMA-blend reads keep
  // working and cross-frame sync goes through the engine frame fence.
  if (!m_surfelAtlasesInitialized[RI.swapchainIndex]) {
    VkImageMemoryBarrier2 toGeneral[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (uint32_t i = 0; i < 2; ++i) {
      toGeneral[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
      toGeneral[i].srcAccessMask = 0;
      toGeneral[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
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

  // surfel_update atomicAdds into cellBuffer[].surfelCount that surfel_prepare
  // just zeroed. The clear has to be visible to the per-surfel scatter before
  // it starts incrementing, so gate the next dispatch on prepare's writes.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kSurfelUpdateHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                       kSurfelUpdateHash,
                                       "hybrid.surfel_update", &computeCreate);
    m_surfelUpdate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> computeBindings;
    computeBindings.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("perFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      computeBindings.push_back(b);
    }

    m_surfelUpdate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, computeBindings.data(),
        computeBindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    CmdDispatch(&RI.primary.cmds[0], (SURFEL_MAX_CAPACITY + 31u) / 32u, 1u, 1u);
  }

  // m_cellInfoUpdate reads cellBuffer[].surfelCount that m_surfelUpdate just
  // wrote. Same global compute-shader storage barrier shape as the previous
  // one between generate and update.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kCellInfoUpdateHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_cellInfoUpdate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kCellInfoUpdateHash,
        "hybrid.cell_info_update", &computeCreate);
    m_cellInfoUpdate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);
    // One thread per cell across the full non-uniform grid. The shader's
    // `idx >= TOTAL_CELL_COUNT` early-out culls excess invocations from the
    // workgroup rounding.
    CmdDispatch(&RI.primary.cmds[0], (TOTAL_CELL_COUNT + 31u) / 32u, 1u, 1u);
  }

  // m_cellToSurfelUpdate reads cellBuffer[].surfelOffset that m_cellInfoUpdate
  // just wrote, then atomically increments cellBuffer[].surfelCount back up to
  // populate cellToSurfel[]. Same compute-shader SSBO hazard pattern as the
  // previous dispatches.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Per-surfel scatter into cellToSurfel[]. aliveSurfelCnt lives on the GPU
  // so we dispatch the SURFEL_MAX_CAPACITY upper bound and let the shader's
  // `idx >= surfelCounter.aliveSurfelCnt` early-out cull excess invocations.
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kCellToSurfelUpdateHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_cellToSurfelUpdate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kCellToSurfelUpdateHash,
        "hybrid.cell_to_surfel_update", &computeCreate);
    m_cellToSurfelUpdate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], (SURFEL_MAX_CAPACITY + 31u) / 32u, 1u, 1u);
  }

  // m_surfelRaytrace reads cellBuffer[].surfelCount/surfelOffset and the
  // cellToSurfel[] mapping that m_cellToSurfelUpdate just populated.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Surfel atlases (irradiance + depth) are already in GENERAL at this
  // point — the one-shot UNDEFINED -> GENERAL transition ran earlier this
  // frame. Raytrace samples them at GENERAL, integrate reads-and-writes them
  // at GENERAL, and the relocated surfel_generate pass after integrate keeps
  // them in GENERAL too. No further layout change needed.

  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kSurfelRaytraceHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelRaytrace.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kSurfelRaytraceHash,
        "hybrid.surfel_raytrace", &computeCreate);
    m_surfelRaytrace.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> raytraceBindings;
    raytraceBindings.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("perFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      raytraceBindings.push_back(b);
    }
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("topLevelAS");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      b.descriptor.vk.accelStructure = m_tlas.vk.handle;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      raytraceBindings.push_back(b);
    }
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("surfelIrradianceSampler");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b.descriptor.vk.image.sampler = m_materialSampler->vk.image.sampler;
      b.descriptor.vk.image.imageView =
          m_surfelIrradianceView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      raytraceBindings.push_back(b);
    }

    m_surfelRaytrace.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, raytraceBindings.data(),
        raytraceBindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    // One thread per pending surfel ray. surfelCounter.surfelRayCnt early-outs
    // the tail; overdispatching to MAX_RAY_COUNT is fine.
    CmdDispatch(&RI.primary.cmds[0], (MAX_RAY_COUNT + 31u) / 32u, 1u, 1u);
  }

  // surfel_integrate consumes m_surfelRayBuffer.radiance that the raytrace
  // dispatch just wrote. Same compute->compute SSBO barrier shape as the
  // earlier passes; integrate also samples the irradiance atlas, but since
  // it's the first reader after the raytrace and raytrace doesn't write
  // either atlas, no image barrier is needed between them.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Per-surfel integrate: folds raytraced radiance into MSME, EMA-blends
  // the octahedral irradiance + depth tiles, shares radiance across cell
  // neighbours, and writes the per-surfel irradiance gate that
  // surfel_raytrace.comp's ray-guiding branch reads next frame.
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kSurfelIntegrateHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelIntegrate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kSurfelIntegrateHash,
        "hybrid.surfel_integrate", &computeCreate);
    m_surfelIntegrate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> integrateBindings;
    integrateBindings.reserve(5);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("perFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      integrateBindings.push_back(b);
    }
    auto pushAtlas = [&](const char *samplerName, const char *imageName,
                         VkImageView view) {
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(samplerName);
        b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptor.vk.image.sampler = m_materialSampler->vk.image.sampler;
        b.descriptor.vk.image.imageView = view;
        b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        RIFinalizeDescriptor(&RI.device, &b.descriptor);
        integrateBindings.push_back(b);
      }
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(imageName);
        b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
        b.descriptor.vk.image.imageView = view;
        b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        RIFinalizeDescriptor(&RI.device, &b.descriptor);
        integrateBindings.push_back(b);
      }
    };
    pushAtlas("surfelIrradianceSampler", "surfelIrradianceMap",
              m_surfelIrradianceView[RI.swapchainIndex].vk.image);
    pushAtlas("surfelDepthSampler", "surfelDepthMap",
              m_surfelDepthView[RI.swapchainIndex].vk.image);

    m_surfelIntegrate.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                      RI.frameIndex, integrateBindings.data(),
                                      integrateBindings.size(),
                                      VK_PIPELINE_BIND_POINT_COMPUTE);

    // One thread per surfel slot; the shader's
    // `index >= surfelCounter.aliveSurfelCnt` early-out culls the tail.
    CmdDispatch(&RI.primary.cmds[0], (SURFEL_MAX_CAPACITY + 31u) / 32u, 1u, 1u);
  }

  // surfel_generate runs last in the surfel chain — it gathers per-pixel
  // coverage by walking cellBuffer[]/cellToSurfel[] (populated this frame by
  // surfel_update + cellInfo_update + cellToSurfel_update) and samples each
  // covering surfel's radiance (just refreshed by surfel_integrate). Running
  // it earlier would read the cleared cellBuffer and produce zero coverage
  // everywhere, causing uncontrolled spawning and zero-radiance surfels.
  // Matches SurfelPlus reference (sample_example.cpp::calculateSurfels).
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kSurfelGenerateHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGenerate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kSurfelGenerateHash,
        "hybrid.surfel_generate", &computeCreate);
    m_surfelGenerate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0, VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> computeBindings;
    computeBindings.reserve(5);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("perFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      computeBindings.push_back(b);
    }
    auto pushImage = [&](const char *name, VkImageView view,
                         VkImageLayout layout, VkDescriptorType type,
                         VkSampler sampler) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create(name);
      b.descriptor.vk.type = type;
      b.descriptor.vk.image.sampler = sampler;
      b.descriptor.vk.image.imageView = view;
      b.descriptor.vk.image.imageLayout = layout;
      // RIFinalizeDescriptor stamps a non-zero cookie. Without it
      // RI_IsEmptyDescriptor returns true and bindDescriptors silently
      // skips the binding, leaving the shader's set unbound.
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      computeBindings.push_back(b);
    };
    const VkSampler sampler = m_materialSampler->vk.image.sampler;
    // Integer formats (R32_UINT for visibility + normal) can't be linearly
    // filtered. Use a NEAREST sampler for those two; depth stays on the
    // trilinear sampler (D32_SFLOAT supports linear filtering).
    RIDescriptor_s *nearestSamplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Nearest);
    const VkSampler nearestSampler = nearestSamplerDesc->vk.image.sampler;
    pushImage("visibilityBuffer", RI.visibilityView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nearestSampler);
    pushImage("normalMap", RI.normalView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nearestSampler);
    pushImage("depthMap", RI.depthView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler);
    pushImage("resultImage", m_surfelResultView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              VK_NULL_HANDLE);

    m_surfelGenerate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, computeBindings.data(),
        computeBindings.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    const uint32_t halfW = RI.swapchain.width / 2u;
    const uint32_t halfH = RI.swapchain.height / 2u;
    CmdDispatch(&RI.primary.cmds[0], (halfW + 15u) / 16u, (halfH + 15u) / 16u,
                1u);
  }

  // --------------------------------------------------------------------
  // Final composite (visibility-buffer shade pass).
  //
  // Port of AmnesiaTheDarkDescent's visibility_emit_shade_pass.frag.fsl,
  // adapted to the bindless layout. Runs as a fullscreen triangle that
  // unpacks (objectID, primID) from the visibility buffer, reconstructs
  // the triangle data per pixel, samples diffuse textures with analytical
  // mip gradients, adds point-light direct lighting, gathers surfel GI,
  // and writes the swapchain image.
  //
  // Two hazards to sync before issuing the fragment work:
  //   (a) The surfel compute passes that just ran wrote surfelBuffer /
  //       cellBuffer / cellToSurfel; the fragment shader reads them.
  //   (b) The swapchain image needs to be in COLOR_ATTACHMENT_OPTIMAL
  //       for vkCmdBeginRendering to write to it.
  // --------------------------------------------------------------------
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkImageMemoryBarrier2 imageBarriers[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};

    VkImageMemoryBarrier2 &swapToColor = imageBarriers[0];
    swapToColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    swapToColor.srcAccessMask = 0;
    swapToColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    swapToColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    swapToColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    swapToColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapToColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapToColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    swapToColor.image = RI.swapchain.vk.images[RI.swapchainIndex];
    swapToColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Surfel-indirect map: surfel_generation_pass wrote it as a storage image
    // in GENERAL; the visibility-shade fragment shader samples it as a
    // sampler2D. Move it to SHADER_READ_ONLY_OPTIMAL and gate the fragment
    // sampled-read on the compute storage-write. The next frame's pre-gen
    // barrier (HybridRenderer.cpp earlier in Draw) re-acquires it as
    // UNDEFINED -> GENERAL, which is fine because surfel-generation
    // overwrites every touched texel.
    VkImageMemoryBarrier2 &surfelToRead = imageBarriers[1];
    surfelToRead.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    surfelToRead.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    surfelToRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    surfelToRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    surfelToRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    surfelToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    surfelToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    surfelToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    surfelToRead.image = m_surfelResultTexture[RI.swapchainIndex].vk.image;
    surfelToRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = imageBarriers;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  {
    VkRenderingAttachmentInfo colorAttach = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttach.imageView = RI.swapchainView[RI.swapchainIndex].vk.image;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAttach;
    renderInfo.pDepthAttachment = nullptr;

    vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

    VkViewport vp = {0.0f,
                     (float)RI.swapchain.height,
                     (float)RI.swapchain.width,
                     -(float)RI.swapchain.height,
                     0.0f,
                     1.0f};
    VkRect2D scissor = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
    vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
    vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &scissor);

    VisibilityShadePipelineDesc shadeDesc((RI_Format_e)RI.swapchain.format);
    m_visibilityShade.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                   shadeDesc.hash, "hybrid.visibility_shade",
                                   &shadeDesc.createInfo);
    m_visibilityShade.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                                &m_bindlessSet, 0);

    std::vector<RIProgram::DescriptorBinding> bindings;
    bindings.reserve(6);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("perFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bindings.push_back(b);
    }
    // Both visibility + normal are R32_UINT — must be sampled with a
    // NEAREST filter (integer formats can't be linearly filtered).
    RIDescriptor_s *nearestDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Nearest);
    const VkSampler nearestSampler = nearestDesc->vk.image.sampler;
    const VkSampler linearSampler = m_materialSampler->vk.image.sampler;
    auto pushSampledImage = [&](const char *name, VkImageView view,
                                VkSampler sampler, VkImageLayout layout) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create(name);
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b.descriptor.vk.image.sampler = sampler;
      b.descriptor.vk.image.imageView = view;
      b.descriptor.vk.image.imageLayout = layout;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bindings.push_back(b);
    };
    pushSampledImage("visibilityBuffer",
                     RI.visibilityView[RI.swapchainIndex].vk.image,
                     nearestSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pushSampledImage("normalBufferTex",
                     RI.normalView[RI.swapchainIndex].vk.image, nearestSampler,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pushSampledImage("depthBufferTex", RI.depthView[RI.swapchainIndex].vk.image,
                     linearSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Half-res surfel indirect-lighting map written by surfel_generation_pass.
    // Linear sampler so the fullscreen composite bilinearly upsamples from
    // half-res to swapchain resolution (visibility_shade.frag:42-44). The
    // image is transitioned GENERAL -> SHADER_READ_ONLY_OPTIMAL by the
    // barrier block above this vkCmdBeginRendering.
    pushSampledImage("surfelIndirect",
                     m_surfelResultView[RI.swapchainIndex].vk.image,
                     linearSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("topLevelAS");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      b.descriptor.vk.accelStructure = m_tlas.vk.handle;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bindings.push_back(b);
    }

    m_visibilityShade.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                      RI.frameIndex, bindings.data(),
                                      bindings.size());

    vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3u, 1u, 0u, 0u);

    vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
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
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_surfelResultTexture[i].vk.image,
                      m_surfelResultTexture[i].vk.allocation);
      m_surfelResultTexture[i] = {};
    }
    if (m_surfelIrradianceView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device,
                         m_surfelIrradianceView[i].vk.image, NULL);
      m_surfelIrradianceView[i] = {};
    }
    if (m_surfelIrradianceTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_surfelIrradianceTexture[i].vk.image,
                      m_surfelIrradianceTexture[i].vk.allocation);
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
