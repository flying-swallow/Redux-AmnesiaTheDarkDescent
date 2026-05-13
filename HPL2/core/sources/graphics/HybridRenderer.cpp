#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialResource.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/Renderable.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer_RI.h"
#include "math/Frustum.h"
#include "math/Math.h"

#include "resources/Resources.h"
#include "scene/RenderableContainer.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
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

    blendAttachments[0] = {VK_TRUE,
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
        VK_TRUE,         VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
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

} // namespace

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources, 0),
      m_diffuseBindless(OBJECT_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT),
      m_textureBindless(TEXTURE_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT),
      m_materialBindless(MATERIAL_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT) {
  {
    {
      std::vector<RIBindlessDescriptorSet::Binding> bindings = {};
      // textures_2d[]
      bindings.push_back((RIBindlessDescriptorSet::Binding){
          BINDING_TEXTURES_2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          TEXTURE_SLOT_CAPACITY,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
      // opaque*Handles bindings 3..8
      for (uint32_t i = 0; i < 6; ++i) {
        bindings.push_back((RIBindlessDescriptorSet::Binding){
            BINDING_OPAQUE_POSITION_HANDLES + i,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      }
      // materialSampler
      bindings.push_back((RIBindlessDescriptorSet::Binding){
          BINDING_MATERIAL_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      // Surfel-GI SSBOs (bindings 10..16) + cell-grid SSBOs (bindings 17..19).
      // Used by compute (surfel_*.comp) and sampled in fragment shading once
      // the GI path lights surfaces.
      const uint32_t kSurfelCellBindings[] = {
          BINDING_SURFEL_COUNTER, BINDING_SURFEL_BUFFER,  BINDING_SURFEL_ALIVE,
          BINDING_SURFEL_DEAD,    BINDING_SURFEL_DIRTY,   BINDING_SURFEL_RECYCLE,
          BINDING_SURFEL_RAY,
          BINDING_CELL_BUFFER,    BINDING_CELL_COUNTER,   BINDING_CELL_TO_SURFEL,
      };
      for (uint32_t b : kSurfelCellBindings) {
        bindings.push_back((RIBindlessDescriptorSet::Binding){
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      }
      // Scene-object + opaque-material tables (bindings 20..21). Read by the
      // gbuffer pipeline (VS reads sceneObjects, FS reads opaqueMaterial).
      const uint32_t kSceneTableBindings[] = {
          BINDING_SCENE_OBJECTS, BINDING_OPAQUE_MATERIAL,
      };
      for (uint32_t b : kSceneTableBindings) {
        bindings.push_back((RIBindlessDescriptorSet::Binding){
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0});
      }

      VkDescriptorPoolSize poolSizes[3] = {};
      poolSizes[0] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          TEXTURE_SLOT_CAPACITY};
      // 6 opaque*Handles + 7 surfel + 3 cell + 2 scene/material = 18.
      poolSizes[1] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18};
      poolSizes[2] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1};

      m_bindlessSet.initialize(&RI.device, bindings, poolSizes);
    }

    const VkDescriptorSetLayout externalLayouts[] = {m_bindlessSet.vk.m_bindlessSetLayout};
    {
      auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                  "gbuffer.vert.spv");
      auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                  "gbuffer.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage}};
      m_gbuffer.initialize(&RI.device, stages, externalLayouts);
    }

    {
      auto comp_prepare = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                  "surfel_prepare.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_COMPUTE, comp_prepare}};
      m_surfelPrepare.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto comp_generate = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                  "surfel_generation_pass.comp.spv");
      std::array<RIProgram::ModuleStage, 1> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_COMPUTE, comp_generate}};
      m_surfelGenerate.initialize(&RI.device, stages, externalLayouts);
    }

    const VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    m_diffuseBindless.reset(OBJECT_SLOT_CAPACITY);
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
        &RI.device, SURFEL_MAX_CAPACTIY, sizeof(Surfel), kStorage);
    m_surfelAliveBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACTIY, sizeof(uint32_t), kStorage);
    m_surfelDeadBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACTIY, sizeof(uint32_t), kStorage);
    m_surfelDirtyBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACTIY, sizeof(uint32_t), kStorage);
    m_surfelRecycleBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, SURFEL_MAX_CAPACTIY, sizeof(SurfelRecycleInfo), kStorage);
    m_surfelRayBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, MAX_RAY_COUNT, sizeof(SurfelRay), kStorage);

    // Seed the surfel free-list: every slot starts dead. surfel_generation_pass
    // consumes m_surfelDeadBuffer from the back (kMaxSurfelCount - i - 1), so
    // the stack is [0..N-1] and deadSurfelCnt = N marks the top of the stack.
    {
      auto *dead = static_cast<uint32_t *>(m_surfelDeadBuffer.mappedAddress);
      for (uint32_t i = 0; i < SURFEL_MAX_CAPACTIY; ++i) {
        dead[i] = i;
      }
      auto *counter =
          static_cast<SurfelCounter *>(m_surfelCounterBuffer.mappedAddress);
      counter->aliveSurfelCnt = 0;
      counter->deadSurfelCnt = SURFEL_MAX_CAPACTIY;
      counter->dirtySurfelCnt = 0;
      counter->surfelRayCnt = 0;
    }

    // Cell-grid SSBOs (bindless.resource.glsl set=0 bindings 17..19). Sizing
    // from forward_shared.h: CELL_COUNT (64^3) cells of CellInfo, a single
    // CellCounter, and a flat CELL_TO_SURFEL_CAPACITY-entry uint table.
    m_cellInfoBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, CELL_COUNT, sizeof(CellInfo), kStorage);
    m_cellCounterBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, 1, sizeof(CellCounter), kStorage);
    m_cellToSurfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, CELL_TO_SURFEL_CAPACITY, sizeof(uint32_t), kStorage);

    // Surfel-generation output image — one storage texture per swapchain
    // image. RGBA16F so HDR radiance survives; SAMPLED so a future
    // composite pass can read it back. View aspect color, full mip 0.
    m_surfelResultWidth = RI.swapchain.width;
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
      imgInfo.usage =
          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
          {BINDING_OPAQUE_POSITION_HANDLES, &m_opaquePositionHandles, kOpaqueHandleRange},
          {BINDING_OPAQUE_TANGENT_HANDLES,  &m_opaqueTangentHandles,  kOpaqueHandleRange},
          {BINDING_OPAQUE_NORMAL_HANDLES,   &m_opaqueNormalHandles,   kOpaqueHandleRange},
          {BINDING_OPAQUE_UV0_HANDLES,      &m_opaqueUv0Handles,      kOpaqueHandleRange},
          {BINDING_OPAQUE_COLOR_HANDLES,    &m_opaqueColorHandles,    kOpaqueHandleRange},
          {BINDING_OPAQUE_INDEX_HANDLES,    &m_opaqueIndexHandles,    kOpaqueHandleRange},
          {BINDING_SURFEL_COUNTER,  &m_surfelCounterBuffer,  sizeof(SurfelCounter)},
          {BINDING_SURFEL_BUFFER,   &m_surfelBuffer,         SURFEL_MAX_CAPACTIY * sizeof(Surfel)},
          {BINDING_SURFEL_ALIVE,    &m_surfelAliveBuffer,    SURFEL_MAX_CAPACTIY * sizeof(uint32_t)},
          {BINDING_SURFEL_DEAD,     &m_surfelDeadBuffer,     SURFEL_MAX_CAPACTIY * sizeof(uint32_t)},
          {BINDING_SURFEL_DIRTY,    &m_surfelDirtyBuffer,    SURFEL_MAX_CAPACTIY * sizeof(uint32_t)},
          {BINDING_SURFEL_RECYCLE,  &m_surfelRecycleBuffer,  SURFEL_MAX_CAPACTIY * sizeof(SurfelRecycleInfo)},
          {BINDING_SURFEL_RAY,      &m_surfelRayBuffer,      MAX_RAY_COUNT       * sizeof(SurfelRay)},
          {BINDING_CELL_BUFFER,     &m_cellInfoBuffer,       CELL_COUNT              * sizeof(CellInfo)},
          {BINDING_CELL_COUNTER,    &m_cellCounterBuffer,    sizeof(CellCounter)},
          {BINDING_CELL_TO_SURFEL,  &m_cellToSurfelBuffer,   CELL_TO_SURFEL_CAPACITY * sizeof(uint32_t)},
          {BINDING_SCENE_OBJECTS,   &m_diffuseObjectBuffer,  OBJECT_SLOT_CAPACITY    * sizeof(ObjectGPUData)},
          {BINDING_OPAQUE_MATERIAL, &m_opaqueMaterialBuffer, MATERIAL_SLOT_CAPACITY  * sizeof(DiffuseMaterialGPU)},
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
      m_bindlessSet.writeDescriptors(&RI.device, std::span(writes).subspan(0, count));
    }
  }
}

uint32_t cHybridRenderer::resolveMaterial(RIBootstrap::FrameContext *cntx,
                                          cMaterial *mat, uint32_t frameIndex) {
  hash_t cookie = hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)mat);
  cookie = hash_u64(cookie, (uint64_t)mat->Generation());

  auto req = m_materialBindless.request(cookie, frameIndex);
  if (req.exhausted)
    return UINT32_MAX;
  if (req.found)
    return req.id;

  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    Image *img = mat->GetImage(type);
    if (!img)
      return INVALID_TEXTURE_INDEX;
    auto texture = img->GetTexture();

    const hash_t cookie =
        hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)texture.get());
    auto req = m_textureBindless.request(cookie, frameIndex);
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
      binding.descriptor.vk.image.imageView =
          texture->binding.vk.image.imageView;
      binding.descriptor.vk.image.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      m_bindlessSet.writeDescriptors(&RI.device, {&binding, 1});
    }
    return req.id;
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
  auto solids = m_rendererList.GetSolidObjects();

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
  // Fog params + worldFogColor + invViewRotationMat default to zero — fine for
  // the first pass; populate when the deferred-fog path needs them.

  // Ring-allocate one VkDrawIndirectCommand per visible renderable. On wrap /
  // exhaustion, drop the frame's dispatch — reallocation grows the underlying
  // buffer, but for now we cap at maxElements.
  RISegmentReq_s indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(RI.frameIndex, solids.size(), &indirectReq);
  auto *indirectDst =
      indirectOk
          ? reinterpret_cast<VkDrawIndirectCommand *>(
                static_cast<uint8_t *>(m_indirectDrawBuffer.mappedAddress) +
                (size_t)indirectReq.elementOffset *
                    sizeof(VkDrawIndirectCommand))
          : nullptr;
  uint32_t writtenDraws = 0;

  // TLAS instance accumulator. Built up alongside the indirect-draw write so
  // visible solids whose BLAS we successfully resolve land in the TLAS exactly
  // once.
  const bool rtEnabled = RI.device.physicalAdapter.isRayTracingSupported;
  std::vector<RIAccelInstance_s> tlasInstances;
  if (rtEnabled)
    tlasInstances.reserve(solids.size());

  // Contract: Scene::Render hands control to Draw with NO active rendering
  // instance and expects none on return. The MRT pass below opens and closes
  // its own rendering in one tight block.

  for (iRenderable *pObject : solids) {
    cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
    iVertexBuffer *pVB = pObject->GetVertexBuffer();
    cMaterial *pMat = pObject->GetMaterial();
    if (!pVB || !pMat)
      continue;

    const uint32_t materialSlot =
        resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
    if (materialSlot == UINT32_MAX) {
      Warning("Material Slot exhausted");
      // Material pool exhausted — drop this draw rather than indexing OOB.
      continue;
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

    const hash_t cookie =
        hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)pObject);
    const hash_t payloadHash =
        hash_data(HASH_INITIAL_VALUE, &payload, sizeof(payload));
    auto req = m_diffuseBindless.request(cookie, (uint32_t)RI.frameIndex);
    if (req.exhausted) {
      // TODO: will probably resize the buffer and goto the beginning and
      // reconstruct the data
      Error("bindless pool is exhausted");
    }

    auto *vb = static_cast<VertexBuffer_RI *>(pVB);
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

    // Keep the VertexBuffer_RI's underlying RIBuffer_s shared_ptrs alive at
    // least until this frame's set rotates back around — the GPU dereferences
    // their BDAs in the indirect draw, and if the renderable is destroyed
    // mid-flight the shared_ptr deleter would otherwise free them too early.
    {
      auto *cntx = RI.GetActiveSet();
      const eVertexBufferElement attrs[] = {
          eVertexBufferElement_Position, eVertexBufferElement_Texture1Tangent,
          eVertexBufferElement_Normal,   eVertexBufferElement_Texture0,
          eVertexBufferElement_Color0,
      };
      for (auto type : attrs) {
        const auto *element = vb->GetElement(type);
        if (element && element->buffer)
          cntx->bufferLink.push_back(element->buffer);
      }
      if (auto idx = vb->GetIndexRIBuffer())
        cntx->bufferLink.push_back(idx);
    }

    // firstInstance carries the slot id to the VS via gl_InstanceIndex;
    // the VS pulls vertex / index data via BDA from the bindless set 0 SSBOs,
    // so vertexCount is the index count (one VS invocation per index).
    if (indirectDst && writtenDraws < indirectReq.numElements) {
      indirectDst[writtenDraws++] = VkDrawIndirectCommand{
          /*vertexCount   =*/(uint32_t)pVB->GetIndexNum(),
          /*instanceCount =*/1u,
          /*firstVertex   =*/0u,
          /*firstInstance =*/req.id,
      };
    }

    // Lazy BLAS build + TLAS instance emission. GetOrBuildBlas records into the
    // same primary cmd buffer the forward pass uses; an accel-build→accel-build
    // barrier below guarantees the TLAS read sees the BLAS writes.
    if (rtEnabled) {
      RIAccelStructure_s *blas =
          vb->GetOrBuildBlas(&RI.device, &RI.primary.cmds[0]);
      if (blas && blas->vk.handle != VK_NULL_HANDLE) {
        // VkAccelerationStructureInstanceKHR is row-major 3x4
        // (transform[row][col]). ml::float4x4 from ToFloatTranspose4x4 is
        // column-major; the transpose path above already produced row-major
        // data in payload.modelMat (16 floats), so copy the first 12 directly
        // into the instance matrix.
        RIAccelInstance_s inst = {};
        // payload.modelMat is row-major 4x4; we want rows 0..2 cols 0..3.
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 4; ++c) {
            inst.matrix[r][c] = payload.modelMat[r * 4 + c];
          }
        }
        inst.instanceCustomIndex = req.id;
        inst.mask = 0xFF;
        inst.shaderBindingTableRecordOffset = 0;
        inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE;
        inst.accelerationStructureDeviceAddress = blas->vk.deviceAddress;
        tlasInstances.push_back(inst);
      }
    }
  }

  // ---------- TLAS build ----------
  // Walks the BLAS instances accumulated above and emits one TLAS build into
  // the primary cmd buffer. The TLAS isn't bound to any shader yet (phase 4);
  // building it here exercises the path so RenderDoc / validation can verify
  // correctness.
  if (rtEnabled && !tlasInstances.empty()) {
    const uint32_t instanceCount = (uint32_t)tlasInstances.size();

    auto destroyBuffer = [](RIBuffer_s *b) {
      if (b->vk.buffer) {
        auto *cntx = RI.GetActiveSet();
        cntx->freelist.push_back(RIFree(b->vk.buffer));
        cntx->freelist.push_back(RIFree(b->vk.allocation));
      }
      delete b;
    };

    auto createInstanceBuffer = [&](uint32_t capacity) {
      // Host-coherent mapped: the instance array is rebuilt every frame, so
      // persistent mapping + sequential writes is the fastest path.
      uint32_t qf[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN, qf,
                                      RI_QUEUE_LEN);
      bci.size = (VkDeviceSize)capacity * sizeof(RIAccelInstance_s);
      bci.usage =
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO;
      aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
      VmaAllocationInfo info = {};
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_tlasInstanceBuffer.vk.buffer,
                                    &m_tlasInstanceBuffer.vk.allocation,
                                    &info));
      m_tlasInstanceBuffer.mappedAddress = info.pMappedData;
    };

    auto createDeviceBuffer =
        [&](VkDeviceSize size,
            VkBufferUsageFlags usage) -> std::shared_ptr<RIBuffer_s> {
      std::shared_ptr<RIBuffer_s> buf(new RIBuffer_s(), destroyBuffer);
      uint32_t qf[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN, qf,
                                      RI_QUEUE_LEN);
      bci.size = size;
      bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &buf->vk.buffer, &buf->vk.allocation,
                                    nullptr));
      return buf;
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
      createInstanceBuffer(newCap);
      m_tlasCapacity = newCap;
    }

    // Stream the instance array into the host-mapped buffer.
    std::memcpy(m_tlasInstanceBuffer.mappedAddress, tlasInstances.data(),
                instanceCount * sizeof(RIAccelInstance_s));

    // Make BLAS writes visible to the TLAS build. The plan calls this an
    // accel-build→accel-build barrier; one global memory barrier covers all
    // BLASes that GetOrBuildBlas just emitted.
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

    const bool needsTlasRealloc =
        (m_tlas.vk.handle == VK_NULL_HANDLE) ||
        (!m_tlasStorage || m_tlas.flags == 0) ||
        (m_tlas.buildScratchSize < tlasBuildScratch) ||
        (m_tlasStorage && tlasStorageSize > 0 /*always realloc on grow*/);
    // Heuristic above keeps Phase-3 simple: rebuild storage whenever the size
    // query changed. A future phase should cache storage and reuse when sizes
    // are equal.
    if (needsTlasRealloc) {
      if (m_tlas.vk.handle != VK_NULL_HANDLE) {
        cntx->freelist.push_back(RIFree(m_tlas.vk.handle));
        m_tlas = {};
      }
      m_tlasStorage = createDeviceBuffer(
          tlasStorageSize,
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
      tlasDesc.storage = m_tlasStorage.get();
      tlasDesc.storageOffset = 0;
      tlasDesc.storageSize = tlasStorageSize;
      if (InitRIAccelStructure(&RI.device, &tlasDesc, &m_tlas) != RI_SUCCESS) {
        // Leave m_tlas zeroed; skip the build this frame.
        m_tlas = {};
      }
    }

    if (m_tlas.vk.handle != VK_NULL_HANDLE) {
      std::shared_ptr<RIBuffer_s> tlasScratch = createDeviceBuffer(
          tlasBuildScratch, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

      RIBuildTlasDesc_s build = {};
      build.dst = &m_tlas;
      build.src = nullptr;
      build.mode = RI_ACCEL_BUILD_MODE_BUILD;
      build.instanceNum = instanceCount;
      build.instanceBuffer = &m_tlasInstanceBuffer;
      build.instanceOffset = 0;
      build.scratchBuffer = tlasScratch.get();
      build.scratchOffset = 0;
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
    toColor[1].image = RI.visiblityTexture[RI.swapchainIndex].vk.image;
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
  colorAttachments[1].imageView = RI.visiblityView[RI.swapchainIndex].vk.image;
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
    m_surfelPrepare.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                        kSurfelPrepareHash,
                                        "hybrid.surfel_prepare",
                                        &computeCreate);
    m_surfelPrepare.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], CELL_COUNT / 32u, 1, 1);
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
    toRead[1].image = RI.visiblityTexture[RI.swapchainIndex].vk.image;

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[2].srcStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
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

  // Surfel generation — reads gbuffer (set=2: primObjIDMap/normalMap/depthMap)
  // + perFrame (set=1) + bindless SSBOs (set=0), writes m_surfelResultTexture
  // (set=2 binding=3). Runs once per frame at half-resolution.
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kSurfelGenerateHash =
        hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGenerate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                         kSurfelGenerateHash,
                                         "hybrid.surfel_generate",
                                         &computeCreate);
    m_surfelGenerate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

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
      computeBindings.push_back(b);
    };
    const VkSampler sampler = m_materialSampler->vk.image.sampler;
    pushImage("primObjIDMap",
              RI.visiblityView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler);
    pushImage("normalMap",
              RI.normalView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler);
    pushImage("depthMap",
              RI.depthView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler);
    pushImage("resultImage",
              m_surfelResultView[RI.swapchainIndex].vk.image,
              VK_IMAGE_LAYOUT_GENERAL,
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE);

    m_surfelGenerate.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, computeBindings.data(),
                                     computeBindings.size(),
                                     VK_PIPELINE_BIND_POINT_COMPUTE);

    const uint32_t halfW = RI.swapchain.width / 2u;
    const uint32_t halfH = RI.swapchain.height / 2u;
    CmdDispatch(&RI.primary.cmds[0], (halfW + 15u) / 16u,
                (halfH + 15u) / 16u, 1u);
  }
  // Exit contract: no active rendering instance. Scene::Render opens its own
  // rendering for the GUI passes that follow this call.
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
  }
  m_bindlessSet.destroy(&RI.device);
}

} // namespace hpl
