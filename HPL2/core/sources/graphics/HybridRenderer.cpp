#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
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

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

namespace hpl {

namespace detail {
// uint32_t resolveTextureFilterGroup(cMaterial::TextureAntistropy anisotropy,
//                                    eTextureWrap wrap, eTextureFilter filter)
//                                    {
//   const uint32_t anisotropyGroup =
//       (static_cast<uint32_t>(eTextureFilter_LastEnum) *
//        static_cast<uint32_t>(eTextureWrap_LastEnum)) *
//       static_cast<uint32_t>(anisotropy);
//   return anisotropyGroup + ((static_cast<uint32_t>(wrap) *
//                              static_cast<uint32_t>(eTextureFilter_LastEnum))
//                              +
//                             static_cast<uint32_t>(filter));
// }

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

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources, 0),
      m_diffuseBindless(OBJECT_SLOT_CAPACITY, RI_NUMBER_FRAMES_FLIGHT) {
  {
    auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "forward_diffuse.vert.spv");
    auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                 "forward_diffuse.frag.spv");
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage}};
    m_forwardDiffuse.initialize(&RI.device, stages);

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
  }
}

void cHybridRenderer::Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  const ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
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

  for (iRenderable *pObject : solids) {
    cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
    iVertexBuffer *pVB = pObject->GetVertexBuffer();
    cMaterial *pMat = pObject->GetMaterial();
    if (!pVB || !pMat)
      continue;

    cMatrixf *pInv = pObject->GetInvModelMatrix();

    ObjectGPUData payload{};
    payload.dissolveAmount = pObject->GetCoverageAmount();
    payload.materialID = 0;
    payload.lightLevel = 1.0f;
    payload.illuminationAmount = pObject->GetIlluminationAmount();
    const ml::float4x4 modelF4 = cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    if (pInv) {
      const ml::float4x4 invF4 = cMath::ToFloatTranspose4x4(*pInv);
      std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    }
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
          eVertexBufferElement_Position,        eVertexBufferElement_Texture1Tangent,
          eVertexBufferElement_Normal,          eVertexBufferElement_Texture0,
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
  }

  // ---------- First pass: forward MRT (color + visibility) ----------
  VkCommandBuffer cmd = RI.primary.cmds[0].vk.cmd;
  RIProgram::DescriptorBinding bindings[16] = {};
  size_t numBindings = 0;

  RIDescriptor_s sceneObjectsDesc = {};
  sceneObjectsDesc.cookie =
      hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)&m_diffuseObjectBuffer);
  sceneObjectsDesc.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sceneObjectsDesc.vk.buffer.buffer = m_diffuseObjectBuffer.vk.buffer;
  sceneObjectsDesc.vk.buffer.offset = 0;
  sceneObjectsDesc.vk.buffer.range = OBJECT_SLOT_CAPACITY * sizeof(ObjectGPUData);
  bindings[numBindings].descriptor = sceneObjectsDesc;
  bindings[numBindings++].handle =
      DescriptorBindingID::Create("sceneObjectsBuf");

  RI.UpdateFrameUBO(&bindings[numBindings].descriptor, &perFrame,
                    sizeof(perFrame));
  bindings[numBindings++].handle = DescriptorBindingID::Create("perFrame");

  {
    const struct {
      RIBuffer_s *buffer;
      const char *name;
    } handleBindings[6] = {
        {&m_opaquePositionHandles, "opaquePositionHandles"},
        {&m_opaqueTangentHandles, "opaqueTangentHandles"},
        {&m_opaqueNormalHandles, "opaqueNormalHandles"},
        {&m_opaqueUv0Handles, "opaqueUv0Handles"},
        {&m_opaqueColorHandles, "opaqueColorHandles"},
        {&m_opaqueIndexHandles, "opaqueIndexHandles"},
    };
    for (const auto &hb : handleBindings) {
      RIDescriptor_s d = {};
      d.cookie = hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)hb.buffer);
      d.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      d.vk.buffer.buffer = hb.buffer->vk.buffer;
      d.vk.buffer.offset = 0;
      d.vk.buffer.range = OBJECT_SLOT_CAPACITY * sizeof(VkDeviceAddress);
      bindings[numBindings].descriptor = d;
      bindings[numBindings++].handle = DescriptorBindingID::Create(hb.name);
    }
  }

  // Close Scene's outer rendering so we can attach our second color target.
  vkCmdEndRendering(cmd);

  // Visibility target: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL.
  {
    VkImageMemoryBarrier2 toColor = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toColor.srcAccessMask = 0;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.image = RI.visiblityTexture[RI.swapchainIndex].vk.image;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toColor;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // Begin our 2-color rendering: [swapchain color, R32_UINT visibility] +
  // depth.
  VkRenderingAttachmentInfo colorAttachments[2] = {
      {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
      {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
  RI_VK_FillColorAttachmentView(&colorAttachments[0],
                                &RI.swapchainView[RI.swapchainIndex],
                                /*attachAndClear=*/false);
  colorAttachments[1].imageView = RI.visiblityView[RI.swapchainIndex].vk.image;
  colorAttachments[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachments[1].clearValue.color.uint32[0] = 0;

  VkRenderingAttachmentInfo depthAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  RI_VK_FillDepthAttachment(&depthAttachment, &RI.depthView[RI.swapchainIndex],
                            /*attachAndClear=*/false);

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

  // Pipeline create info — stable for this pass; bindPipeline caches by hash.
  // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs, so the
  // pipeline declares zero vertex input bindings and zero attributes.
  VkPipelineVertexInputStateCreateInfo vertexInputState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInputState.vertexBindingDescriptionCount = 0;
  vertexInputState.pVertexBindingDescriptions = nullptr;
  vertexInputState.vertexAttributeDescriptionCount = 0;
  vertexInputState.pVertexAttributeDescriptions = nullptr;

  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineRasterizationStateCreateInfo rasterizationState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizationState.lineWidth = 1.0f;

  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  VkFormat colorFormats[2] = {RIFormatToVK(RI.swapchain.format),
                              VK_FORMAT_R32_UINT};
  VkPipelineRenderingCreateInfo pipelineRendering = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  pipelineRendering.colorAttachmentCount = 2;
  pipelineRendering.pColorAttachmentFormats = colorFormats;
  pipelineRendering.depthAttachmentFormat =
      RIFormatToVK(RIBootstrap::DepthFormat);

  VkPipelineViewportStateCreateInfo viewportState = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineMultisampleStateCreateInfo multisampleState = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencilState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencilState.depthTestEnable = VK_TRUE;
  depthStencilState.depthWriteEnable = VK_TRUE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencilState.minDepthBounds = 0.0f;
  depthStencilState.maxDepthBounds = 1.0f;

  VkPipelineColorBlendAttachmentState blendAttachments[2] = {
      {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
       VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
       VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT},
      {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
       VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
       VK_COLOR_COMPONENT_R_BIT},
  };
  VkPipelineColorBlendStateCreateInfo colorBlendState = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlendState.attachmentCount = 2;
  colorBlendState.pAttachments = blendAttachments;

  VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipelineCreateInfo.pNext = &pipelineRendering;
  pipelineCreateInfo.pVertexInputState = &vertexInputState;
  pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
  pipelineCreateInfo.pRasterizationState = &rasterizationState;
  pipelineCreateInfo.pDynamicState = &dynamicState;
  pipelineCreateInfo.pViewportState = &viewportState;
  pipelineCreateInfo.pMultisampleState = &multisampleState;
  pipelineCreateInfo.pDepthStencilState = &depthStencilState;
  pipelineCreateInfo.pColorBlendState = &colorBlendState;

  hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, RI.swapchain.format);
  pipelineHash = hash_u32(pipelineHash, RIBootstrap::DepthFormat);
  m_forwardDiffuse.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineHash,
                                "hybrid.forward_diffuse_mrt",
                                &pipelineCreateInfo);
  m_forwardDiffuse.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                   RI.frameIndex, bindings, numBindings);

  // Single indirect dispatch — VS pulls vertex/index streams from set 0 SSBOs
  // keyed by gl_InstanceIndex (slot id) and gl_VertexIndex (index in stream).
  if (writtenDraws > 0) {
    vkCmdDrawIndirect(cmd, m_indirectDrawBuffer.vk.buffer,
                      (VkDeviceSize)indirectReq.elementOffset *
                          sizeof(VkDrawIndirectCommand),
                      writtenDraws, (uint32_t)sizeof(VkDrawIndirectCommand));
  }

  vkCmdEndRendering(cmd);

  // Visibility target: COLOR_ATTACHMENT -> SHADER_READ_ONLY (consumer ready).
  {
    VkImageMemoryBarrier2 toRead = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = RI.visiblityTexture[RI.swapchainIndex].vk.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toRead;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // Re-begin Scene's *original* rendering — single color (swapchain) + depth —
  // so cScene::Render's GUI/3D-Gui calls see the rendering they expected.
  VkRenderingAttachmentInfo restoreColor = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  RI_VK_FillColorAttachmentView(&restoreColor,
                                &RI.swapchainView[RI.swapchainIndex],
                                /*attachAndClear=*/false);
  VkRenderingAttachmentInfo restoreDepth = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  RI_VK_FillDepthAttachment(&restoreDepth, &RI.depthView[RI.swapchainIndex],
                            /*attachAndClear=*/false);
  VkRenderingInfo restoreInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  restoreInfo.renderArea = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
  restoreInfo.layerCount = 1;
  restoreInfo.colorAttachmentCount = 1;
  restoreInfo.pColorAttachments = &restoreColor;
  restoreInfo.pDepthAttachment = &restoreDepth;
  vkCmdBeginRendering(cmd, &restoreInfo);
}

cHybridRenderer::~cHybridRenderer() {}

} // namespace hpl
