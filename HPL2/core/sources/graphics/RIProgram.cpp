#include "graphics/RIProgram.h"
#include "system/Platform.h"

#include <cassert>
#include <system/Types.h>
#include <system/stb_ds.h>

#include "graphics/RIRenderer.h"
#include "graphics/spirv_reflect.h"

namespace hpl {
static void vkDescriptorSetAlloc( struct RIDevice_s *device, struct RIDescriptorSetAlloc *alloc ) {
	assert( device->renderer->api == RI_DEVICE_API_VK );
	struct RIProgram::DescriptorSetSlot *programDescriptor = hpl_container_of( alloc, &RIProgram::DescriptorSetSlot::alloc );
  VkDescriptorPoolSize descriptorPoolSize[16] = {};
  size_t descriptorPoolLen = 0;
  if( programDescriptor->samplerMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_SAMPLER, (uint32_t)programDescriptor->samplerMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->constantBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)programDescriptor->constantBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->dynamicConstantBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, (uint32_t)programDescriptor->dynamicConstantBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->textureMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (uint32_t)programDescriptor->textureMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->storageTextureMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (uint32_t)programDescriptor->storageTextureMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->bufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, (uint32_t)programDescriptor->bufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->storageBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, (uint32_t)programDescriptor->storageBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->structuredBufferMaxNum > 0 || programDescriptor->storageStructuredBufferMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){
  		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)programDescriptor->structuredBufferMaxNum * DESCRIPTOR_MAX_SIZE + (uint32_t)programDescriptor->storageStructuredBufferMaxNum * DESCRIPTOR_MAX_SIZE };
  if( programDescriptor->accelerationStructureMaxNum > 0 )
  	descriptorPoolSize[descriptorPoolLen++] = (VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, (uint32_t)programDescriptor->accelerationStructureMaxNum * DESCRIPTOR_MAX_SIZE };
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
      stageCreateInfo[0] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stageCreateInfo[0].stage = VK_SHADER_STAGE_VERTEX_BIT,
      stageCreateInfo[0].module = modules[numModules],
      stageCreateInfo[0].pName = "main";
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
      stageCreateInfo[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      stageCreateInfo[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stageCreateInfo[1].module = modules[numModules];
      stageCreateInfo[1].pName = "main";
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

    pipelineCreateInfo->stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineCreateInfo->stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineCreateInfo->stage.module = module;
    pipelineCreateInfo->stage.pName = "main";
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

void RIProgram::bindDescriptors(struct RIDevice_s* device, struct RICmd_s* cmd, uint32_t frameIndex, DescriptorBinding* bindings, size_t bindingCount, VkPipelineBindPoint bindPoint) {
#if ( DEVICE_IMPL_VULKAN )
	{
		size_t numWrites = 0;
		VkWriteDescriptorSet descriptorWrite[32]; // write 32 descriptors at once
		for( uint32_t setIndex = 0; setIndex < DESCRIPTOR_SET_MAX; setIndex++ ) {
			hash_t hash = HASH_INITIAL_VALUE;
			for( size_t i = 0; i < bindingCount; i++ ) {
				const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle );
				if( !refl || setIndex != refl->set || RI_IsEmptyDescriptor( &bindings[i].descriptor ) )
					continue;
				hash = hash_u64( hash, refl->hash );
				hash = hash_u64( hash, bindings[i].descriptor.cookie );
			}
			if( hash == HASH_INITIAL_VALUE )
				continue;
			struct DescriptorSetSlot *info = &programDescriptors[setIndex];
			struct RIDescriptorSetResult result = resolveDescriptorSetAlloc( device, &info->alloc, frameIndex, hash );
			if( !result.found ) {
				for( size_t i = 0; i < bindingCount; i++ ) {
						const struct RIProgram::BindingReflection *refl = findReflection(bindings[i].handle );
					if( !refl || setIndex != refl->set || RI_IsEmptyDescriptor( &bindings[i].descriptor ) )
						continue;

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
							vkDesc->pImageInfo = &bindings[i].descriptor.vk.image;
							break;
						default:
							assert( false ); // this is bad
							break;
					}

					if( numWrites >= ARRAY_COUNT( descriptorWrite ) ) {
						vkUpdateDescriptorSets( device->vk.device, numWrites, descriptorWrite, 0, NULL );
						numWrites = 0;
					}
				}
			}
			VkDescriptorSet vkDescriptorSet = result.set->vk.handle;
			vkCmdBindDescriptorSets( cmd->vk.cmd, bindPoint, impl.vk.pipelineLayout, setIndex, 1, &vkDescriptorSet, 0, NULL );
		}
		if( numWrites > 0 ) {
			vkUpdateDescriptorSets( device->vk.device, numWrites, descriptorWrite, 0, NULL );
		}
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
	tWString sPath = searcher->GetFilePath(asName);
	if(sPath==_W("")){
		printf("Couldn't find file '%s' in resources\n",asName.c_str());
		return result;
	}
	unsigned int fileSize = cPlatform::GetFileSize(sPath);
	result.resize(fileSize);
	cPlatform::CopyFileToBuffer(sPath,result.data(),fileSize);
  return result;
}

void RIProgram::initialize(RIDevice_s* device,std::span<ModuleStage> moduleInit) {
  assert(device);
  this->device = device;

  VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings[DESCRIPTOR_SET_MAX] ;
  std::vector<VkDescriptorBindingFlags> descriptorBindingFlags[DESCRIPTOR_SET_MAX];
  VkDescriptorSetLayout setLayouts[DESCRIPTOR_SET_MAX] = {0};
  VkPushConstantRange pushConstantRange = {0};
  std::vector<SpvReflectDescriptorSet *> reflectionDescSets;

  for (auto &init : moduleInit) {
    auto *bin = &shaderBin[init.stage];
    bin->buf.insert(bin->buf.begin(), init.data.begin(), init.data.end());
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
        printf("RIProgram: stage %u declares %u push constant blocks; only 1 supported per stage\n",
               init.stage, pushConstantCount);
        assert(false && "multiple push-constant blocks in a single stage");
      } else if (pushConstantCount == 1) {
        SpvReflectBlockVariable *blocks[1] = {nullptr};
        result = spvReflectEnumeratePushConstantBlocks(
            &module, &pushConstantCount, blocks);
        assert(result == SPV_REFLECT_RESULT_SUCCESS);

        VkShaderStageFlags stageBit = 0;
        switch (init.stage) {
        case PROGRAM_STAGE_VERTEX:   stageBit = VK_SHADER_STAGE_VERTEX_BIT;   break;
        case PROGRAM_STAGE_FRAGMENT: stageBit = VK_SHADER_STAGE_FRAGMENT_BIT; break;
        case PROGRAM_STAGE_COMPUTE:  stageBit = VK_SHADER_STAGE_COMPUTE_BIT;  break;
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
        printf("[MP] Descriptor[%lu], name: %s hash: %lu stage: %u\n", i_set, reflectionBinding->name, reflc->hash, init.stage);

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
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          assert(false);
          break;
        }
      }
    }
	  uint32_t numLayoutCount = 0;
	  for( size_t bindingIdx = 0; bindingIdx < DESCRIPTOR_SET_MAX; bindingIdx++ ) {
		  if( descriptorSetLayoutBindings[bindingIdx].size() > 0) {
			  numLayoutCount = bindingIdx + 1;
		  }
	  }
		
		for( size_t bindingIdx = 0; bindingIdx < numLayoutCount; bindingIdx++ ) {
			if(descriptorSetLayoutBindings[bindingIdx].size() > 0 ) {
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
