#include "graphics/RIProgram.h"
#include "system/Platform.h"

#include <cassert>
#include <system/Types.h>
#include <system/stb_ds.h>

#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIRenderer.h"
#include "graphics/spirv_reflect.h"

#if (DEVICE_IMPL_VULKAN)
#include "graphics/RIVK.h"
#endif
#if (DEVICE_IMPL_MTL)
#include "graphics/RIMTL.h"
#endif

namespace hpl {

#if (DEVICE_IMPL_MTL)
// Compile a Metal Shading Language source blob at runtime (no offline metallib
// toolchain dependency) and resolve its entry-point function. Caller releases
// the returned function. `msl` holds NUL-free MSL text emitted by slangc
// (-target metal); `entry` is the Slang entry-point name (e.g. "vsMain").
static MTL::Function *mtlLoadFunction(MTL::Device *device,
                                      const std::vector<char> &msl,
                                      const std::string &entry) {
  if (msl.empty())
    return nullptr;
  NS::String *src = NS::String::string(
      std::string(msl.data(), msl.size()).c_str(), NS::UTF8StringEncoding);
  NS::Error *err = nullptr;
  MTL::Library *lib = device->newLibrary(src, nullptr, &err);
  if (!lib) {
    if (err)
      Error("Metal newLibrary failed: %s\n",
                 err->localizedDescription()->utf8String());
    return nullptr;
  }
  NS::String *fnName = NS::String::string(entry.c_str(), NS::UTF8StringEncoding);
  MTL::Function *fn = lib->newFunction(fnName);
  lib->release();
  return fn;
}
#endif
#if (DEVICE_IMPL_VULKAN)
static void vkDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc ) {
	assert( device->renderer->api == RI_DEVICE_API_VK );
	struct RIProgram::DescriptorSetSlot *programDescriptor = hpl_container_of( alloc, &RIProgram::DescriptorSetSlot::alloc );
  VkDescriptorPoolSize descriptorPoolSize[16] = {};
  size_t descriptorPoolLen = 0;
  if( programDescriptor->samplerMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_SAMPLER, (uint32_t)programDescriptor->samplerMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->combinedImageSamplerMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)programDescriptor->combinedImageSamplerMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->constantBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)programDescriptor->constantBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->dynamicConstantBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (uint32_t)programDescriptor->dynamicConstantBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->textureMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (uint32_t)programDescriptor->textureMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->storageTextureMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)programDescriptor->storageTextureMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->bufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, (uint32_t)programDescriptor->bufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->storageBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, (uint32_t)programDescriptor->storageBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->structuredBufferMaxNum > 0 || programDescriptor->storageStructuredBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{
  		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)programDescriptor->structuredBufferMaxNum * DESCRIPTOR_MAX_SIZE + (uint32_t)programDescriptor->storageStructuredBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->accelerationStructureMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, (uint32_t)programDescriptor->accelerationStructureMaxNum * DESCRIPTOR_MAX_SIZE };
  assert( descriptorPoolLen < ARRAY_COUNT( descriptorPoolSize ) );
  const VkDescriptorPoolCreateInfo info = {
  	VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, DESCRIPTOR_MAX_SIZE, (uint32_t)descriptorPoolLen, descriptorPoolSize };
  struct RIDescriptorPoolAllocSlot poolSlot = {};
  VK_WrapResult( vkCreateDescriptorPool( device->vk.device, &info, NULL, &poolSlot.vk.handle ) );
  arrpush( alloc->pools, poolSlot );
  for( size_t i = 0; i < DESCRIPTOR_MAX_SIZE; i++ ) {
  	struct RIDescriptorSetSlot* slot = allocDescriptorSetSlot( alloc );
  	VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  	info.pNext = NULL;
  	info.descriptorPool = poolSlot.vk.handle;
  	info.descriptorSetCount = 1;
  	assert( programDescriptor->vk.setLayout != VK_NULL_HANDLE );
  	info.pSetLayouts = &programDescriptor->vk.setLayout;
  	VK_WrapResult( vkAllocateDescriptorSets( device->vk.device, &info, &slot->vk.handle ) );
  	arrpush( alloc->reservedSlots, slot );
  }
}
#endif // DEVICE_IMPL_VULKAN

void RIProgram::bindPipeline(struct RIDevice *device, struct RICmd *cmd,
                             hash_t pipelineHash, const char *debugName,
                             const RIGraphicsPipelineDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    // Build a VkGraphicsPipelineCreateInfo from the neutral descriptor, then
    // hand off to the existing Vk-typed overload (which fills in shader stages
    // + layout from this program and caches by pipelineHash). All sub-state
    // structs are locals that stay alive across that call.
    VkVertexInputBindingDescription
        vkStreams[RIGraphicsPipelineDesc::MAX_VERTEX_STREAMS] = {};
    for (uint32_t i = 0; i < desc.vertexStreamCount; i++) {
      vkStreams[i].binding = desc.vertexStreams[i].binding;
      vkStreams[i].stride = desc.vertexStreams[i].stride;
      vkStreams[i].inputRate = desc.vertexStreams[i].perInstance
                                   ? VK_VERTEX_INPUT_RATE_INSTANCE
                                   : VK_VERTEX_INPUT_RATE_VERTEX;
    }
    VkVertexInputAttributeDescription
        vkAttrs[RIGraphicsPipelineDesc::MAX_VERTEX_ATTRIBUTES] = {};
    for (uint32_t i = 0; i < desc.vertexAttributeCount; i++) {
      vkAttrs[i].location = desc.vertexAttributes[i].location;
      vkAttrs[i].binding = desc.vertexAttributes[i].binding;
      vkAttrs[i].format = RIFormatToVK(desc.vertexAttributes[i].format);
      vkAttrs[i].offset = desc.vertexAttributes[i].offset;
    }
    VkPipelineVertexInputStateCreateInfo vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = desc.vertexStreamCount;
    vertexInput.pVertexBindingDescriptions =
        desc.vertexStreamCount ? vkStreams : NULL;
    vertexInput.vertexAttributeDescriptionCount = desc.vertexAttributeCount;
    vertexInput.pVertexAttributeDescriptions =
        desc.vertexAttributeCount ? vkAttrs : NULL;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology =
        ri_vk_RITopologyToVK((enum RITopology_e)desc.topology);

    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = desc.fillMode == RI_FILL_LINE ? VK_POLYGON_MODE_LINE
                                                       : VK_POLYGON_MODE_FILL;
    raster.cullMode = ri_vk_RICullModeToVK((enum RICullMode_e)desc.cullMode);
    raster.frontFace = desc.frontCounterClockwise
                           ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                           : VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineViewportStateCreateInfo viewportState = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples =
        (VkSampleCountFlagBits)(desc.sampleCount ? desc.sampleCount : 1);

    VkPipelineDepthStencilStateCreateInfo depthStencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp =
        ri_vk_RICompareOpToVK((enum RICompareFunc_e)desc.depthCompareOp);
    depthStencil.maxDepthBounds = 1.0f;
    if (desc.stencilTestEnable) {
      depthStencil.stencilTestEnable = VK_TRUE;
      const RIStencilOpDesc *faces[2] = {&desc.stencilFront, &desc.stencilBack};
      VkStencilOpState *out[2] = {&depthStencil.front, &depthStencil.back};
      for (int f = 0; f < 2; f++) {
        out[f]->failOp =
            ri_vk_RIStencilOpToVK((enum RIStencilOp_e)faces[f]->failOp);
        out[f]->passOp =
            ri_vk_RIStencilOpToVK((enum RIStencilOp_e)faces[f]->passOp);
        out[f]->depthFailOp =
            ri_vk_RIStencilOpToVK((enum RIStencilOp_e)faces[f]->depthFailOp);
        out[f]->compareOp =
            ri_vk_RICompareOpToVK((enum RICompareFunc_e)faces[f]->compareOp);
        out[f]->compareMask = faces[f]->compareMask;
        out[f]->writeMask = faces[f]->writeMask;
        out[f]->reference = desc.stencilReference;
      }
    }

    VkPipelineColorBlendAttachmentState
        blendAttach[RIGraphicsPipelineDesc::MAX_COLOR_ATTACHMENTS] = {};
    VkFormat colorFormats[RIGraphicsPipelineDesc::MAX_COLOR_ATTACHMENTS] = {};
    for (uint32_t i = 0; i < desc.colorCount; i++) {
      const RIColorAttachmentDesc &c = desc.colors[i];
      blendAttach[i].blendEnable = c.blendEnabled ? VK_TRUE : VK_FALSE;
      blendAttach[i].srcColorBlendFactor =
          ri_vk_RIBlendFactorToVK((enum RIBlendFactor_e)c.srcColor);
      blendAttach[i].dstColorBlendFactor =
          ri_vk_RIBlendFactorToVK((enum RIBlendFactor_e)c.dstColor);
      blendAttach[i].colorBlendOp =
          ri_vk_RIBlendOpToVK((enum RIBlendOp_e)c.colorBlendOp);
      blendAttach[i].srcAlphaBlendFactor =
          ri_vk_RIBlendFactorToVK((enum RIBlendFactor_e)c.srcAlpha);
      blendAttach[i].dstAlphaBlendFactor =
          ri_vk_RIBlendFactorToVK((enum RIBlendFactor_e)c.dstAlpha);
      blendAttach[i].alphaBlendOp =
          ri_vk_RIBlendOpToVK((enum RIBlendOp_e)c.alphaBlendOp);
      blendAttach[i].colorWriteMask = (VkColorComponentFlags)c.writeMask;
      colorFormats[i] = RIFormatToVK(c.format);
    }
    VkPipelineColorBlendStateCreateInfo colorBlend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = desc.colorCount;
    colorBlend.pAttachments = desc.colorCount ? blendAttach : NULL;

    VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = desc.colorCount;
    renderingInfo.pColorAttachmentFormats = desc.colorCount ? colorFormats : NULL;
    if (desc.depthStencilFormat != RI_FORMAT_UNKNOWN)
      renderingInfo.depthAttachmentFormat =
          RIFormatToVK(desc.depthStencilFormat);
    if (desc.stencilTestEnable && desc.depthStencilFormat != RI_FORMAT_UNKNOWN)
      renderingInfo.stencilAttachmentFormat =
          RIFormatToVK(desc.depthStencilFormat);

    VkGraphicsPipelineCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    createInfo.pNext = &renderingInfo;
    createInfo.pVertexInputState = &vertexInput;
    createInfo.pInputAssemblyState = &inputAssembly;
    createInfo.pViewportState = &viewportState;
    createInfo.pRasterizationState = &raster;
    createInfo.pMultisampleState = &multisample;
    createInfo.pDepthStencilState = &depthStencil;
    createInfo.pColorBlendState = &colorBlend;
    createInfo.pDynamicState = &dynamicState;

    bindPipeline(device, cmd, pipelineHash, debugName, &createInfo);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    auto it = pipeline.find(pipelineHash);
    RIPipeline slot = {};
    if (it == pipeline.end()) {
      MTL::Device *dev = device->mtl.device;
      MTL::RenderPipelineDescriptor *rp =
          MTL::RenderPipelineDescriptor::alloc()->init();
      MTL::Function *vfn =
          mtlLoadFunction(dev, shaderBin[PROGRAM_STAGE_VERTEX].buf,
                          shaderBin[PROGRAM_STAGE_VERTEX].entryPoint);
      MTL::Function *ffn =
          mtlLoadFunction(dev, shaderBin[PROGRAM_STAGE_FRAGMENT].buf,
                          shaderBin[PROGRAM_STAGE_FRAGMENT].entryPoint);
      rp->setVertexFunction(vfn);
      rp->setFragmentFunction(ffn);
      if (rp->label())
        rp->setLabel(NS::String::string(debugName, NS::UTF8StringEncoding));
      for (uint32_t i = 0; i < desc.colorCount; i++) {
        const RIColorAttachmentDesc &c = desc.colors[i];
        MTL::RenderPipelineColorAttachmentDescriptor *ca =
            rp->colorAttachments()->object(i);
        ca->setPixelFormat(RIToMTLFormat(c.format));
        ca->setBlendingEnabled(c.blendEnabled);
        ca->setSourceRGBBlendFactor(RIToMTLBlendFactor(c.srcColor));
        ca->setDestinationRGBBlendFactor(RIToMTLBlendFactor(c.dstColor));
        ca->setRgbBlendOperation(RIToMTLBlendOp(c.colorBlendOp));
        ca->setSourceAlphaBlendFactor(RIToMTLBlendFactor(c.srcAlpha));
        ca->setDestinationAlphaBlendFactor(RIToMTLBlendFactor(c.dstAlpha));
        ca->setAlphaBlendOperation(RIToMTLBlendOp(c.alphaBlendOp));
        ca->setWriteMask(RIToMTLColorWriteMask(c.writeMask));
      }
      if (desc.depthStencilFormat != RI_FORMAT_UNKNOWN)
        rp->setDepthAttachmentPixelFormat(
            RIToMTLFormat(desc.depthStencilFormat));
      if (desc.stencilTestEnable && desc.depthStencilFormat != RI_FORMAT_UNKNOWN)
        rp->setStencilAttachmentPixelFormat(
            RIToMTLFormat(desc.depthStencilFormat));
      if (desc.vertexAttributeCount > 0) {
        MTL::VertexDescriptor *vd = MTL::VertexDescriptor::vertexDescriptor();
        for (uint32_t i = 0; i < desc.vertexAttributeCount; i++) {
          const RIVertexAttributeDesc &a = desc.vertexAttributes[i];
          MTL::VertexAttributeDescriptor *va =
              vd->attributes()->object(a.location);
          va->setFormat(RIToMTLVertexFormat(a.format));
          va->setOffset(a.offset);
          // Top-of-table mapping (see RI_MTL_VertexBufferIndex) so stream buffers
          // don't collide with descriptor-set argument buffers / push constants.
          va->setBufferIndex(RI_MTL_VertexBufferIndex(a.binding));
        }
        for (uint32_t i = 0; i < desc.vertexStreamCount; i++) {
          const RIVertexStreamDesc &s = desc.vertexStreams[i];
          MTL::VertexBufferLayoutDescriptor *l =
              vd->layouts()->object(RI_MTL_VertexBufferIndex(s.binding));
          l->setStride(s.stride);
          l->setStepFunction(s.perInstance ? MTL::VertexStepFunctionPerInstance
                                           : MTL::VertexStepFunctionPerVertex);
        }
        rp->setVertexDescriptor(vd);
      }
      rp->setRasterSampleCount(desc.sampleCount ? desc.sampleCount : 1);
      NS::Error *err = nullptr;
      slot.mtl.render = dev->newRenderPipelineState(rp, &err);
      slot.mtl.primitiveType = RIToMTLPrimitiveType(desc.topology);
      if (!slot.mtl.render && err)
        Error("Metal render pipeline '%s' failed: %s\n", debugName,
                   err->localizedDescription()->utf8String());
      if (vfn) vfn->release();
      if (ffn) ffn->release();
      rp->release();
      // Metal keeps depth/stencil test in a separate state object (not the
      // render pipeline). Build it always: a default depth-disabled state
      // matches Metal's default and stops a pipeline from inheriting a previous
      // bind's depth/stencil state on the shared encoder.
      {
        MTL::DepthStencilDescriptor *dsd =
            MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(
            desc.depthTestEnable ? RIToMTLCompareFunc(desc.depthCompareOp)
                                 : MTL::CompareFunctionAlways);
        dsd->setDepthWriteEnabled(desc.depthTestEnable && desc.depthWriteEnable);
        if (desc.stencilTestEnable) {
          const RIStencilOpDesc *faces[2] = {&desc.stencilFront,
                                             &desc.stencilBack};
          MTL::StencilDescriptor *sd[2] = {nullptr, nullptr};
          for (int f = 0; f < 2; f++) {
            sd[f] = MTL::StencilDescriptor::alloc()->init();
            sd[f]->setStencilFailureOperation(RIToMTLStencilOp(faces[f]->failOp));
            sd[f]->setDepthStencilPassOperation(
                RIToMTLStencilOp(faces[f]->passOp));
            sd[f]->setDepthFailureOperation(
                RIToMTLStencilOp(faces[f]->depthFailOp));
            sd[f]->setStencilCompareFunction(
                RIToMTLCompareFunc(faces[f]->compareOp));
            sd[f]->setReadMask(faces[f]->compareMask);
            sd[f]->setWriteMask(faces[f]->writeMask);
          }
          dsd->setFrontFaceStencil(sd[0]);
          dsd->setBackFaceStencil(sd[1]);
          sd[0]->release();
          sd[1]->release();
        }
        slot.mtl.depthStencil = dev->newDepthStencilState(dsd);
        slot.mtl.stencilReference = desc.stencilReference;
        dsd->release();
      }
      pipeline[pipelineHash] = slot;
    } else {
      slot = it->second;
    }
    cmd->mtl.primitiveType = slot.mtl.primitiveType;
    assert(cmd->mtl.render && "bindPipeline requires an open render encoder");
    cmd->mtl.render->setRenderPipelineState(slot.mtl.render);
    if (slot.mtl.depthStencil) {
      cmd->mtl.render->setDepthStencilState(slot.mtl.depthStencil);
      cmd->mtl.render->setStencilReferenceValue(slot.mtl.stencilReference);
    }
    // Fill mode, cull mode and front-facing winding are Metal encoder state (not
    // baked into the pipeline), so apply them here alongside the primitive type.
    // Without setCullMode the encoder defaults to CullModeNone (no culling) and
    // without setFrontFacingWinding it defaults to Clockwise, so both must be set
    // explicitly to match the Vulkan rasterizer state above.
    cmd->mtl.render->setTriangleFillMode(desc.fillMode == RI_FILL_LINE
                                             ? MTL::TriangleFillModeLines
                                             : MTL::TriangleFillModeFill);
    MTL::CullMode mtlCull = MTL::CullModeNone;
    if (desc.cullMode == RI_CULL_MODE_FRONT)
      mtlCull = MTL::CullModeFront;
    else if (desc.cullMode == RI_CULL_MODE_BACK)
      mtlCull = MTL::CullModeBack;
    cmd->mtl.render->setCullMode(mtlCull);
    // NOTE: when the Metal render path adopts a viewport Y-flip convention
    // (see setViewport), this winding may need to be inverted to compensate for
    // the flipped clip-space handedness; it is faithful to desc as-is today.
    cmd->mtl.render->setFrontFacingWinding(
        desc.frontCounterClockwise ? MTL::WindingCounterClockwise
                                   : MTL::WindingClockwise);
    return;
  }
#endif
}

void RIProgram::pushConstants(struct RICmd *cmd, const void *data,
                              uint32_t size, uint32_t offset) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    vkCmdPushConstants(cmd->vk.cmd, impl.vk.pipelineLayout,
                       impl.vk.pushConstant.shaderStageFlags, offset, size,
                       data);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    // slangc emits the push-constant block as a loose constant* [[buffer(N)]];
    // setBytes inlines the data at that per-stage index (from mtlBindings).
    // (offset is unused — Metal setBytes replaces the whole inline block.)
    (void)offset;
    if (cmd->mtl.compute) {
      if (mtlPushConstantIndex[PROGRAM_STAGE_COMPUTE] >= 0)
        cmd->mtl.compute->setBytes(data, size,
                                   mtlPushConstantIndex[PROGRAM_STAGE_COMPUTE]);
    } else if (cmd->mtl.render) {
      if (mtlPushConstantIndex[PROGRAM_STAGE_VERTEX] >= 0)
        cmd->mtl.render->setVertexBytes(
            data, size, mtlPushConstantIndex[PROGRAM_STAGE_VERTEX]);
      if (mtlPushConstantIndex[PROGRAM_STAGE_FRAGMENT] >= 0)
        cmd->mtl.render->setFragmentBytes(
            data, size, mtlPushConstantIndex[PROGRAM_STAGE_FRAGMENT]);
    }
    return;
  }
#endif
}

void RIProgram::bindComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                                    hash_t pipelineHash, const char *debugName,
                                    const RIComputePipelineDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    (void)desc; // [numthreads] baked into SPIR-V
    VkComputePipelineCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    bindComputePipeline(device, cmd, pipelineHash, debugName, &createInfo);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    auto it = pipeline.find(pipelineHash);
    RIPipeline slot = {};
    if (it == pipeline.end()) {
      MTL::Device *dev = device->mtl.device;
      MTL::Function *cfn =
          mtlLoadFunction(dev, shaderBin[PROGRAM_STAGE_COMPUTE].buf,
                          shaderBin[PROGRAM_STAGE_COMPUTE].entryPoint);
      NS::Error *err = nullptr;
      slot.mtl.compute = dev->newComputePipelineState(cfn, &err);
      if (!slot.mtl.compute && err)
        Error("Metal compute pipeline '%s' failed: %s\n", debugName,
                   err->localizedDescription()->utf8String());
      if (cfn) cfn->release();
      pipeline[pipelineHash] = slot;
    } else {
      slot = it->second;
    }
    // Ensure a compute encoder is active (caller closed any other encoder via
    // mtl_encoderEnd first).
    cmd->mtl_encoderCompute();
    cmd->mtl.compute->setComputePipelineState(slot.mtl.compute);
    // Stash the shader's [numthreads] for dispatchThreadgroups (Metal needs it
    // host-side; it is not carried in the MSL kernel).
    cmd->mtl.threadsPerThreadgroup[0] = desc.numThreads[0];
    cmd->mtl.threadsPerThreadgroup[1] = desc.numThreads[1];
    cmd->mtl.threadsPerThreadgroup[2] = desc.numThreads[2];
    return;
  }
#endif
}

// Raw Vk-typed pipeline overloads — Vulkan-only (the header declares them under
// the same guard). Metal consumers use the neutral RIGraphicsPipelineDesc/
// RIComputePipelineDesc overloads above.
#if (DEVICE_IMPL_VULKAN)
void RIProgram::bindPipeline(struct RIDevice *device, struct RICmd* cmd, hash_t pipelineHash, const char* debugName, VkGraphicsPipelineCreateInfo* pipelineCreateInfo) {
  assert(shaderBin[PROGRAM_STAGE_COMPUTE].buf.empty() && "compute-only programs must use bindComputePipeline");
  VkPipeline pipelineHandle = VK_NULL_HANDLE;
  auto it = pipeline.find(pipelineHash);
  if(it == pipeline.end()) {
    uint32_t numModules = 0;
    VkShaderModule modules[4] = {0};
    VkPipelineShaderStageCreateInfo stageCreateInfo[4] = {};
    if (shaderBin[PROGRAM_STAGE_VERTEX].buf.size() > 0 &&
        shaderBin[PROGRAM_STAGE_FRAGMENT].buf.size() > 0) {
      pipelineCreateInfo->stageCount = 2;
      const VkShaderModuleCreateInfo vertModuleCreateInfo = {
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          NULL,
          (VkShaderModuleCreateFlags)0,
          (size_t)shaderBin[PROGRAM_STAGE_VERTEX].buf.size(),
          (const uint32_t *)shaderBin[PROGRAM_STAGE_VERTEX].buf.data(),
      };
      vkCreateShaderModule(device->vk.device, &vertModuleCreateInfo, NULL,
                          &modules[numModules]);
      stageCreateInfo[0] = VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stageCreateInfo[0].stage = VK_SHADER_STAGE_VERTEX_BIT,
      stageCreateInfo[0].module = modules[numModules],
      stageCreateInfo[0].pName = shaderBin[PROGRAM_STAGE_VERTEX].entryPoint.c_str();
      numModules++;

      const VkShaderModuleCreateInfo fragModuleCreateInfo = {
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          NULL,
          (VkShaderModuleCreateFlags)0,
          (size_t)shaderBin[PROGRAM_STAGE_FRAGMENT].buf.size(),
          (const uint32_t *)shaderBin[PROGRAM_STAGE_FRAGMENT].buf.data(),
      };
      vkCreateShaderModule(device->vk.device, &fragModuleCreateInfo, NULL,
                          &modules[numModules]);
      stageCreateInfo[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stageCreateInfo[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stageCreateInfo[1].module = modules[numModules];
      stageCreateInfo[1].pName = shaderBin[PROGRAM_STAGE_FRAGMENT].entryPoint.c_str();
      numModules++;
    } else {
      assert(false && "failed to resolve bin");
    }
    pipelineCreateInfo->pStages = stageCreateInfo;
    pipelineCreateInfo->basePipelineIndex = -1;
    pipelineCreateInfo->layout = impl.vk.pipelineLayout;
    RIPipeline slot = {};
    VK_WrapResult(vkCreateGraphicsPipelines(device->vk.device, VK_NULL_HANDLE, 1,
                                            pipelineCreateInfo, NULL,
                                            &slot.vk.handle));

		if( vkSetDebugUtilsObjectNameEXT ) {
			VkDebugUtilsObjectNameInfoEXT debugExt = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot.vk.handle, debugName};
			VK_WrapResult( vkSetDebugUtilsObjectNameEXT( device->vk.device, &debugExt ) );
		}
    pipelineHandle = slot.vk.handle;
    pipeline[pipelineHash] = slot;
    for (size_t i = 0; i < numModules; i++) {
      vkDestroyShaderModule(device->vk.device, modules[i], NULL);
    }
  } else {
    pipelineHandle = it->second.vk.handle;
  }
  vkCmdBindPipeline(cmd->vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineHandle);
}

void RIProgram::bindComputePipeline(struct RIDevice* device, struct RICmd* cmd, hash_t pipelineHash, const char* debugName, VkComputePipelineCreateInfo* pipelineCreateInfo) {
  VkPipeline pipelineHandle = VK_NULL_HANDLE;
  auto it = pipeline.find(pipelineHash);
  if (it == pipeline.end()) {
    assert(shaderBin[PROGRAM_STAGE_COMPUTE].buf.size() > 0 && "no compute shader binary");
    VkShaderModule module = VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo moduleCreateInfo = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        NULL,
        (VkShaderModuleCreateFlags)0,
        (size_t)shaderBin[PROGRAM_STAGE_COMPUTE].buf.size(),
        (const uint32_t*)shaderBin[PROGRAM_STAGE_COMPUTE].buf.data(),
    };
    vkCreateShaderModule(device->vk.device, &moduleCreateInfo, NULL, &module);

    pipelineCreateInfo->stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineCreateInfo->stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineCreateInfo->stage.module = module;
    pipelineCreateInfo->stage.pName = shaderBin[PROGRAM_STAGE_COMPUTE].entryPoint.c_str();
    pipelineCreateInfo->layout = impl.vk.pipelineLayout;
    pipelineCreateInfo->basePipelineIndex = -1;

    RIPipeline slot = {};
    VK_WrapResult(vkCreateComputePipelines(device->vk.device, VK_NULL_HANDLE, 1, pipelineCreateInfo, NULL, &slot.vk.handle));

    if (vkSetDebugUtilsObjectNameEXT) {
      VkDebugUtilsObjectNameInfoEXT debugExt = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL, VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot.vk.handle, debugName };
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &debugExt));
    }
    pipelineHandle = slot.vk.handle;
    pipeline[pipelineHash] = slot;
    vkDestroyShaderModule(device->vk.device, module, NULL);
  } else {
    pipelineHandle = it->second.vk.handle;
  }
  vkCmdBindPipeline(cmd->vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineHandle);
}
#endif // DEVICE_IMPL_VULKAN

void RIProgram::bindRayTracingPipeline(
    struct RIDevice *device, struct RICmd *cmd, hash_t pipelineHash,
    const char *debugName, const RIRayTracingPipelineDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
  VkPipeline pipelineHandle = VK_NULL_HANDLE;
  auto it = rtPipeline.find(pipelineHash);
  if (it == rtPipeline.end()) {
    // Query SBT alignment/size requirements for this physical device.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(
        device->physicalAdapter.vk.physicalDevice, &props2);

    // RT stage table: parallel to the populated shaderBin[] slots.
    static const struct {
      ProgramStages programStage;
      VkShaderStageFlagBits vkStage;
    } kRTStages[] = {
        {PROGRAM_STAGE_RAYGEN, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {PROGRAM_STAGE_MISS, VK_SHADER_STAGE_MISS_BIT_KHR},
        {PROGRAM_STAGE_CLOSEST_HIT, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {PROGRAM_STAGE_ANY_HIT, VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
        {PROGRAM_STAGE_INTERSECTION, VK_SHADER_STAGE_INTERSECTION_BIT_KHR},
        {PROGRAM_STAGE_CALLABLE, VK_SHADER_STAGE_CALLABLE_BIT_KHR},
    };
    constexpr uint32_t RT_STAGE_COUNT = ARRAY_COUNT(kRTStages);

    VkShaderModule modules[RT_STAGE_COUNT] = {};
    VkPipelineShaderStageCreateInfo stages[RT_STAGE_COUNT] = {};
    int32_t stageIdx[RT_STAGE_COUNT];
    uint32_t stageCount = 0;
    for (uint32_t i = 0; i < RT_STAGE_COUNT; i++)
      stageIdx[i] = -1;
    for (uint32_t i = 0; i < RT_STAGE_COUNT; i++) {
      const auto &bin = shaderBin[kRTStages[i].programStage];
      if (bin.buf.empty())
        continue;
      VkShaderModuleCreateInfo modInfo = {
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      modInfo.codeSize = (size_t)bin.buf.size();
      modInfo.pCode = (const uint32_t *)bin.buf.data();
      vkCreateShaderModule(device->vk.device, &modInfo, NULL,
                           &modules[stageCount]);
      stages[stageCount].sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[stageCount].stage = kRTStages[i].vkStage;
      stages[stageCount].module = modules[stageCount];
      stages[stageCount].pName = shaderBin[kRTStages[i].programStage].entryPoint.c_str();
      stageIdx[i] = (int32_t)stageCount;
      stageCount++;
    }
    assert(stageIdx[0] >= 0 &&
           "ray-tracing pipeline requires a raygen shader");

    // Group layout: 1 raygen, 1 miss (optional), 1 hit group (optional),
    // 1 callable (optional). Hit group is triangles if no intersection
    // shader, procedural otherwise.
    VkRayTracingShaderGroupCreateInfoKHR groups[4] = {};
    uint32_t groupCount = 0;
    uint32_t raygenGroupOffset = 0, raygenGroupCount = 0;
    uint32_t missGroupOffset = 0, missGroupCount = 0;
    uint32_t hitGroupOffset = 0, hitGroupCount = 0;
    uint32_t callableGroupOffset = 0, callableGroupCount = 0;

    auto pushGeneral = [&](int32_t shader) {
      groups[groupCount].sType =
          VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
      groups[groupCount].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
      groups[groupCount].generalShader = (uint32_t)shader;
      groups[groupCount].closestHitShader = VK_SHADER_UNUSED_KHR;
      groups[groupCount].anyHitShader = VK_SHADER_UNUSED_KHR;
      groups[groupCount].intersectionShader = VK_SHADER_UNUSED_KHR;
      groupCount++;
    };

    raygenGroupOffset = groupCount;
    pushGeneral(stageIdx[0]);
    raygenGroupCount = 1;

    missGroupOffset = groupCount;
    if (stageIdx[1] >= 0) {
      pushGeneral(stageIdx[1]);
      missGroupCount = 1;
    }

    hitGroupOffset = groupCount;
    if (stageIdx[2] >= 0 || stageIdx[3] >= 0 || stageIdx[4] >= 0) {
      groups[groupCount].sType =
          VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
      groups[groupCount].type =
          (stageIdx[4] >= 0)
              ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
              : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
      groups[groupCount].generalShader = VK_SHADER_UNUSED_KHR;
      groups[groupCount].closestHitShader =
          stageIdx[2] >= 0 ? (uint32_t)stageIdx[2] : VK_SHADER_UNUSED_KHR;
      groups[groupCount].anyHitShader =
          stageIdx[3] >= 0 ? (uint32_t)stageIdx[3] : VK_SHADER_UNUSED_KHR;
      groups[groupCount].intersectionShader =
          stageIdx[4] >= 0 ? (uint32_t)stageIdx[4] : VK_SHADER_UNUSED_KHR;
      groupCount++;
      hitGroupCount = 1;
    }

    callableGroupOffset = groupCount;
    if (stageIdx[5] >= 0) {
      pushGeneral(stageIdx[5]);
      callableGroupCount = 1;
    }

    // Translate the neutral desc; the program fills stages/groups/layout.
    VkRayTracingPipelineCreateInfoKHR createInfo = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    createInfo.flags = desc.flags;
    createInfo.maxPipelineRayRecursionDepth =
        desc.maxRecursionDepth ? desc.maxRecursionDepth : 1;
    createInfo.stageCount = stageCount;
    createInfo.pStages = stages;
    createInfo.groupCount = groupCount;
    createInfo.pGroups = groups;
    createInfo.layout = impl.vk.pipelineLayout;
    createInfo.basePipelineIndex = -1;

    RIRayTracingPipeline slot = {};
    VK_WrapResult(vkCreateRayTracingPipelinesKHR(
        device->vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
        &createInfo, NULL, &slot.vk.handle));

    // SBT: one record per group, each padded to handleAlignment; each
    // region (raygen/miss/hit/callable) starts on a baseAlignment offset.
    auto alignUp = [](VkDeviceSize x, VkDeviceSize a) -> VkDeviceSize {
      return (x + a - 1) & ~(a - 1);
    };
    const VkDeviceSize handleSize = rtProps.shaderGroupHandleSize;
    const VkDeviceSize handleStride =
        alignUp(handleSize, rtProps.shaderGroupHandleAlignment);
    const VkDeviceSize baseAlign = rtProps.shaderGroupBaseAlignment;
    const VkDeviceSize raygenRegionSize =
        alignUp(handleStride * raygenGroupCount, baseAlign);
    const VkDeviceSize missRegionSize =
        alignUp(handleStride * missGroupCount, baseAlign);
    const VkDeviceSize hitRegionSize =
        alignUp(handleStride * hitGroupCount, baseAlign);
    const VkDeviceSize callableRegionSize =
        alignUp(handleStride * callableGroupCount, baseAlign);
    const VkDeviceSize sbtSize =
        raygenRegionSize + missRegionSize + hitRegionSize + callableRegionSize;

    RIBufferDesc sbtDesc = {};
    sbtDesc.size = sbtSize;
    sbtDesc.usage = RI_BUFFER_USAGE_BINDING_TABLE | RI_BUFFER_USAGE_DEVICE_ADDRESS |
                    RI_BUFFER_USAGE_TRANSFER_DST;
    sbtDesc.location = RI_MEMORY_HOST_UPLOAD;
    sbtDesc.alignment = baseAlign;
    RIBuffer sbt = RIBuffer::create(device, sbtDesc);
    // The cache slot owns the raw handles (destroyed via vmaDestroyBuffer at
    // slot eviction); copy them out of the transient RIBuffer wrapper.
    slot.vk.sbtBuffer = sbt.vk.buffer;
    slot.vk.sbtAlloc = sbt.vk.allocation;

    std::vector<uint8_t> handles((size_t)handleSize * groupCount);
    VK_WrapResult(vkGetRayTracingShaderGroupHandlesKHR(
        device->vk.device, slot.vk.handle, 0, groupCount, handles.size(),
        handles.data()));

    auto writeRegion = [&](uint8_t *dst, uint32_t groupOff, uint32_t count) {
      for (uint32_t i = 0; i < count; i++) {
        memcpy(dst + (VkDeviceSize)i * handleStride,
               handles.data() + (size_t)(groupOff + i) * handleSize,
               (size_t)handleSize);
      }
    };
    uint8_t *sbtMapped = (uint8_t *)sbt.mappedAddress;
    writeRegion(sbtMapped + 0, raygenGroupOffset, raygenGroupCount);
    writeRegion(sbtMapped + raygenRegionSize, missGroupOffset, missGroupCount);
    writeRegion(sbtMapped + raygenRegionSize + missRegionSize, hitGroupOffset,
                hitGroupCount);
    writeRegion(sbtMapped + raygenRegionSize + missRegionSize + hitRegionSize,
                callableGroupOffset, callableGroupCount);

    VkBufferDeviceAddressInfo bdaInfo = {
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bdaInfo.buffer = slot.vk.sbtBuffer;
    const VkDeviceAddress sbtBase =
        vkGetBufferDeviceAddress(device->vk.device, &bdaInfo);

    slot.vk.raygenRegion.deviceAddress = sbtBase;
    // raygen region stride MUST equal size per the spec.
    slot.vk.raygenRegion.stride = raygenRegionSize;
    slot.vk.raygenRegion.size = raygenRegionSize;
    slot.vk.missRegion.deviceAddress = sbtBase + raygenRegionSize;
    slot.vk.missRegion.stride = missGroupCount ? handleStride : 0;
    slot.vk.missRegion.size = missRegionSize;
    slot.vk.hitRegion.deviceAddress =
        sbtBase + raygenRegionSize + missRegionSize;
    slot.vk.hitRegion.stride = hitGroupCount ? handleStride : 0;
    slot.vk.hitRegion.size = hitRegionSize;
    slot.vk.callableRegion.deviceAddress =
        sbtBase + raygenRegionSize + missRegionSize + hitRegionSize;
    slot.vk.callableRegion.stride = callableGroupCount ? handleStride : 0;
    slot.vk.callableRegion.size = callableRegionSize;

    if (vkSetDebugUtilsObjectNameEXT) {
      VkDebugUtilsObjectNameInfoEXT debugExt = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_PIPELINE, (uint64_t)slot.vk.handle, debugName};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &debugExt));
    }
    pipelineHandle = slot.vk.handle;
    rtPipeline[pipelineHash] = slot;
    for (uint32_t i = 0; i < stageCount; i++) {
      vkDestroyShaderModule(device->vk.device, modules[i], NULL);
    }
  } else {
    pipelineHandle = it->second.vk.handle;
  }
  vkCmdBindPipeline(cmd->vk.cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                    pipelineHandle);
  return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    // Inline ray-query model: the RT "pipeline" is just a compute pipeline built
    // from the raygen [[kernel]]. The hand-written SurfelRayTrace.rt.metal does
    // scatter + shadow rays with metal::raytracing::intersection_query and the
    // alpha test inline, so no intersection-function table is needed.
    auto it = rtPipeline.find(pipelineHash);
    RIRayTracingPipeline slot = {};
    if (it == rtPipeline.end()) {
      MTL::Device *dev = device->mtl.device;
      MTL::Function *fn = mtlLoadFunction(dev, shaderBin[PROGRAM_STAGE_RAYGEN].buf,
                                          shaderBin[PROGRAM_STAGE_RAYGEN].entryPoint);
      NS::Error *err = nullptr;
      slot.mtl.pipeline = fn ? dev->newComputePipelineState(fn, &err) : nullptr;
      if (!slot.mtl.pipeline && err)
        Error("Metal RT (compute) pipeline '%s' failed: %s\n", debugName,
              err->localizedDescription()->utf8String());
      if (fn) fn->release();
      slot.mtl.intersectionTable = nullptr;
      slot.mtl.numThreads[0] = 64; slot.mtl.numThreads[1] = 1; slot.mtl.numThreads[2] = 1;
      rtPipeline[pipelineHash] = slot;
    } else {
      slot = it->second;
    }
    cmd->mtl_encoderCompute();
    cmd->mtl.compute->setComputePipelineState(slot.mtl.pipeline);
    cmd->mtl.threadsPerThreadgroup[0] = slot.mtl.numThreads[0];
    cmd->mtl.threadsPerThreadgroup[1] = slot.mtl.numThreads[1];
    cmd->mtl.threadsPerThreadgroup[2] = slot.mtl.numThreads[2];
    (void)desc;
    return;
  }
#endif
}

void RIProgram::traceRays(struct RICmd *cmd, hash_t pipelineHash,
                          uint32_t width, uint32_t height, uint32_t depth) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    auto it = rtPipeline.find(pipelineHash);
    assert(it != rtPipeline.end() &&
           "traceRays called before bindRayTracingPipeline");
    const auto &slot = it->second.vk;
    vkCmdTraceRaysKHR(cmd->vk.cmd, &slot.raygenRegion, &slot.missRegion,
                      &slot.hitRegion, &slot.callableRegion, width, height,
                      depth);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    auto it = rtPipeline.find(pipelineHash);
    assert(it != rtPipeline.end() &&
           "traceRays called before bindRayTracingPipeline");
    const auto &slot = it->second.mtl;
    // width == ray count (1-D dispatch); height/depth pass through. The TLAS +
    // bindless/geometry resources must already be resident on the encoder
    // (caller's bindExternalSet + accel-structure residency) before this runs.
    const uint32_t tx = slot.numThreads[0] ? slot.numThreads[0] : 64u;
    MTL::Size tpg = MTL::Size::Make(tx, 1, 1);
    MTL::Size groups = MTL::Size::Make((width + tx - 1u) / tx,
                                       height ? height : 1u, depth ? depth : 1u);
    cmd->mtl.compute->dispatchThreadgroups(groups, tpg);
    return;
  }
#endif
}

// Neutral RIDescriptorType_e -> backend type mappers (defined further below).
#if (DEVICE_IMPL_VULKAN)
static VkDescriptorType ri_vk_BindlessDescriptorType(uint8_t t);
#endif
#if (DEVICE_IMPL_MTL)
static enum RIMTLDescriptorType_e ri_mtl_BindlessDescriptorType(uint8_t t);
#endif

void RIProgram::bindDescriptors(struct RIDevice* device, struct RICmd* cmd, uint32_t frameIndex, DescriptorBinding* bindings, size_t bindingCount, RIPipelineBindPoint_e bindPoint) {
#if ( DEVICE_IMPL_VULKAN )
	{
		const VkPipelineBindPoint vkBind =
			(bindPoint == RI_PIPELINE_BIND_COMPUTE) ? VK_PIPELINE_BIND_POINT_COMPUTE :
			(bindPoint == RI_PIPELINE_BIND_RAY_TRACING) ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR :
			VK_PIPELINE_BIND_POINT_GRAPHICS;
		VkDescriptorSet setsToBind[DESCRIPTOR_SET_MAX] = { VK_NULL_HANDLE };
		uint32_t firstSetToBind = 0;
		uint32_t setsToBindCount = 0;

		for( uint32_t setIndex = 0; setIndex < DESCRIPTOR_SET_MAX; setIndex++ ) {
			// External sets are bound by the caller via bindExternalSet
			// (or vkCmdBindDescriptorSets directly). Skip alloc/write/bind here.
			if( programDescriptors[setIndex].isExternal )
				continue;
			hash_t hash = HASH_INITIAL_VALUE;
			for( size_t i = 0; i < bindingCount; i++ ) {
				const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle );
				if( !refl || setIndex != refl->set || bindings[i].descriptor.isEmpty() )
					continue;
				hash = hash_u64( hash, refl->hash );
				hash = hash_u64( hash, bindings[i].registerOffset );
				assert( bindings[i].descriptor.cookie != 0 );
				hash = hash_u64( hash, bindings[i].descriptor.cookie );
			}
			if( hash == HASH_INITIAL_VALUE )
				continue;
			struct DescriptorSetSlot *info = &programDescriptors[setIndex];
			struct RIDescriptorSetResult result = resolveDescriptorSetAlloc( device, &info->alloc, frameIndex, hash );
			if( !result.found ) {
				size_t numWrites = 0;
				VkWriteDescriptorSet descriptorWrite[32];
				// Acceleration-structure writes need a pNext payload alive across the
				// vkUpdateDescriptorSets call; one entry per descriptorWrite[i] keeps
				// the lifetimes paired through the batch flush.
				VkWriteDescriptorSetAccelerationStructureKHR accelWrites[32] = {};
				// Pure-payload descriptors carry no inline VkDescriptor*Info; build
				// them here from the referenced RI objects (lifetimes span the flush).
				VkDescriptorImageInfo imageInfos[32];
				VkDescriptorBufferInfo bufferInfos[32];
				VkAccelerationStructureKHR accelHandles[32] = {};
				for( size_t i = 0; i < bindingCount; i++ ) {
						const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle );
					if( !refl || setIndex != refl->set || bindings[i].descriptor.isEmpty() )
						continue;

					if( numWrites == ARRAY_COUNT( descriptorWrite ) ) {
						vkUpdateDescriptorSets( device->vk.device, numWrites, descriptorWrite, 0, NULL );
						numWrites = 0;
					}
					VkWriteDescriptorSet *vkDesc = descriptorWrite + ( numWrites++ );
					memset( vkDesc, 0, sizeof( VkWriteDescriptorSet ) );
					vkDesc->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					vkDesc->dstSet = result.set->vk.handle;
					if( refl->isArray ) {
						vkDesc->dstBinding = refl->baseRegisterIndex;
						vkDesc->dstArrayElement = bindings[i].registerOffset;
					} else {
						vkDesc->dstBinding = refl->baseRegisterIndex + bindings[i].registerOffset;
						vkDesc->dstArrayElement = 0;
					}
					vkDesc->descriptorCount = 1;
					const struct RIDescriptor &d = bindings[i].descriptor;
					const size_t w = numWrites - 1;
					vkDesc->descriptorType = ri_vk_BindlessDescriptorType(d.type);
					switch( (enum RIDescriptorType_e)d.type ) {
						case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
						case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
							bufferInfos[w] = VkDescriptorBufferInfo{ d.vkBuffer(), d.offset, d.range };
							vkDesc->pBufferInfo = &bufferInfos[w];
							break;
						case RI_DESCRIPTOR_TYPE_SAMPLER:
						case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
						case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
							imageInfos[w] = VkDescriptorImageInfo{ d.vkSampler(), d.vkImageView(), d.vkLayout() };
							vkDesc->pImageInfo = &imageInfos[w];
							break;
						case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: {
							accelHandles[w] = d.vkAccel();
							VkWriteDescriptorSetAccelerationStructureKHR *aw = &accelWrites[w];
							aw->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
							aw->pNext = NULL;
							aw->accelerationStructureCount = 1;
							aw->pAccelerationStructures = &accelHandles[w];
							vkDesc->pNext = aw;
							break;
						}
						default:
							assert( false );
							break;
					}
				}
				if( numWrites > 0 ) {
					vkUpdateDescriptorSets( device->vk.device, numWrites, descriptorWrite, 0, NULL );
				}
			}

			if( setsToBindCount > 0 && firstSetToBind + setsToBindCount != setIndex ) {
				vkCmdBindDescriptorSets( cmd->vk.cmd, vkBind, impl.vk.pipelineLayout,
					firstSetToBind, setsToBindCount, setsToBind, 0, NULL );
				setsToBindCount = 0;
			}
			if( setsToBindCount == 0 ) {
				firstSetToBind = setIndex;
			}
			setsToBind[setsToBindCount++] = result.set->vk.handle;
		}
		if( setsToBindCount > 0 ) {
			vkCmdBindDescriptorSets( cmd->vk.cmd, vkBind, impl.vk.pipelineLayout,
				firstSetToBind, setsToBindCount, setsToBind, 0, NULL );
		}
	}
#endif
#if ( DEVICE_IMPL_MTL )
	if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
		// Each descriptor set is a Metal argument buffer (escapes the loose ~31
		// buffer-slot limit). Mirror the Vulkan arm: per set, hash the live
		// bindings, resolve a cached slot + its argument buffer, encode on a miss,
		// bind the buffer at buffer(setIndex), and make each resource resident.
		for (uint32_t setIndex = 0; setIndex < DESCRIPTOR_SET_MAX; setIndex++) {
			if (programDescriptors[setIndex].isExternal)
				continue; // bound via bindExternalSet
			struct DescriptorSetSlot *info = &programDescriptors[setIndex];
			if (!info->alloc.mtl.encoder)
				continue; // no program-managed bindings in this set
			hash_t hash = HASH_INITIAL_VALUE;
			for (size_t i = 0; i < bindingCount; i++) {
				const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle);
				if (!refl || setIndex != refl->set || bindings[i].descriptor.isEmpty())
					continue;
				hash = hash_u64(hash, refl->hash);
				hash = hash_u64(hash, bindings[i].registerOffset);
				hash = hash_u64(hash, bindings[i].descriptor.cookie);
			}
			if (hash == HASH_INITIAL_VALUE)
				continue; // no live bindings
			struct RIDescriptorSetResult result = resolveDescriptorSetAlloc(device, &info->alloc, frameIndex, hash);
			MTL::Buffer *argBuf = result.set->mtl.argumentBuffer;
			if (!result.found) {
				MTL::ArgumentEncoder *enc = info->alloc.mtl.encoder;
				enc->setArgumentBuffer(argBuf, 0);
				for (size_t i = 0; i < bindingCount; i++) {
					const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle);
					if (!refl || setIndex != refl->set || bindings[i].descriptor.isEmpty())
						continue;
					const struct RIDescriptor &d = bindings[i].descriptor;
					const uint32_t id = refl->baseRegisterIndex + bindings[i].registerOffset;
					switch (ri_mtl_BindlessDescriptorType(d.type)) {
					case RI_MTL_DESC_SAMPLER:
						enc->setSamplerState(d.mtlSampler(), id);
						break;
					case RI_MTL_DESC_TEXTURE:
						enc->setTexture((MTL::Texture *)d.mtlResource(), id);
						break;
					case RI_MTL_DESC_BUFFER:
						enc->setBuffer((MTL::Buffer *)d.mtlResource(), 0, id);
						break;
					case RI_MTL_DESC_ACCELERATION_STRUCTURE:
						enc->setAccelerationStructure((MTL::AccelerationStructure *)d.mtlResource(), id);
						break;
					default:
						break;
					}
				}
			}
			// Bind the set's argument buffer at buffer(setIndex) for all stages.
			if (cmd->mtl.render) {
				cmd->mtl.render->setVertexBuffer(argBuf, 0, setIndex);
				cmd->mtl.render->setFragmentBuffer(argBuf, 0, setIndex);
			} else if (cmd->mtl.compute) {
				cmd->mtl.compute->setBuffer(argBuf, 0, setIndex);
			}
			// Residency: argument-buffer contents are not auto-resident, and
			// useResource is per-encoder, so run every bind (hit or miss).
			for (size_t i = 0; i < bindingCount; i++) {
				const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle);
				if (!refl || setIndex != refl->set || bindings[i].descriptor.isEmpty())
					continue;
				const struct RIDescriptor &d = bindings[i].descriptor;
				if (d.type == RI_DESCRIPTOR_TYPE_SAMPLER)
					continue;
				MTL::Resource *res = d.mtlResource();
				if (!res)
					continue;
				const bool write = (d.type == RI_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
					                    d.type == RI_DESCRIPTOR_TYPE_STORAGE_BUFFER);
				const MTL::ResourceUsage usage = write ? (MTL::ResourceUsageRead | MTL::ResourceUsageWrite) : MTL::ResourceUsageRead;
				if (cmd->mtl.render)
					cmd->mtl.render->useResource(res, usage);
				else if (cmd->mtl.compute)
					cmd->mtl.compute->useResource(res, usage);
			}
		}
		(void)bindPoint;
	}
#endif
}

void RIProgram::bindExternalSet(struct RICmd *cmd,
                                RIBindlessDescriptorSet *set, uint32_t setIndex,
                                RIPipelineBindPoint_e bindPoint) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    const VkPipelineBindPoint vkBind =
        (bindPoint == RI_PIPELINE_BIND_COMPUTE) ? VK_PIPELINE_BIND_POINT_COMPUTE
        : (bindPoint == RI_PIPELINE_BIND_RAY_TRACING)
            ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
            : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindDescriptorSets(cmd->vk.cmd, vkBind, impl.vk.pipelineLayout,
                            setIndex, 1, &set->vk.m_bindlessSet, 0, NULL);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    // Bind the pre-encoded argument buffer at the set's register space on the
    // active encoder, mirroring the bindDescriptors Metal arm.
    if (cmd->mtl.render) {
      cmd->mtl.render->setVertexBuffer(set->mtl.argumentBuffer, 0, setIndex);
      cmd->mtl.render->setFragmentBuffer(set->mtl.argumentBuffer, 0, setIndex);
    } else if (cmd->mtl.compute) {
      cmd->mtl.compute->setBuffer(set->mtl.argumentBuffer, 0, setIndex);
    }
    // Make the set's resources resident (argument-buffer contents aren't auto-
    // resident). The non-array buffers + single textures are tracked in
    // mtlResident; the bindless texture arrays (gTextures2D/gTexturesCube) live
    // on device->mtl.textureHeap and are covered by one useHeap().
    for (MTL::Resource *res : set->mtlResident) {
      if (cmd->mtl.render)
        cmd->mtl.render->useResource(res, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
      else if (cmd->mtl.compute)
        cmd->mtl.compute->useResource(res, MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
    }
    if (device && device->mtl.textureHeap) {
      if (cmd->mtl.render)
        cmd->mtl.render->useHeap(device->mtl.textureHeap);
      else if (cmd->mtl.compute)
        cmd->mtl.compute->useHeap(device->mtl.textureHeap);
    }
    (void)bindPoint;
    return;
  }
#endif
  (void)cmd;
  (void)set;
  (void)setIndex;
  (void)bindPoint;
}

// RIBindlessDescriptorSet construction is Vulkan-only for now; the Metal
// producer (argument-buffer encode) is deferred with the DXR passes.
// RIBindlessDescriptorSet — Vulkan builds the descriptor-set layout/pool/set;
// Metal builds a persistent argument encoder + Shared argument buffer.
#if (DEVICE_IMPL_VULKAN)
static VkDescriptorType ri_vk_BindlessDescriptorType(uint8_t t) {
  switch ((enum RIDescriptorType_e)t) {
  case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  case RI_DESCRIPTOR_TYPE_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
  case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
    return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  }
  return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}
#endif
#if (DEVICE_IMPL_MTL)
static enum RIMTLDescriptorType_e ri_mtl_BindlessDescriptorType(uint8_t t) {
  switch ((enum RIDescriptorType_e)t) {
  case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
  case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE: return RI_MTL_DESC_TEXTURE;
  case RI_DESCRIPTOR_TYPE_SAMPLER: return RI_MTL_DESC_SAMPLER;
  case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
  case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER: return RI_MTL_DESC_BUFFER;
  case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
    return RI_MTL_DESC_ACCELERATION_STRUCTURE;
  }
  return RI_MTL_DESC_BUFFER;
}

// Metal argument-buffer [[id]] for a set-0 bindless binding. Arrays consume
// consecutive ids, so the three large texture arrays (kBindingTextures2D=0,
// TexturesCube=1, Textures2DArray=2, each kTextureSlotCapacity long) cannot sit
// at their Vulkan binding numbers — they would overlap every other resource
// (buffers/samplers at 3..48). They are relocated above 48; everything else
// keeps id == binding. MUST match the Set0 struct layout in the .metal shaders
// (gTextures2D id(49), gTexturesCube id(16433)).
static uint32_t ri_mtl_BindlessArgId(uint32_t binding) {
  constexpr uint32_t kTexCap = 16384u; // kTextureSlotCapacity (Constants.h)
  switch (binding) {
  case 0:  return 49u;                  // kBindingTextures2D     -> [49 .. 49+cap)
  case 1:  return 49u + kTexCap;        // kBindingTexturesCube   -> [16433 .. )
  case 2:  return 49u + 2u * kTexCap;   // kBindingTextures2DArray
  default: return binding;
  }
}
#endif

void RIBindlessDescriptorSet::initialize(RIDevice *device,
                                         std::span<const Binding> bindings) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    std::vector<VkDescriptorSetLayoutBinding> lbBindings(bindings.size());
    std::vector<VkDescriptorBindingFlags> lbFlags(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
      lbBindings[i].binding = bindings[i].binding;
      lbBindings[i].descriptorType =
          ri_vk_BindlessDescriptorType(bindings[i].descriptorType);
      lbBindings[i].descriptorCount = bindings[i].descriptorCount;
      lbBindings[i].stageFlags = (VkShaderStageFlags)bindings[i].stageFlags;
      lbBindings[i].pImmutableSamplers = nullptr;
      lbFlags[i] = (VkDescriptorBindingFlags)bindings[i].flags;
    }

    // Pool sizes derived from the bindings (accumulate count per Vulkan type).
    std::unordered_map<int, uint32_t> sizeByType;
    for (size_t i = 0; i < bindings.size(); ++i)
      sizeByType[(int)ri_vk_BindlessDescriptorType(bindings[i].descriptorType)] +=
          bindings[i].descriptorCount;
    std::vector<VkDescriptorPoolSize> poolSizes;
    poolSizes.reserve(sizeByType.size());
    for (auto &kv : sizeByType)
      poolSizes.push_back(
          VkDescriptorPoolSize{(VkDescriptorType)kv.first, kv.second});

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = (uint32_t)bindings.size();
    flagsInfo.pBindingFlags = lbFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = (uint32_t)lbBindings.size();
    layoutInfo.pBindings = lbBindings.data();
    layoutInfo.flags =
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext = &flagsInfo;
    VK_WrapResult(vkCreateDescriptorSetLayout(device->vk.device, &layoutInfo,
                                              NULL, &vk.m_bindlessSetLayout));

    VkDescriptorPoolCreateInfo poolInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
    poolInfo.pPoolSizes = poolSizes.data();
    VK_WrapResult(vkCreateDescriptorPool(device->vk.device, &poolInfo, NULL,
                                         &vk.m_bindlessPool));

    VkDescriptorSetAllocateInfo setAlloc = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAlloc.descriptorPool = vk.m_bindlessPool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &vk.m_bindlessSetLayout;
    VK_WrapResult(vkAllocateDescriptorSets(device->vk.device, &setAlloc,
                                           &vk.m_bindlessSet));
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    MTL::Device *dev = device->mtl.device;
    std::vector<MTL::ArgumentDescriptor *> descs;
    descs.reserve(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
      MTL::ArgumentDescriptor *ad = MTL::ArgumentDescriptor::alloc()->init();
      ad->setIndex(ri_mtl_BindlessArgId(bindings[i].binding));
      ad->setArrayLength(
          bindings[i].descriptorCount > 1 ? bindings[i].descriptorCount : 1);
      ad->setAccess(MTL::BindingAccessReadWrite);
      ad->setDataType(RIMTLDescriptorDataType(
          ri_mtl_BindlessDescriptorType(bindings[i].descriptorType)));
      descs.push_back(ad);
    }
    NS::Array *argDescs = NS::Array::array(
        (const NS::Object *const *)descs.data(), descs.size());
    mtl.encoder = dev->newArgumentEncoder(argDescs);
    mtl.argumentBuffer = dev->newBuffer(mtl.encoder->encodedLength(),
                                        MTL::ResourceStorageModeShared);
    mtl.encoder->setArgumentBuffer(mtl.argumentBuffer, 0);
    for (auto *ad : descs)
      ad->release();
    return;
  }
#endif
}

void RIBindlessDescriptorSet::writeDescriptors(
    RIDevice *device, std::span<const WriteBinding> writes) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    if (writes.empty())
      return;

    std::vector<VkWriteDescriptorSet> vkWrites(writes.size());
    // Acceleration-structure writes need a pNext-chained struct that must
    // outlive the vkUpdateDescriptorSets call. Sized for the worst case so
    // pointers into the vector stay stable across reallocs.
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelWrites(
        writes.size());
    // Pure-payload descriptors: build the VkDescriptor*Info from the referenced
    // RI objects; the vectors keep the pointers stable across the update call.
    std::vector<VkDescriptorImageInfo> imageInfos(writes.size());
    std::vector<VkDescriptorBufferInfo> bufferInfos(writes.size());
    std::vector<VkAccelerationStructureKHR> accelHandles(writes.size());
    for (size_t i = 0; i < writes.size(); ++i) {
      const WriteBinding &w = writes[i];
      const struct RIDescriptor &d = w.descriptor;
      VkWriteDescriptorSet &vkDesc = vkWrites[i];
      vkDesc = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      vkDesc.dstSet = vk.m_bindlessSet;
      vkDesc.dstBinding = w.binding;
      vkDesc.dstArrayElement = w.arrayElement;
      vkDesc.descriptorCount = 1;
      vkDesc.descriptorType = ri_vk_BindlessDescriptorType(d.type);
      switch ((enum RIDescriptorType_e)d.type) {
      case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        bufferInfos[i] = VkDescriptorBufferInfo{ d.vkBuffer(), d.offset, d.range };
        vkDesc.pBufferInfo = &bufferInfos[i];
        break;
      case RI_DESCRIPTOR_TYPE_SAMPLER:
      case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        imageInfos[i] = VkDescriptorImageInfo{ d.vkSampler(), d.vkImageView(), d.vkLayout() };
        vkDesc.pImageInfo = &imageInfos[i];
        break;
      case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: {
        accelHandles[i] = d.vkAccel();
        VkWriteDescriptorSetAccelerationStructureKHR &aw = accelWrites[i];
        aw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        aw.accelerationStructureCount = 1;
        aw.pAccelerationStructures = &accelHandles[i];
        vkDesc.pNext = &aw;
        break;
      }
      default:
        assert(false && "RIBindlessDescriptorSet::writeDescriptors: "
                        "unsupported descriptor type");
        break;
      }
    }
    vkUpdateDescriptorSets(device->vk.device, (uint32_t)vkWrites.size(),
                           vkWrites.data(), 0, NULL);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    if (writes.empty() || !mtl.encoder)
      return;
    // Re-target the persistent encoder at the set's argument buffer, then encode
    // each resource at index (binding + arrayElement), mirroring Vulkan's
    // dstBinding/dstArrayElement addressing.
    mtl.encoder->setArgumentBuffer(mtl.argumentBuffer, 0);
    for (size_t i = 0; i < writes.size(); ++i) {
      const WriteBinding &w = writes[i];
      const struct RIDescriptor &d = w.descriptor;
      const uint32_t idx = ri_mtl_BindlessArgId(w.binding) + w.arrayElement;
      // Pull the backend handle from the referenced RI object.
      MTL::Resource *res = d.mtlResource();
      switch (ri_mtl_BindlessDescriptorType(d.type)) {
      case RI_MTL_DESC_SAMPLER:
        mtl.encoder->setSamplerState(d.mtlSampler(), idx);
        break;
      case RI_MTL_DESC_TEXTURE:
        mtl.encoder->setTexture((MTL::Texture *)res, idx);
        break;
      case RI_MTL_DESC_BUFFER:
        mtl.encoder->setBuffer((MTL::Buffer *)res, 0, idx);
        break;
      default:
        break;
      }
      // Track non-array resources for bindExternalSet residency (skip samplers
      // and the big texture arrays at binding 0/1/2 — those need a heap).
      if (res && w.binding > 2 &&
          ri_mtl_BindlessDescriptorType(d.type) != RI_MTL_DESC_SAMPLER) {
        bool present = false;
        for (MTL::Resource *r : mtlResident)
          if (r == res) { present = true; break; }
        if (!present)
          mtlResident.push_back(res);
      }
    }
    return;
  }
#endif
}

void RIBindlessDescriptorSet::destroy(RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    if (vk.m_bindlessPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device->vk.device, vk.m_bindlessPool, NULL);
      vk.m_bindlessPool = VK_NULL_HANDLE;
      vk.m_bindlessSet = VK_NULL_HANDLE;
    }
    if (vk.m_bindlessSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device->vk.device, vk.m_bindlessSetLayout,
                                   NULL);
      vk.m_bindlessSetLayout = VK_NULL_HANDLE;
    }
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    if (mtl.encoder) {
      mtl.encoder->release();
      mtl.encoder = nullptr;
    }
    if (mtl.argumentBuffer) {
      mtl.argumentBuffer->release();
      mtl.argumentBuffer = nullptr;
    }
    return;
  }
#endif
}

const struct RIProgram::BindingReflection* RIProgram::findReflection(const struct DescriptorBindingID& handle) {
  for(auto& ref : bindingReflection) {
    if(ref.hash == handle.hash) {
      return &ref;
    }
  }
  return NULL;
}

std::vector<char> RIProgram::loadShaderStage(cFileSearcher *searcher, const tString& asName) {
  std::vector<char> result = {};
	tString sLookup = asName;
#if (DEVICE_IMPL_MTL)
	// Call sites request the reflection-convention `*.spv` name, but the Metal
	// backend executes the textual `*.metal` emitted alongside it (compiled at
	// runtime via newLibrary). Rewrite a trailing `.spv` to `.metal` so every
	// load site stays backend-neutral.
	if (sLookup.size() >= 4 &&
	    sLookup.compare(sLookup.size() - 4, 4, ".spv") == 0) {
		sLookup.replace(sLookup.size() - 4, 4, ".metal");
	}
#endif
	tWString sPath = searcher->GetFilePath(sLookup);
	if(sPath==_W("")){
		FatalError("Couldn't find file '%s' in resources!\n", asName.c_str());
		return result;
	}
	unsigned int fileSize = cPlatform::GetFileSize(sPath);
	result.resize(fileSize);
	cPlatform::CopyFileToBuffer(sPath,result.data(),fileSize);
  return result;
}

#if (DEVICE_IMPL_VULKAN)
void RIProgram::initialize(RIDevice* device, const RIProgramDescriptor &desc) {
  assert(device);
  this->device = device;

  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings[DESCRIPTOR_SET_MAX] ;
  std::vector<VkDescriptorBindingFlags> descriptorBindingFlags[DESCRIPTOR_SET_MAX];
  VkDescriptorSetLayout setLayouts[DESCRIPTOR_SET_MAX] = {0};
  VkPushConstantRange pushConstantRange = {0};

  auto externalLayoutFor = [&](size_t setIndex) -> VkDescriptorSetLayout {
    if (setIndex < desc.externalSets.size() && desc.externalSets[setIndex])
      return desc.externalSets[setIndex]->vk.m_bindlessSetLayout;
    return VK_NULL_HANDLE;
  };

  // Stash each stage's binary + entry point.
  for (auto &init : desc.stages) {
    auto *bin = &shaderBin[init.stage];
    bin->buf.insert(bin->buf.begin(), init.data.begin(), init.data.end());
    if (init.entryPoint && *init.entryPoint)
      bin->entryPoint = init.entryPoint;
  }

  // Push constant (one block). RIShaderStageBits_e mirrors VkShaderStageFlagBits,
  // so the stage mask casts straight through.
  if (desc.pushConstantSize > 0) {
    pushConstantRange.stageFlags = (VkShaderStageFlags)desc.pushConstantStages;
    pushConstantRange.size = desc.pushConstantSize;
    pushConstantRange.offset = 0;
    impl.vk.pushConstant.shaderStageFlags = (VkShaderStageFlags)desc.pushConstantStages;
    impl.vk.pushConstant.size = desc.pushConstantSize;
    hasPushConstant = true;
  }

  // Descriptor bindings from the unified table. Each entry's `stages` mask
  // (== VkShaderStageFlagBits) is OR'd straight into the layout binding; a
  // resource shared across stages is one entry. Populates the per-set layout
  // bindings + max-count buckets and the name->{set,binding} reflection that
  // bindDescriptors consumes.
  for (const RIProgramBinding &vb : desc.bindings) {
    assert(vb.vk.set < programDescriptors.size());
    const uint32_t bindingCount = std::max<uint32_t>(vb.count, 1);
    struct DescriptorSetSlot *program_desc = &programDescriptors[vb.vk.set];
    program_desc->alloc.descriptor_alloc_handle = vkDescriptorSetAlloc;
    program_desc->alloc.framesInFlight = RI_NUMBER_FRAMES_FLIGHT;

    DescriptorBindingID id = DescriptorBindingID::Create(vb.name);
    struct RIProgram::BindingReflection *reflc = NULL;
    for (auto &ref : bindingReflection)
      if (ref.hash == id.hash) { reflc = &ref; break; }
    if (reflc == NULL) {
      bindingReflection.push_back({});
      reflc = &bindingReflection.back();
    }
    reflc->hash = id.hash;
    reflc->set = vb.vk.set;
    reflc->baseRegisterIndex = vb.vk.binding;
    reflc->isArray = vb.count > 1;
    reflc->dimCount = std::max<uint16_t>(1, (uint16_t)bindingCount);

    VkDescriptorSetLayoutBinding *layoutBinding = NULL;
    VkDescriptorBindingFlags *bindingFlags = NULL;
    for (size_t i = 0; i < descriptorSetLayoutBindings[vb.vk.set].size(); i++) {
      if (descriptorSetLayoutBindings[vb.vk.set][i].binding == vb.vk.binding) {
        layoutBinding = &descriptorSetLayoutBindings[vb.vk.set][i];
        bindingFlags = &descriptorBindingFlags[vb.vk.set][i];
      }
    }
    if (!layoutBinding) {
      descriptorSetLayoutBindings[vb.vk.set].push_back({0});
      descriptorBindingFlags[vb.vk.set].push_back(0);
      layoutBinding = &descriptorSetLayoutBindings[vb.vk.set].back();
      bindingFlags = &descriptorBindingFlags[vb.vk.set].back();
    }
    if (reflc->isArray)
      (*bindingFlags) = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    layoutBinding->binding = vb.vk.binding;
    layoutBinding->descriptorCount = bindingCount;
    layoutBinding->stageFlags |= (VkShaderStageFlags)vb.stages;
    switch ((RIDescriptorType_e)vb.type) {
    case RI_DESCRIPTOR_TYPE_SAMPLER:
      layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      program_desc->samplerMaxNum += bindingCount;
      break;
    case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      program_desc->textureMaxNum += bindingCount;
      break;
    case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      program_desc->storageTextureMaxNum += bindingCount;
      break;
    case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      program_desc->constantBufferMaxNum += bindingCount;
      break;
    case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      program_desc->structuredBufferMaxNum += bindingCount;
      break;
    case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE:
      layoutBinding->descriptorType =
          VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      program_desc->accelerationStructureMaxNum += bindingCount;
      break;
    default:
      assert(false && "unhandled RIDescriptorType_e in RIProgramBinding");
      break;
    }
  }
	  uint32_t numLayoutCount = 0;
	  for( size_t bindingIdx = 0; bindingIdx < DESCRIPTOR_SET_MAX; bindingIdx++ ) {
		  if( descriptorSetLayoutBindings[bindingIdx].size() > 0 ||
		      externalLayoutFor(bindingIdx) != VK_NULL_HANDLE ) {
			  numLayoutCount = bindingIdx + 1;
		  }
	  }

		for( size_t bindingIdx = 0; bindingIdx < numLayoutCount; bindingIdx++ ) {
			VkDescriptorSetLayout external = externalLayoutFor(bindingIdx);
			if( external != VK_NULL_HANDLE ) {
				// Caller-owned layout. Use it directly; mark the slot external
				// so bindDescriptors skips alloc/write/bind for this set.
				setLayouts[bindingIdx] = external;
				programDescriptors[bindingIdx].isExternal = true;
			} else if(descriptorSetLayoutBindings[bindingIdx].size() > 0 ) {
				VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
				bindingFlagsInfo.bindingCount = descriptorBindingFlags[bindingIdx].size();
				bindingFlagsInfo.pBindingFlags = descriptorBindingFlags[bindingIdx].data();

				VkDescriptorSetLayoutCreateInfo createSetLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
				createSetLayoutInfo.bindingCount = descriptorSetLayoutBindings[bindingIdx].size();
				createSetLayoutInfo.pBindings = descriptorSetLayoutBindings[bindingIdx].data();
				R_VK_ADD_STRUCT( &createSetLayoutInfo, &bindingFlagsInfo );

				VK_WrapResult( vkCreateDescriptorSetLayout( device->vk.device, &createSetLayoutInfo, NULL, setLayouts + bindingIdx ) );
			} else {
				VkDescriptorSetLayoutCreateInfo createSetLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
				VK_WrapResult( vkCreateDescriptorSetLayout( device->vk.device, &createSetLayoutInfo, NULL, setLayouts + bindingIdx ) );
			}
			programDescriptors[bindingIdx].vk.setLayout = setLayouts[bindingIdx];
		}
		pipelineLayoutCreateInfo.pSetLayouts = setLayouts;
		pipelineLayoutCreateInfo.setLayoutCount = numLayoutCount;
		if( pushConstantRange.stageFlags > 0 ) {
			pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
			pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
		}
		VK_WrapResult( vkCreatePipelineLayout( device->vk.device, &pipelineLayoutCreateInfo, NULL, &impl.vk.pipelineLayout ) );
}
#endif // DEVICE_IMPL_VULKAN (initialize)

#if (DEVICE_IMPL_MTL)
// RIShaderStageBits_e bit -> ProgramStages index (the mtlArgByStage row).
static int ri_mtl_stage_index(uint32_t bit) {
  switch (bit) {
  case RI_SHADER_STAGE_VERTEX:      return RIProgram::PROGRAM_STAGE_VERTEX;
  case RI_SHADER_STAGE_FRAGMENT:    return RIProgram::PROGRAM_STAGE_FRAGMENT;
  case RI_SHADER_STAGE_COMPUTE:     return RIProgram::PROGRAM_STAGE_COMPUTE;
  case RI_SHADER_STAGE_RAYGEN:      return RIProgram::PROGRAM_STAGE_RAYGEN;
  case RI_SHADER_STAGE_ANY_HIT:     return RIProgram::PROGRAM_STAGE_ANY_HIT;
  case RI_SHADER_STAGE_CLOSEST_HIT: return RIProgram::PROGRAM_STAGE_CLOSEST_HIT;
  case RI_SHADER_STAGE_MISS:        return RIProgram::PROGRAM_STAGE_MISS;
  default:                          return -1;
  }
}
// ProgramStages index -> RIShaderStageBits_e bit (inverse of the table above),
// used to test a module stage against RIProgramDescriptor::pushConstantStages.
static uint32_t ri_stage_bit(RIProgram::ProgramStages s) {
  switch (s) {
  case RIProgram::PROGRAM_STAGE_VERTEX:      return RI_SHADER_STAGE_VERTEX;
  case RIProgram::PROGRAM_STAGE_FRAGMENT:    return RI_SHADER_STAGE_FRAGMENT;
  case RIProgram::PROGRAM_STAGE_COMPUTE:     return RI_SHADER_STAGE_COMPUTE;
  case RIProgram::PROGRAM_STAGE_RAYGEN:      return RI_SHADER_STAGE_RAYGEN;
  case RIProgram::PROGRAM_STAGE_MISS:        return RI_SHADER_STAGE_MISS;
  case RIProgram::PROGRAM_STAGE_CLOSEST_HIT: return RI_SHADER_STAGE_CLOSEST_HIT;
  case RIProgram::PROGRAM_STAGE_ANY_HIT:     return RI_SHADER_STAGE_ANY_HIT;
  default:                                   return 0;
  }
}
// Metal binding kind derived from the logical descriptor type.
static RIProgram::RIMtlBindingKind ri_mtl_kind(uint8_t type) {
  switch ((RIDescriptorType_e)type) {
  case RI_DESCRIPTOR_TYPE_SAMPLER:                return RIProgram::RI_MTL_BIND_SAMPLER;
  case RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
  case RI_DESCRIPTOR_TYPE_STORAGE_IMAGE:          return RIProgram::RI_MTL_BIND_TEXTURE;
  case RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE: return RIProgram::RI_MTL_BIND_ACCEL;
  case RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
  case RI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
  default:                                        return RIProgram::RI_MTL_BIND_BUFFER;
  }
}

// Slot create-handle for a program-managed set: reserve a batch of slots, each
// owning a Shared argument buffer sized by the set's encoder. Mirrors
// vkDescriptorSetAlloc (which allocates VkDescriptorSets from a pool).
static void mtlDescriptorSetAlloc(struct RIDevice *device,
                                  struct RIDescriptorSetAlloc *alloc) {
  MTL::Device *dev = device->mtl.device;
  for (size_t i = 0; i < DESCRIPTOR_MAX_SIZE; i++) {
    struct RIDescriptorSetSlot *slot = allocDescriptorSetSlot(alloc);
    slot->mtl.argumentBuffer = dev->newBuffer(
        alloc->mtl.encodedLength, MTL::ResourceStorageModeShared);
    arrpush(alloc->reservedSlots, slot);
  }
}

void RIProgram::initialize(RIDevice *device, const RIProgramDescriptor &desc) {
  assert(device);
  this->device = device;
  // External sets: no pipeline-layout / descriptor-set-layout objects on Metal.
  // Just mark the slot external so bindDescriptors skips alloc/write/bind for it
  // (the caller binds the external argument buffer at the set index via
  // bindExternalSet). Positional by set index; nullptr = program-managed slot.
  for (size_t setIndex = 0;
       setIndex < desc.externalSets.size() && setIndex < DESCRIPTOR_SET_MAX;
       setIndex++) {
    if (desc.externalSets[setIndex])
      programDescriptors[setIndex].isExternal = true;
  }
  // Execution: stash the MSL blob + entry-point so bindPipeline can compile it
  // via newLibrary at pipeline-creation time.
  for (auto &init : desc.stages) {
    auto *bin = &shaderBin[init.stage];
    bin->buf.insert(bin->buf.begin(), init.data.begin(), init.data.end());
    if (init.entryPoint && *init.entryPoint)
      bin->entryPoint = init.entryPoint;
  }

  // Binding map: name hash -> {Metal [[*(N)]] index, kind} per stage. The index
  // is the per-stage flat slot slangc assigned (read off the generated .metal);
  // kind is derived from the logical type.
  for (const RIProgramBinding &b : desc.bindings) {
    if (b.mtl.index == RI_MTL_NONE)
      continue; // VK-only resource (slangc dropped it from the MSL kernel)
    const hash_t h = DescriptorBindingID::Create(b.name).hash;
    const uint8_t kind = (uint8_t)ri_mtl_kind(b.type);
    for (uint32_t bit = 1; bit; bit <<= 1) {
      if (!(b.stages & bit))
        continue;
      const int s = ri_mtl_stage_index(bit);
      if (s < 0)
        continue;
      mtlArgByStage[s][h] = MtlArg{b.mtl.index, kind};
    }
  }
  // Per-set argument buffers. A whole descriptor set is one Metal argument
  // buffer (escaping the loose ~31-slot limit); bindDescriptors encodes each
  // set's resources into it. The reflection carries the argument id (mtl.index)
  // within the set's buffer; build one MTL::ArgumentEncoder per program-managed
  // set and point its slot allocator at mtlDescriptorSetAlloc.
  for (const RIProgramBinding &b : desc.bindings) {
    if (b.mtl.index == RI_MTL_NONE)
      continue;
    const hash_t h = DescriptorBindingID::Create(b.name).hash;
    BindingReflection *refl = nullptr;
    for (auto &r : bindingReflection)
      if (r.hash == h) { refl = &r; break; }
    if (!refl) {
      bindingReflection.push_back({});
      refl = &bindingReflection.back();
    }
    refl->hash = h;
    refl->set = b.vk.set;
    refl->baseRegisterIndex = b.mtl.index; // argument id within the set's buffer
    refl->isArray = b.count > 1;
    refl->dimCount = std::max<uint16_t>(1, b.count ? b.count : 1);
  }
  {
    MTL::Device *dev = device->mtl.device;
    for (uint32_t s = 0; s < DESCRIPTOR_SET_MAX; s++) {
      if (programDescriptors[s].isExternal)
        continue;
      std::vector<MTL::ArgumentDescriptor *> descs;
      for (const RIProgramBinding &b : desc.bindings) {
        if (b.vk.set != s || b.mtl.index == RI_MTL_NONE)
          continue;
        MTL::ArgumentDescriptor *ad = MTL::ArgumentDescriptor::alloc()->init();
        ad->setIndex(b.mtl.index);
        ad->setArrayLength(b.count > 1 ? b.count : 1);
        ad->setAccess(MTL::BindingAccessReadWrite);
        ad->setDataType(
            RIMTLDescriptorDataType(ri_mtl_BindlessDescriptorType(b.type)));
        descs.push_back(ad);
      }
      if (descs.empty())
        continue;
      NS::Array *argDescs = NS::Array::array(
          (const NS::Object *const *)descs.data(), descs.size());
      RIDescriptorSetAlloc &alloc = programDescriptors[s].alloc;
      alloc.mtl.encoder = dev->newArgumentEncoder(argDescs);
      alloc.mtl.encodedLength = alloc.mtl.encoder->encodedLength();
      alloc.descriptor_alloc_handle = mtlDescriptorSetAlloc;
      alloc.framesInFlight = RI_NUMBER_FRAMES_FLIGHT;
      for (auto *ad : descs)
        ad->release();
    }
  }
  // Push constant: each module stage that participates in the push-constant
  // stage mask contributes its own Metal [[buffer(N)]] slot (slangc can assign
  // the PC a different index per stage).
  if (desc.pushConstantSize > 0) {
    for (const ModuleStage &m : desc.stages) {
      if (desc.pushConstantStages & ri_stage_bit((ProgramStages)m.stage))
        mtlPushConstantIndex[m.stage] = (int16_t)m.pushConstantMtlIndex;
    }
  }
}
#endif

} // namespace hpl
