#include "graphics/RIProgram.h"
#include "system/Platform.h"

#include <cassert>
#include <system/Types.h>
#include <system/stb_ds.h>

#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIRenderer.h"
#include "graphics/spirv_reflect.h"

namespace hpl {
static void vkDescriptorSetAlloc( struct RIDevice_s *device, struct RIDescriptorSetAlloc *alloc ) {
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

void RIProgram::bindPipeline(struct RIDevice_s *device, struct RICmd_s* cmd, hash_t pipelineHash, const char* debugName, VkGraphicsPipelineCreateInfo* pipelineCreateInfo) {
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
    PipelineSlot slot = {};
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

void RIProgram::bindComputePipeline(struct RIDevice_s* device, struct RICmd_s* cmd, hash_t pipelineHash, const char* debugName, VkComputePipelineCreateInfo* pipelineCreateInfo) {
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

    PipelineSlot slot = {};
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

void RIProgram::bindRayTracingPipeline(
    struct RIDevice_s *device, struct RICmd_s *cmd, hash_t pipelineHash,
    const char *debugName,
    VkRayTracingPipelineCreateInfoKHR *pipelineCreateInfo) {
#if (DEVICE_IMPL_VULKAN)
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

    pipelineCreateInfo->sType =
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineCreateInfo->stageCount = stageCount;
    pipelineCreateInfo->pStages = stages;
    pipelineCreateInfo->groupCount = groupCount;
    pipelineCreateInfo->pGroups = groups;
    pipelineCreateInfo->layout = impl.vk.pipelineLayout;
    if (pipelineCreateInfo->maxPipelineRayRecursionDepth == 0)
      pipelineCreateInfo->maxPipelineRayRecursionDepth = 1;
    pipelineCreateInfo->basePipelineIndex = -1;

    RTPipelineSlot slot = {};
    VK_WrapResult(vkCreateRayTracingPipelinesKHR(
        device->vk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
        pipelineCreateInfo, NULL, &slot.vk.handle));

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

    VkBufferCreateInfo sbtBufInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    sbtBufInfo.size = sbtSize;
    sbtBufInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    sbtBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo sbtAllocInfo = {};
    sbtAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    sbtAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VmaAllocationInfo sbtAllocOut = {};
    VK_WrapResult(vmaCreateBufferWithAlignment(device->vk.vmaAllocator, &sbtBufInfo,
                                  &sbtAllocInfo, baseAlign, &slot.vk.sbtBuffer,
                                  &slot.vk.sbtAlloc, &sbtAllocOut));

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
    uint8_t *sbtMapped = (uint8_t *)sbtAllocOut.pMappedData;
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
#endif
}

void RIProgram::traceRays(struct RICmd_s *cmd, hash_t pipelineHash,
                          uint32_t width, uint32_t height, uint32_t depth) {
#if (DEVICE_IMPL_VULKAN)
  auto it = rtPipeline.find(pipelineHash);
  assert(it != rtPipeline.end() &&
         "traceRays called before bindRayTracingPipeline");
  const auto &slot = it->second.vk;
  vkCmdTraceRaysKHR(cmd->vk.cmd, &slot.raygenRegion, &slot.missRegion,
                    &slot.hitRegion, &slot.callableRegion, width, height,
                    depth);
#endif
}

void RIProgram::bindDescriptors(struct RIDevice_s* device, struct RICmd_s* cmd, uint32_t frameIndex, DescriptorBinding* bindings, size_t bindingCount, VkPipelineBindPoint bindPoint) {
#if ( DEVICE_IMPL_VULKAN )
	{
		VkDescriptorSet setsToBind[DESCRIPTOR_SET_MAX] = { VK_NULL_HANDLE };
		uint32_t firstSetToBind = 0;
		uint32_t setsToBindCount = 0;

		for( uint32_t setIndex = 0; setIndex < DESCRIPTOR_SET_MAX; setIndex++ ) {
			// External sets are bound by the caller via bindBindlessDescriptorSet
			// (or vkCmdBindDescriptorSets directly). Skip alloc/write/bind here.
			if( programDescriptors[setIndex].isExternal )
				continue;
			hash_t hash = HASH_INITIAL_VALUE;
			for( size_t i = 0; i < bindingCount; i++ ) {
				const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle );
				if( !refl || setIndex != refl->set || RI_IsEmptyDescriptor( &bindings[i].descriptor ) )
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
				for( size_t i = 0; i < bindingCount; i++ ) {
						const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle );
					if( !refl || setIndex != refl->set || RI_IsEmptyDescriptor( &bindings[i].descriptor ) )
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
					vkDesc->descriptorType = bindings[i].descriptor.vk.type;
					switch( bindings[i].descriptor.vk.type ) {
						case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
						case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
							vkDesc->pBufferInfo = &bindings[i].descriptor.vk.buffer;
							break;
						case VK_DESCRIPTOR_TYPE_SAMPLER:
						case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
						case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
						case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
							vkDesc->pImageInfo = &bindings[i].descriptor.vk.image;
							break;
						case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
							VkWriteDescriptorSetAccelerationStructureKHR *aw = &accelWrites[numWrites - 1];
							aw->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
							aw->pNext = NULL;
							aw->accelerationStructureCount = 1;
							aw->pAccelerationStructures = &bindings[i].descriptor.vk.accelStructure;
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
				vkCmdBindDescriptorSets( cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout,
					firstSetToBind, setsToBindCount, setsToBind, 0, NULL );
				setsToBindCount = 0;
			}
			if( setsToBindCount == 0 ) {
				firstSetToBind = setIndex;
			}
			setsToBind[setsToBindCount++] = result.set->vk.handle;
		}
		if( setsToBindCount > 0 ) {
			vkCmdBindDescriptorSets( cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout,
				firstSetToBind, setsToBindCount, setsToBind, 0, NULL );
		}
	}
#endif
}

void RIProgram::bindBindlessDescriptorSet(struct RICmd_s *cmd,
                                          RIBindlessDescriptorSet *bindless,
                                          uint32_t setIndex,
                                          VkPipelineBindPoint bindPoint) {
#if (DEVICE_IMPL_VULKAN)
  vkCmdBindDescriptorSets(cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout,
                          setIndex, 1, &bindless->vk.m_bindlessSet, 0, NULL);
#endif
}

void RIBindlessDescriptorSet::initialize(
    RIDevice_s *device, std::span<const Binding> bindings,
    std::span<const VkDescriptorPoolSize> poolSizes) {
  std::vector<VkDescriptorSetLayoutBinding> lbBindings(bindings.size());
  std::vector<VkDescriptorBindingFlags> lbFlags(bindings.size());

  for (size_t i = 0; i < bindings.size(); ++i) {
    lbBindings[i].binding = bindings[i].binding;
    lbBindings[i].descriptorType = bindings[i].descriptorType;
    lbBindings[i].descriptorCount = bindings[i].descriptorCount;
    lbBindings[i].stageFlags = bindings[i].stageFlags;
    lbBindings[i].pImmutableSamplers = nullptr;
    lbFlags[i] = bindings[i].flags;
  }

  VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  flagsInfo.bindingCount = (uint32_t)bindings.size();
  flagsInfo.pBindingFlags = lbFlags.data();

  VkDescriptorSetLayoutCreateInfo layoutInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layoutInfo.bindingCount = (uint32_t)lbBindings.size();
  layoutInfo.pBindings = lbBindings.data();
  layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layoutInfo.pNext = &flagsInfo;
  VK_WrapResult(vkCreateDescriptorSetLayout(device->vk.device, &layoutInfo, NULL,
                                            &vk.m_bindlessSetLayout));

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
  VK_WrapResult(
      vkAllocateDescriptorSets(device->vk.device, &setAlloc, &vk.m_bindlessSet));
}

void RIBindlessDescriptorSet::writeDescriptors(
    RIDevice_s *device, std::span<const WriteBinding> writes) {
#if (DEVICE_IMPL_VULKAN)
  if (writes.empty())
    return;

  std::vector<VkWriteDescriptorSet> vkWrites(writes.size());
  // Acceleration-structure writes need a pNext-chained struct that must
  // outlive the vkUpdateDescriptorSets call. Sized for the worst case so
  // pointers into the vector stay stable across reallocs.
  std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelWrites(
      writes.size());
  for (size_t i = 0; i < writes.size(); ++i) {
    const WriteBinding &w = writes[i];
    VkWriteDescriptorSet &vkDesc = vkWrites[i];
    vkDesc = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    vkDesc.dstSet = vk.m_bindlessSet;
    vkDesc.dstBinding = w.binding;
    vkDesc.dstArrayElement = w.arrayElement;
    vkDesc.descriptorCount = 1;
    vkDesc.descriptorType = w.descriptor.vk.type;
    switch (w.descriptor.vk.type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      vkDesc.pBufferInfo = &w.descriptor.vk.buffer;
      break;
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      vkDesc.pImageInfo = &w.descriptor.vk.image;
      break;
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: {
      VkWriteDescriptorSetAccelerationStructureKHR &aw = accelWrites[i];
      aw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
      aw.accelerationStructureCount = 1;
      aw.pAccelerationStructures = &w.descriptor.vk.accelStructure;
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
	tWString sPath = searcher->GetFilePath(asName);
	if(sPath==_W("")){
		FatalError("Couldn't find file '%s' in resources!\n", asName.c_str());
		return result;
	}
	unsigned int fileSize = cPlatform::GetFileSize(sPath);
	result.resize(fileSize);
	cPlatform::CopyFileToBuffer(sPath,result.data(),fileSize);
  return result;
}

void RIProgram::initialize(RIDevice_s* device, std::span<ModuleStage> moduleInit,
                           std::span<const VkDescriptorSetLayout> externalLayouts) {
  assert(device);
  this->device = device;

  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings[DESCRIPTOR_SET_MAX] ;
  std::vector<VkDescriptorBindingFlags> descriptorBindingFlags[DESCRIPTOR_SET_MAX];
  VkDescriptorSetLayout setLayouts[DESCRIPTOR_SET_MAX] = {0};
  VkPushConstantRange pushConstantRange = {0};
  std::vector<SpvReflectDescriptorSet *> reflectionDescSets;

  auto externalLayoutFor = [&](size_t setIndex) -> VkDescriptorSetLayout {
    if (setIndex < externalLayouts.size())
      return externalLayouts[setIndex];
    return VK_NULL_HANDLE;
  };

  for (auto &init : moduleInit) {
    auto *bin = &shaderBin[init.stage];
    bin->buf.insert(bin->buf.begin(), init.data.begin(), init.data.end());
    if (init.entryPoint && *init.entryPoint)
      bin->entryPoint = init.entryPoint;
    SpvReflectShaderModule module = {};
    SpvReflectResult result = spvReflectCreateShaderModule(
        init.data.size(), init.data.data(), &module);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    {
      uint32_t pushConstantCount = 0;
      result = spvReflectEnumeratePushConstantBlocks(&module,
                                                     &pushConstantCount, NULL);
      assert(result == SPV_REFLECT_RESULT_SUCCESS);
      if (pushConstantCount > 1) {
        FatalError("RIProgram: stage %u declares %u push constant blocks; only 1 supported per stage!\n", init.stage, pushConstantCount);
        assert(false && "multiple push-constant blocks in a single stage");
      } else if (pushConstantCount == 1) {
        SpvReflectBlockVariable *blocks[1] = {nullptr};
        result = spvReflectEnumeratePushConstantBlocks(
            &module, &pushConstantCount, blocks);
        assert(result == SPV_REFLECT_RESULT_SUCCESS);

        VkShaderStageFlags stageBit = 0;
        switch (init.stage) {
        case PROGRAM_STAGE_VERTEX:       stageBit = VK_SHADER_STAGE_VERTEX_BIT;          break;
        case PROGRAM_STAGE_FRAGMENT:     stageBit = VK_SHADER_STAGE_FRAGMENT_BIT;        break;
        case PROGRAM_STAGE_COMPUTE:      stageBit = VK_SHADER_STAGE_COMPUTE_BIT;         break;
        case PROGRAM_STAGE_RAYGEN:       stageBit = VK_SHADER_STAGE_RAYGEN_BIT_KHR;      break;
        case PROGRAM_STAGE_MISS:         stageBit = VK_SHADER_STAGE_MISS_BIT_KHR;        break;
        case PROGRAM_STAGE_CLOSEST_HIT:  stageBit = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR; break;
        case PROGRAM_STAGE_ANY_HIT:      stageBit = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;     break;
        case PROGRAM_STAGE_INTERSECTION: stageBit = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;break;
        case PROGRAM_STAGE_CALLABLE:     stageBit = VK_SHADER_STAGE_CALLABLE_BIT_KHR;    break;
        default: assert(false); break;
        }

        pushConstantRange.stageFlags |= stageBit;
        pushConstantRange.size = std::max(pushConstantRange.size, blocks[0]->size);
        pushConstantRange.offset = 0;
        impl.vk.pushConstant.shaderStageFlags |= stageBit;
        impl.vk.pushConstant.size = std::max<uint32_t>(impl.vk.pushConstant.size, blocks[0]->size);
        hasPushConstant = true;
      }
    }

    if (init.stage == PROGRAM_STAGE_VERTEX) {
      for (size_t i = 0; i < module.input_variable_count; i++) {
        const uint32_t location = module.input_variables[i]->location;
        if (location >= vertex_input_format.size()) continue;
        vertex_input_mask |= (1u << location);
        vertex_input_format[location] = (uint32_t)module.input_variables[i]->format;
      }
    }

    uint32_t reflectionDescriptorCount = 0;
    result = spvReflectEnumerateDescriptorSets(
        &module, &reflectionDescriptorCount, NULL);
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    reflectionDescSets.resize(reflectionDescriptorCount);
    result = spvReflectEnumerateDescriptorSets(
        &module, &reflectionDescriptorCount, reflectionDescSets.data());
    assert(result == SPV_REFLECT_RESULT_SUCCESS);
    for (size_t i_set = 0; i_set < reflectionDescriptorCount; i_set++) {
      const SpvReflectDescriptorSet *spv_reflection = reflectionDescSets[i_set];
      assert(spv_reflection->set < programDescriptors.size());
      struct DescriptorSetSlot *program_desc =
          &programDescriptors[spv_reflection->set];
      program_desc->alloc.descriptor_alloc_handle = vkDescriptorSetAlloc;
      program_desc->alloc.framesInFlight = RI_NUMBER_FRAMES_FLIGHT;
      for (size_t i_binding = 0; i_binding < spv_reflection->binding_count; i_binding++) {
        const SpvReflectDescriptorBinding *reflectionBinding =
            spv_reflection->bindings[i_binding];
        assert(reflectionBinding->array.dims_count <=
               1); // not going to handle multi-dim arrays
        DescriptorBindingID reflID = CreateDescriptorBindingID(reflectionBinding->name);
        struct RIProgram::BindingReflection* reflc = NULL;
        for(auto& ref : bindingReflection) {
          if(ref.hash == reflID.hash) {
            reflc = &ref;
            break;
          }
        }
        if(reflc == NULL) {
          bindingReflection.push_back({});
          reflc = &bindingReflection.back();
        }

        reflc->hash = reflID.hash;
        reflc->set = reflectionBinding->set;
        reflc->baseRegisterIndex = reflectionBinding->binding;
        reflc->isArray = reflectionBinding->count > 1;
        reflc->dimCount = std::max<uint16_t>(1, reflectionBinding->count);
        Log("[MP] Descriptor[%zu], name: %s hash: %llu stage: %u\n", i_set, reflectionBinding->name ? reflectionBinding->name : "<null>", (unsigned long long)reflc->hash, (unsigned)init.stage);

        VkDescriptorSetLayoutBinding *layoutBinding = NULL;
        VkDescriptorBindingFlags *bindingFlags = NULL;
        for (size_t i = 0; i < descriptorSetLayoutBindings[spv_reflection->set].size(); i++) {
          if (descriptorSetLayoutBindings[spv_reflection->set][i].binding == reflectionBinding->binding) {
            layoutBinding = &descriptorSetLayoutBindings[spv_reflection->set][i];
            bindingFlags = &descriptorBindingFlags[spv_reflection->set][i];
          }
        }

        if (!layoutBinding) {
          VkDescriptorSetLayoutBinding bindings = {0};
          VkDescriptorBindingFlags flags = 0;
          descriptorSetLayoutBindings[spv_reflection->set].push_back(bindings);
          descriptorBindingFlags[spv_reflection->set].push_back(flags);
          layoutBinding = &descriptorSetLayoutBindings[spv_reflection->set][descriptorSetLayoutBindings[spv_reflection->set].size() - 1];
          bindingFlags = &descriptorBindingFlags[spv_reflection->set][descriptorBindingFlags[spv_reflection->set].size() - 1];
        }

        if (reflc->isArray) {
          (*bindingFlags) = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        }

        const uint32_t bindingCount =
            std::max<uint32_t>(reflectionBinding->count, 1);
        layoutBinding->binding = reflectionBinding->binding;
        layoutBinding->descriptorCount = bindingCount;
        switch (init.stage) {
        case PROGRAM_STAGE_VERTEX:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
          break;
        case PROGRAM_STAGE_FRAGMENT:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
          break;
        case PROGRAM_STAGE_COMPUTE:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
          break;
        case PROGRAM_STAGE_RAYGEN:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
          break;
        case PROGRAM_STAGE_MISS:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_MISS_BIT_KHR;
          break;
        case PROGRAM_STAGE_CLOSEST_HIT:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
          break;
        case PROGRAM_STAGE_ANY_HIT:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
          break;
        case PROGRAM_STAGE_INTERSECTION:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
          break;
        case PROGRAM_STAGE_CALLABLE:
          layoutBinding->stageFlags |= VK_SHADER_STAGE_CALLABLE_BIT_KHR;
          break;
        default:
          assert(false);
          break;
        }
        switch (reflectionBinding->descriptor_type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
          program_desc->samplerMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          program_desc->textureMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
          program_desc->bufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          program_desc->storageTextureMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
          program_desc->storageBufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          program_desc->constantBufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          program_desc->structuredBufferMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          layoutBinding->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          program_desc->combinedImageSamplerMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
          layoutBinding->descriptorType =
              VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
          program_desc->accelerationStructureMaxNum += bindingCount;
          break;
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
          assert(false);
          break;
        }
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
}

} // namespace hpl
